#!/usr/bin/env python3
"""
pcap_to_yaml.py - heuristic usbmon capture -> usbvcr YAML project converter.

Reads a Linux usbmon pcap/pcapng capture (the same framing src/lib/pcap/pcap.c
reads and writes - see docs/yaml_dialect.md) and writes a best-effort device
project: devices/, flows/, and, when the HID report descriptor decompiles
cleanly, codecs/. Anything the heuristics can't confidently turn into
symbolic dialect fields falls back to a raw payload file, so the emitted
project still reproduces the capture byte-for-byte.

Heuristics applied, in order of how much they assume:
  - Device/config descriptors: GET_DESCRIPTOR replies are parsed by their
    fixed USB layout (always correct, no guessing).
  - HID report descriptors: walked generically by (type, tag) against a
    built-in copy of the "hid_report" codec table (same one used by
    tests/data/example1/codecs/hid_report.yaml). Falls back to a raw payload
    if an item uses a tag/type this table doesn't know.
  - Flow rules: one "operational" state. GET_DESCRIPTOR/SET_IDLE/
    SET_CONFIGURATION are recognized by (bRequest, bmRequestType); anything
    else just mirrors whether the capture showed a reply or a stall.
    Interrupt IN completions after SET_CONFIGURATION are queued verbatim
    (this assumes - as in example1 - that the device starts streaming right
    after enumeration, not through further state transitions).
  - This only models a single configuration/interface and does not attempt
    to verify endpoint OUT traffic; it only replays what the device already
    sent.

Usage:
    python3 scripts/pcap_to_yaml.py CAPTURE.pcapng OUTPUT_DIR \\
        [--name NAME] [--vendor VENDOR] [--product PRODUCT]
"""

import argparse
import pathlib
import struct
import sys

try:
    import yaml
except ImportError:
    sys.exit("error: PyYAML is required (pip install pyyaml)")


# ---------------------------------------------------------------------------
# usbmon framing (mirrors src/lib/pcap/pcap.h / pcap.c)
# ---------------------------------------------------------------------------

USBMON_HDR_FMT = "<QccBBHccqiiII8siiII"
USBMON_HDR_SIZE = struct.calcsize(USBMON_HDR_FMT)
assert USBMON_HDR_SIZE == 64

XFER_ISO, XFER_INT, XFER_CONTROL, XFER_BULK = range(4)

PCAPNG_SHB = 0x0A0D0D0A
PCAPNG_EPB = 0x00000006
PCAP_MAGICS = (0xA1B2C3D4, 0xD4C3B2A1, 0xA1B23C4D, 0x4D3CB2A1)


class Transfer:
    def __init__(self, ts_usec, urb_id, bus, device, endpoint, direction_in,
                xfer, event_type, setup, data):
        self.ts_usec = ts_usec
        self.urb_id = urb_id
        self.bus = bus
        self.device = device
        self.endpoint = endpoint
        self.direction_in = direction_in
        self.xfer = xfer
        self.event_type = event_type
        self.setup = setup          # 8 raw bytes, or None
        self.data = data            # captured payload bytes


def _decode_record(raw: bytes) -> Transfer:
    (urb_id, typ, xfer_type, epnum, devnum, busnum, flag_setup, _flag_data,
     ts_sec, ts_usec, _status, _length, len_cap, setup,
     _interval, _start_frame, _xfer_flags, _ndesc) = struct.unpack_from(USBMON_HDR_FMT, raw, 0)

    return Transfer(
        ts_usec=ts_sec * 1_000_000 + ts_usec,
        urb_id=urb_id,
        bus=busnum,
        device=devnum,
        endpoint=epnum,
        direction_in=bool(epnum & 0x80),
        xfer=xfer_type[0],
        event_type=typ.decode(),
        setup=setup if flag_setup == b"\x00" else None,
        data=raw[USBMON_HDR_SIZE:USBMON_HDR_SIZE + len_cap],
    )


def _read_legacy_pcap(data: bytes) -> list:
    transfers = []
    off = 24  # pcap_header_t
    while off + 16 <= len(data):
        _, _, incl_len, _ = struct.unpack_from("<IIII", data, off)
        off += 16
        rec = data[off:off + incl_len]
        off += incl_len
        if len(rec) >= USBMON_HDR_SIZE:
            transfers.append(_decode_record(rec))
    return transfers


def _read_pcapng(data: bytes) -> list:
    transfers = []
    off = 0
    while off + 12 <= len(data):
        block_type, block_len = struct.unpack_from("<II", data, off)
        if block_len < 12 or off + block_len > len(data):
            break
        if block_type == PCAPNG_EPB:
            _, _, _, cap_len, _ = struct.unpack_from("<IIIII", data, off + 8)
            rec = data[off + 28:off + 28 + cap_len]
            if len(rec) >= USBMON_HDR_SIZE:
                transfers.append(_decode_record(rec))
        # Anything else (Section Header, Interface Description, Interface
        # Statistics, Name Resolution, ...) is skipped - we only care about
        # the Enhanced Packet Blocks.
        off += block_len
    return transfers


def read_usbmon_trace(path) -> list:
    data = pathlib.Path(path).read_bytes()
    if len(data) < 4:
        raise ValueError(f"{path}: file too small to be a capture")
    magic, = struct.unpack_from("<I", data, 0)
    if magic == PCAPNG_SHB:
        return _read_pcapng(data)
    if magic in PCAP_MAGICS:
        return _read_legacy_pcap(data)
    raise ValueError(f"{path}: not a recognized pcap/pcapng file (magic={magic:#x})")


def pair_control_transactions(transfers) -> list:
    """Submit/Complete pairs for Control transfers, in Complete order."""
    pending = {}
    pairs = []
    for t in transfers:
        if t.xfer != XFER_CONTROL:
            continue
        if t.event_type == "S":
            pending[t.urb_id] = t
        elif t.event_type == "C":
            pairs.append((pending.pop(t.urb_id, None), t))
    return pairs


def decode_setup(raw8: bytes) -> dict:
    bmRequestType, bRequest, wValue, wIndex, wLength = struct.unpack("<BBHHH", raw8)
    return {"bmRequestType": bmRequestType, "bRequest": bRequest,
            "wValue": wValue, "wIndex": wIndex, "wLength": wLength}


# ---------------------------------------------------------------------------
# Hex rendering - match the dialect's own convention (hex for ids/types/
# bitmasks, plain decimal for lengths/counts) instead of PyYAML's default of
# dumping everything as decimal.
# ---------------------------------------------------------------------------

class Hex(int):
    def __new__(cls, value, width=2):
        obj = int.__new__(cls, value)
        obj.width = width
        return obj


def _represent_hex(dumper, data):
    return dumper.represent_scalar("tag:yaml.org,2002:int", f"0x{int(data):0{data.width}x}")


yaml.add_representer(Hex, _represent_hex)


def write_yaml(path: pathlib.Path, doc: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w") as f:
        yaml.dump(doc, f, sort_keys=False, default_flow_style=False)


# ---------------------------------------------------------------------------
# Descriptor parsing (fixed USB layout, no guessing)
# ---------------------------------------------------------------------------

def parse_device_descriptor(raw: bytes) -> dict:
    (bLength, bDescriptorType, bcdUSB, bDeviceClass, bDeviceSubClass,
     bDeviceProtocol, bMaxPacketSize0, idVendor, idProduct, bcdDevice,
     iManufacturer, iProduct, iSerialNumber, bNumConfigurations) = \
        struct.unpack_from("<BBHBBBBHHHBBBB", raw, 0)
    return {
        "bLength": bLength,
        "bDescriptorType": Hex(bDescriptorType),
        "bcdUSB": Hex(bcdUSB, 4),
        "bDeviceClass": Hex(bDeviceClass),
        "bDeviceSubClass": Hex(bDeviceSubClass),
        "bDeviceProtocol": Hex(bDeviceProtocol),
        "bMaxPacketSize0": bMaxPacketSize0,
        "idVendor": Hex(idVendor, 4),
        "idProduct": Hex(idProduct, 4),
        "bcdDevice": Hex(bcdDevice, 4),
        "iManufacturer": iManufacturer,
        "iProduct": iProduct,
        "iSerialNumber": iSerialNumber,
        "bNumConfigurations": bNumConfigurations,
    }


def parse_config_descriptor(raw: bytes) -> dict:
    (bLength, bDescriptorType, wTotalLength, bNumInterfaces,
     bConfigurationValue, iConfiguration, bmAttributes, bMaxPower) = \
        struct.unpack_from("<BBHBBBBB", raw, 0)
    config = {
        "bLength": bLength,
        "bDescriptorType": Hex(bDescriptorType),
        "wTotalLength": wTotalLength,
        "bNumInterfaces": bNumInterfaces,
        "bConfigurationValue": bConfigurationValue,
        "iConfiguration": iConfiguration,
        "bmAttributes": Hex(bmAttributes),
        "bMaxPower": bMaxPower,
        "interfaces": [],
    }

    off = bLength
    current_iface = None
    end = min(len(raw), wTotalLength)
    while off + 2 <= end:
        sub_len, sub_type = struct.unpack_from("<BB", raw, off)
        if sub_len == 0 or off + sub_len > end:
            break
        body = raw[off:off + sub_len]

        if sub_type == 0x04 and sub_len >= 9:  # INTERFACE
            (_, _, bInterfaceNumber, bAlternateSetting, bNumEndpoints,
             bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol,
             iInterface) = struct.unpack_from("<BBBBBBBBB", body, 0)
            current_iface = {
                "bInterfaceNumber": bInterfaceNumber,
                "bAlternateSetting": bAlternateSetting,
                "bNumEndpoints": bNumEndpoints,
                "bInterfaceClass": Hex(bInterfaceClass),
                "bInterfaceSubClass": Hex(bInterfaceSubClass),
                "bInterfaceProtocol": Hex(bInterfaceProtocol),
                "iInterface": iInterface,
                "endpoints": [],
            }
            config["interfaces"].append(current_iface)

        elif sub_type == 0x21 and sub_len >= 9 and current_iface is not None:  # HID
            (_, _, bcdHID, bCountryCode, bNumDescriptors,
             bReportDescriptorType, wDescriptorLength) = \
                struct.unpack_from("<BBHBBBH", body, 0)
            current_iface["hid_descriptor"] = {
                "bLength": sub_len,
                "bDescriptorType": Hex(sub_type),
                "bcdHID": Hex(bcdHID, 4),
                "bCountryCode": bCountryCode,
                "bNumDescriptors": bNumDescriptors,
                "bReportDescriptorType": Hex(bReportDescriptorType),
                "wDescriptorLength": wDescriptorLength,
            }

        elif sub_type == 0x05 and sub_len >= 7 and current_iface is not None:  # ENDPOINT
            (_, _, bEndpointAddress, ep_bmAttributes, wMaxPacketSize,
             bInterval) = struct.unpack_from("<BBBBHB", body, 0)
            current_iface["endpoints"].append({
                "bEndpointAddress": Hex(bEndpointAddress),
                "bmAttributes": Hex(ep_bmAttributes),
                "wMaxPacketSize": wMaxPacketSize,
                "bInterval": bInterval,
            })

        off += sub_len

    return config


# ---------------------------------------------------------------------------
# HID report descriptor decompiler, driven by a codec table in the same
# shape as codecs/*.yaml (tokens/symbols/flagsets). Ships a copy of
# tests/data/example1/codecs/hid_report.yaml as the default table; pass a
# richer one (loaded from an existing project) to recognize more tokens.
# ---------------------------------------------------------------------------

DEFAULT_HID_CODEC = {
    "version": "1.0",
    "codec": "hid_report",
    "item_types": {"main": 0, "global": 1, "local": 2},
    "tokens": {
        "usage_page":      {"type": "global", "tag": 0x0, "width": "auto"},
        "logical_minimum": {"type": "global", "tag": 0x1, "width": "auto", "signed": True},
        "logical_maximum": {"type": "global", "tag": 0x2, "width": "auto", "signed": True},
        "report_size":     {"type": "global", "tag": 0x7, "width": "auto"},
        "report_count":    {"type": "global", "tag": 0x9, "width": "auto"},
        "usage":           {"type": "local", "tag": 0x0, "width": "auto"},
        "usage_minimum":   {"type": "local", "tag": 0x1, "width": "auto"},
        "usage_maximum":   {"type": "local", "tag": 0x2, "width": "auto"},
        "collection":      {"type": "main", "tag": 0xA, "width": 1},
        "end_collection":  {"type": "main", "tag": 0xC, "width": 0},
        "input":           {"type": "main", "tag": 0x8, "width": 1, "flagset": "io"},
        "output":          {"type": "main", "tag": 0x9, "width": 1, "flagset": "io"},
    },
    "symbols": {
        "usage_page": {"Generic Desktop": 0x01, "Key Codes": 0x07},
        "usage": {"Keyboard": 0x06},
        "collection": {"Application": 0x01, "Physical": 0x00},
    },
    "flagsets": {
        "io": {"Data": 0, "Const": 1, "Array": 0, "Var": 2, "Abs": 0, "Rel": 4},
    },
}


def _decode_flagset(flagset: dict, value: int) -> str:
    """Pair up consecutive (zero-bit name, nonzero-bit name) entries - this
    is how codecs/*.yaml always lists them (e.g. Data:0 next to Const:1) -
    and pick whichever side of each pair the bit says."""
    names = list(flagset.items())
    picked = []
    for i in range(0, len(names) - 1, 2):
        (a_name, a_bit), (b_name, b_bit) = names[i], names[i + 1]
        if a_bit == 0:
            picked.append(b_name if (value & b_bit) == b_bit else a_name)
        else:
            picked.append(a_name if (value & a_bit) == a_bit else b_name)
    return ",".join(picked)


def decompile_hid_report(raw: bytes, codec: dict):
    """Returns (items, ok). ok is False the moment a short item uses a
    (type, tag) this codec table doesn't define - callers should fall back
    to a raw payload rather than emit a guess."""
    item_types = codec["item_types"]
    tokens = codec["tokens"]
    symbols = codec.get("symbols", {})
    flagsets = codec.get("flagsets", {})

    by_type_tag = {}
    for name, spec in tokens.items():
        by_type_tag[(item_types[spec["type"]], spec["tag"])] = (name, spec)

    items = []
    off = 0
    while off < len(raw):
        prefix = raw[off]
        off += 1
        size = {0: 0, 1: 1, 2: 2, 3: 4}[prefix & 0x03]
        item_type = (prefix >> 2) & 0x03
        tag = (prefix >> 4) & 0x0F
        if item_type == 3:  # long item - not used by real HID report descriptors
            return items, False

        operand = raw[off:off + size]
        off += size
        if len(operand) < size:
            return items, False
        value = int.from_bytes(operand, "little")

        match = by_type_tag.get((item_type, tag))
        if match is None:
            return items, False
        name, spec = match

        if "flagset" in spec:
            items.append({name: _decode_flagset(flagsets.get(spec["flagset"], {}), value)})
        elif spec.get("width") == 0:
            items.append({name: True})
        elif name in symbols:
            reverse = {v: k for k, v in symbols[name].items()}
            items.append({name: reverse.get(value, value)})
        else:
            items.append({name: value})

    return items, True


# ---------------------------------------------------------------------------
# Flow synthesis
# ---------------------------------------------------------------------------

def build_flow(control_pairs, interrupt_payloads_by_endpoint, report_raw_file=None) -> dict:
    rules = []
    seen = set()

    def add_rule(match, actions):
        key = tuple(sorted((k, int(v)) for k, v in match.items()))
        if key in seen:
            return
        seen.add(key)
        rules.append({"match": match, "actions": actions})

    for submit, complete in control_pairs:
        if submit is None or submit.setup is None:
            continue
        setup = decode_setup(submit.setup)
        bmRequestType, bRequest, wValue, wIndex = (
            setup["bmRequestType"], setup["bRequest"], setup["wValue"], setup["wIndex"])
        reply = complete.data if complete else b""

        if bRequest == 0x06:  # GET_DESCRIPTOR
            match = {"bmRequestType": Hex(bmRequestType), "bRequest": bRequest, "wValue": Hex(wValue, 4)}
            desc_type = wValue >> 8
            if not reply:
                add_rule(match, [{"respond_with_status": 1}])  # observed stall
            elif desc_type == 0x01:
                add_rule(match, [{"respond_with": "$device.device_descriptor"}])
            elif desc_type == 0x02:
                add_rule(match, [{"respond_with": "$device.configurations[0]"}])
            elif desc_type == 0x22:
                match["wIndex"] = wIndex
                if report_raw_file:
                    add_rule(match, [{"respond_raw": {"file": report_raw_file}}])
                else:
                    add_rule(match, [{"respond_with": "$device.report_descriptor"}])
            else:
                add_rule(match, [{"respond_with_status": 1}])
            continue

        if bmRequestType == 0x00 and bRequest == 0x09:  # SET_CONFIGURATION
            match = {"bmRequestType": Hex(bmRequestType), "bRequest": bRequest, "wValue": wValue & 0xFF}
            actions = [{"respond_with_status": 0}]
            if interrupt_payloads_by_endpoint:
                ep, payloads = next(iter(interrupt_payloads_by_endpoint.items()))
                actions.append({
                    "queue_endpoint_data": {
                        "endpoint": Hex(ep),
                        "payloads": [{"hex": h} for h in payloads],
                    },
                })
            add_rule(match, actions)
            continue

        if bmRequestType == 0x21 and bRequest == 0x0A:  # SET_IDLE (HID class)
            match = {"bmRequestType": Hex(bmRequestType), "bRequest": bRequest}
            add_rule(match, [{"respond_with_status": 0}])
            continue

        # Unrecognized request: mirror whether the capture ACKed or stalled it.
        match = {"bmRequestType": Hex(bmRequestType), "bRequest": bRequest, "wValue": Hex(wValue, 4)}
        add_rule(match, [{"respond_with_status": 0 if complete is not None else 1}])

    for ep in interrupt_payloads_by_endpoint:
        rules.append({
            "match": {"endpoint_poll": Hex(ep)},
            "actions": [{"return_nak_if_queue_empty": True}],
        })

    return {"version": "1.0", "states": [{"name": "operational", "rules": rules}]}


def build_device(name: str, device_desc: dict, config: dict, report_items) -> dict:
    device = {
        "version": "1.0",
        "name": name,
        "device_descriptor": device_desc,
        "configurations": [config],
    }
    if report_items is not None:
        device["report_descriptor"] = {"codec": "hid_report", "items": report_items}
    return device


def build_project(name: str, vendor: str, product: str, slug: str) -> dict:
    return {
        "version": "1.0",
        "name": name,
        "vars": [{"vendor": vendor}, {"product": product}],
        "devices": [{
            "device": f"devices/{slug}.yaml",
            "flow": f"flows/{slug}_flow.yaml",
        }],
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", help="usbmon pcap/pcapng capture to convert")
    parser.add_argument("output", help="directory to write the YAML project into")
    parser.add_argument("--name", default=None, help="project/device display name")
    parser.add_argument("--vendor", default="Unknown", help="$vendor project var")
    parser.add_argument("--product", default="Unknown", help="$product project var")
    args = parser.parse_args(argv)

    try:
        transfers = read_usbmon_trace(args.capture)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if not transfers:
        print(f"error: no usbmon transfers decoded from {args.capture}", file=sys.stderr)
        return 1

    control_pairs = pair_control_transactions(transfers)

    device_desc = None
    config = None
    report_raw = None
    for submit, complete in control_pairs:
        if submit is None or submit.setup is None or complete is None or not complete.data:
            continue
        setup = decode_setup(submit.setup)
        if setup["bRequest"] != 0x06:
            continue
        desc_type = setup["wValue"] >> 8
        if desc_type == 0x01 and device_desc is None and len(complete.data) >= 18:
            device_desc = parse_device_descriptor(complete.data)
        elif desc_type == 0x02 and len(complete.data) >= 9:
            parsed = parse_config_descriptor(complete.data)
            if config is None or parsed["wTotalLength"] == len(complete.data):
                config = parsed
        elif desc_type == 0x22 and report_raw is None:
            report_raw = complete.data

    if device_desc is None:
        print("error: no GET_DESCRIPTOR(DEVICE) reply found in capture", file=sys.stderr)
        return 1
    if config is None:
        print("error: no GET_DESCRIPTOR(CONFIG) reply found in capture", file=sys.stderr)
        return 1

    report_items = None
    report_raw_file = None
    if report_raw is not None:
        items, ok = decompile_hid_report(report_raw, DEFAULT_HID_CODEC)
        if ok:
            report_items = items
        else:
            report_raw_file = "payloads/report_descriptor.bin"

    interrupt_payloads = {}
    for t in transfers:
        if t.xfer == XFER_INT and t.event_type == "C" and t.direction_in and t.data:
            interrupt_payloads.setdefault(t.endpoint, []).append(t.data.hex().upper())

    idVendor, idProduct = int(device_desc["idVendor"]), int(device_desc["idProduct"])
    name = args.name or f"Extracted Device {idVendor:04x}:{idProduct:04x}"
    slug = f"device_{idVendor:04x}_{idProduct:04x}"

    device = build_device(name, device_desc, config, report_items)
    if report_raw_file and config["interfaces"]:
        config["interfaces"][0]["report_descriptor"] = {"raw_file": report_raw_file}
    flow = build_flow(control_pairs, interrupt_payloads, report_raw_file)
    project = build_project(name, args.vendor, args.product, slug)

    out = pathlib.Path(args.output)
    write_yaml(out / f"{out.name}.yaml", project)
    write_yaml(out / "devices" / f"{slug}.yaml", device)
    write_yaml(out / "flows" / f"{slug}_flow.yaml", flow)
    if report_items is not None:
        write_yaml(out / "codecs" / "hid_report.yaml", DEFAULT_HID_CODEC)
    if report_raw_file:
        payload_path = out / report_raw_file
        payload_path.parent.mkdir(parents=True, exist_ok=True)
        payload_path.write_bytes(report_raw)

    print(f"Wrote project to {out}/")
    print(f"  {out.name}.yaml")
    print(f"  devices/{slug}.yaml")
    print(f"  flows/{slug}_flow.yaml")
    if report_items is not None:
        print("  codecs/hid_report.yaml")
    if report_raw_file:
        print(f"  {report_raw_file}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
