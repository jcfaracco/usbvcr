#!/usr/bin/env python3
"""
pcap_filter.py - keep only one USB device's traffic in a usbmon pcap/pcapng
capture (device <-> host), identified by bus+device number or by
idVendor:idProduct.

Mirrors the exact usbmon-over-pcap/pcapng framing src/lib/pcap/pcap.c reads
and writes (see docs/yaml_dialect.md) - the same framing
scripts/pcap_to_yaml.py decodes. Every retained record's 64-byte usbmon
header and captured payload is copied through byte-for-byte (status,
truncated length vs len_cap, isochronous ndesc/interval, everything); only
the surrounding container framing is rebuilt, so this also converts between
legacy pcap and pcapng - the output format is inferred from OUTPUT's
extension, same convention as pcap_format_get() in pcap.c.

Usage:
    # by bus+device, straight from `lsusb`:
    #   "Bus 001 Device 004: ID 46f4:0011 QEMU QEMU USB Video Class Device"
    python3 scripts/pcap_filter.py usb_capture.pcap device.pcap \\
        --bus 1 --device 4

    # by idVendor:idProduct instead - resolved by scanning the capture for
    # that device's GET_DESCRIPTOR(DEVICE) reply, whichever bus/device
    # number it enumerated on (the transient address-0 state every device
    # passes through during enumeration is ignored; see resolve_by_vid_pid).
    python3 scripts/pcap_filter.py usb_capture.pcap device.pcapng \\
        --vendor 46f4 --product 0011
"""

import argparse
import pathlib
import struct
import sys

# usbmon header framing - mirrors src/lib/pcap/pcap.h's usbmon_header_t and
# scripts/pcap_to_yaml.py's USBMON_HDR_FMT. Verified byte-for-byte against
# the C struct's natural alignment (no padding falls between any of these
# fields on a 64-bit build) and against a real capture (usb_capture.pcap).
USBMON_HDR_FMT = "<QccBBHccqiiII8siiII"
USBMON_HDR_SIZE = struct.calcsize(USBMON_HDR_FMT)
assert USBMON_HDR_SIZE == 64

XFER_CONTROL = 2

PCAP_MAGIC_MS = 0xA1B2C3D4
PCAP_MAGIC_NS = 0xA1B23C4D
PCAPNG_SHB = 0x0A0D0D0A
PCAPNG_IDB = 0x00000001
PCAPNG_EPB = 0x00000006


class Record:
    """One usbmon record: the raw 64-byte header plus its captured payload,
    both kept byte-for-byte as read from the source capture."""

    def __init__(self, header: bytes, payload: bytes):
        (self.urb_id, typ, xfer_type, _epnum, self.devnum, self.busnum,
         flag_setup, _flag_data, ts_sec, ts_usec, _status, _length,
         _len_cap, setup, *_rest) = struct.unpack(USBMON_HDR_FMT, header)
        self.header = header
        self.payload = payload
        self.event_type = typ.decode()
        self.xfer_type = xfer_type[0]
        self.setup = setup if flag_setup == b"\x00" else None
        # usbmon's embedded timestamp is always microsecond-resolution,
        # independent of the outer container's declared resolution (and the
        # only one usbvcr's own reader/writer actually looks at - see
        # pcap_decode_usbmon_header() in pcap.c).
        self.ts_usec_total = ts_sec * 1_000_000 + ts_usec

    def matches(self, bus, device) -> bool:
        return (bus is None or self.busnum == bus) and (device is None or self.devnum == device)


def _decode_setup(raw8: bytes) -> dict:
    bmRequestType, bRequest, wValue, wIndex, wLength = struct.unpack("<BBHHH", raw8)
    return {"bmRequestType": bmRequestType, "bRequest": bRequest,
            "wValue": wValue, "wIndex": wIndex, "wLength": wLength}


def _read_legacy(data: bytes) -> list:
    magic, = struct.unpack_from("<I", data, 0)
    if magic not in (PCAP_MAGIC_MS, PCAP_MAGIC_NS):
        raise ValueError(f"not a legacy pcap file (magic={magic:#x})")
    records = []
    off = 24  # pcap_header_t
    while off + 16 <= len(data):
        _, _, incl_len, _ = struct.unpack_from("<IIII", data, off)
        off += 16
        raw = data[off:off + incl_len]
        off += incl_len
        if len(raw) < USBMON_HDR_SIZE:
            break
        records.append(Record(raw[:USBMON_HDR_SIZE], raw[USBMON_HDR_SIZE:]))
    return records


def _read_pcapng(data: bytes) -> list:
    records = []
    off = 0
    while off + 12 <= len(data):
        block_type, block_len = struct.unpack_from("<II", data, off)
        if block_len < 12 or off + block_len > len(data):
            break
        if block_type == PCAPNG_EPB:
            _, _, _, cap_len, _ = struct.unpack_from("<IIIII", data, off + 8)
            raw = data[off + 28:off + 28 + cap_len]
            if len(raw) >= USBMON_HDR_SIZE:
                records.append(Record(raw[:USBMON_HDR_SIZE], raw[USBMON_HDR_SIZE:]))
        # Anything else (Interface Statistics, Name Resolution, ...) is
        # skipped - only Enhanced Packet Blocks carry usbmon records.
        off += block_len
    return records


def read_capture(path) -> list:
    data = pathlib.Path(path).read_bytes()
    if len(data) < 4:
        raise ValueError(f"{path}: file too small to be a capture")
    magic, = struct.unpack_from("<I", data, 0)
    if magic == PCAPNG_SHB:
        return _read_pcapng(data)
    if magic in (PCAP_MAGIC_MS, PCAP_MAGIC_NS):
        return _read_legacy(data)
    raise ValueError(f"{path}: not a recognized pcap/pcapng file (magic={magic:#x})")


def resolve_by_vid_pid(records: list, vendor: int, product: int):
    """Scan Control Submit/Complete pairs for a GET_DESCRIPTOR(DEVICE) reply
    matching vendor:product, return the (bus, device) it enumerated on.

    Every USB device answers this on address 0 first, before SET_ADDRESS
    moves it to its real (non-zero) address - device 0 is excluded so this
    doesn't resolve to that transient, nearly-traffic-free state instead of
    the device's actual session. If idVendor:idProduct still matches more
    than one non-zero (bus, device) - e.g. the same device reconnecting
    mid-capture - the one with the most overall matching records wins.
    """
    pending = {}
    hits = set()
    for r in records:
        if r.xfer_type != XFER_CONTROL:
            continue
        if r.event_type == "S":
            pending[r.urb_id] = r
        elif r.event_type == "C":
            submit = pending.pop(r.urb_id, None)
            if submit is None or submit.setup is None or len(r.payload) < 12:
                continue
            setup = _decode_setup(submit.setup)
            if setup["bRequest"] != 0x06 or (setup["wValue"] >> 8) != 0x01:
                continue  # not GET_DESCRIPTOR(DEVICE)
            if submit.devnum == 0:
                continue  # default address during enumeration, not a session
            idVendor, idProduct = struct.unpack_from("<HH", r.payload, 8)
            if idVendor == vendor and idProduct == product:
                hits.add((submit.busnum, submit.devnum))

    if not hits:
        return None
    if len(hits) == 1:
        return next(iter(hits))
    return max(hits, key=lambda bd: sum(1 for r in records if r.busnum == bd[0] and r.devnum == bd[1]))


def write_legacy(records: list, out_path) -> None:
    with open(out_path, "wb") as f:
        f.write(struct.pack("<IHHiIII", PCAP_MAGIC_MS, 2, 4, 0, 0, 65535, 220))
        for r in records:
            incl_len = USBMON_HDR_SIZE + len(r.payload)
            sec = (r.ts_usec_total // 1_000_000) & 0xFFFFFFFF
            usec = (r.ts_usec_total % 1_000_000) & 0xFFFFFFFF
            f.write(struct.pack("<IIII", sec, usec, incl_len, incl_len))
            f.write(r.header)
            f.write(r.payload)


def write_pcapng(records: list, out_path) -> None:
    with open(out_path, "wb") as f:
        # Section Header Block (24-byte body + trailing block_len copy).
        f.write(struct.pack("<IIIHHq", PCAPNG_SHB, 28, 0x1A2B3C4D, 1, 0, -1))
        f.write(struct.pack("<I", 28))
        # Interface Description Block (16-byte body + trailing copy).
        f.write(struct.pack("<IIHHI", PCAPNG_IDB, 20, 220, 0, 65535))
        f.write(struct.pack("<I", 20))

        for r in records:
            cap_len = USBMON_HDR_SIZE + len(r.payload)
            padding = (4 - (cap_len % 4)) % 4
            block_len = 28 + cap_len + padding + 4
            ts_high = (r.ts_usec_total >> 32) & 0xFFFFFFFF
            ts_low = r.ts_usec_total & 0xFFFFFFFF
            f.write(struct.pack("<IIIIIII", PCAPNG_EPB, block_len, 0,
                                ts_high, ts_low, cap_len, cap_len))
            f.write(r.header)
            f.write(r.payload)
            f.write(b"\x00" * padding)
            f.write(struct.pack("<I", block_len))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", help="usbmon pcap/pcapng capture to filter")
    parser.add_argument("output",
                        help="output capture path - .pcapng for pcapng, anything else for legacy pcap")
    parser.add_argument("--bus", type=int, default=None,
                        help="USB bus number (lsusb's 'Bus NNN')")
    parser.add_argument("--device", type=int, default=None,
                        help="USB device number (lsusb's 'Device NNN')")
    parser.add_argument("--vendor", type=lambda s: int(s, 16), default=None, metavar="VID",
                        help="idVendor, hex (e.g. 46f4) - resolves --bus/--device automatically")
    parser.add_argument("--product", type=lambda s: int(s, 16), default=None, metavar="PID",
                        help="idProduct, hex (e.g. 0011), used together with --vendor")
    args = parser.parse_args(argv)

    if (args.vendor is None) != (args.product is None):
        parser.error("--vendor and --product must be given together")
    if args.bus is None and args.device is None and args.vendor is None:
        parser.error("specify --bus/--device, or --vendor/--product")

    try:
        records = read_capture(args.capture)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if not records:
        print(f"error: no usbmon records decoded from {args.capture}", file=sys.stderr)
        return 1

    bus, device = args.bus, args.device
    if args.vendor is not None:
        resolved = resolve_by_vid_pid(records, args.vendor, args.product)
        if resolved is None:
            print(f"error: no GET_DESCRIPTOR(DEVICE) reply for idVendor={args.vendor:#06x} "
                  f"idProduct={args.product:#06x} found in {args.capture}", file=sys.stderr)
            return 1
        resolved_bus, resolved_device = resolved
        if (bus is not None and bus != resolved_bus) or (device is not None and device != resolved_device):
            print(f"error: --bus/--device ({bus}/{device}) conflicts with the bus/device "
                  f"({resolved_bus}/{resolved_device}) resolved for that idVendor:idProduct",
                  file=sys.stderr)
            return 1
        bus, device = resolved_bus, resolved_device
        print(f"Resolved idVendor={args.vendor:#06x} idProduct={args.product:#06x} "
              f"-> Bus {bus:03d} Device {device:03d}")

    kept = [r for r in records if r.matches(bus, device)]
    if not kept:
        print(f"error: no records matched Bus {bus} Device {device}", file=sys.stderr)
        return 1

    out_path = pathlib.Path(args.output)
    if out_path.suffix == ".pcapng":
        write_pcapng(kept, out_path)
    else:
        write_legacy(kept, out_path)

    print(f"Wrote {len(kept)}/{len(records)} records (Bus {bus:03d} Device {device:03d}) to {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
