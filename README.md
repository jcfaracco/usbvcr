# usbvcr

**YAML in, real USB traffic out.** Describe a USB device — descriptors,
behavior, raw payloads — in human-readable YAML, and usbvcr serves it live to
QEMU (or a real Linux USB gadget) over the wire. No per-device C code, no
recompiling: one generic engine turns a YAML project into binary USB frames
and drives the state machine.

[![CI](https://github.com/jcfaracco/usbvcr/actions/workflows/ci.yml/badge.svg)](https://github.com/jcfaracco/usbvcr/actions/workflows/ci.yml)
[![Coverage: 72%](https://img.shields.io/badge/coverage-72%25-brightgreen.svg)](tests)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-00599C.svg)](src)
[![Build: Meson](https://img.shields.io/badge/build-meson%20%2B%20ninja-informational.svg)](meson.build)

## Why usbvcr?

- 🎛️ **Device-as-data** — a device is a directory of YAML, not a C file.
  Schemas, instances, and behavior are cleanly separated.
- 🧩 **Two authoring modes** — named descriptor fields for structured
  devices, raw `hex` / file payloads for everything else.
- 🔁 **Record *and* replay** — capture a real device's USB traffic to
  pcap/pcapng and play it straight back to a guest.
- 🧠 **Declarative behavior** — flows are a small, Ansible-flavored state
  machine (`match` → `actions` → `transition_state`), not callback soup.
- 🧪 **Fuzz-friendly by design** — every field is overridable and every
  transfer can be mutated; built for finding guest USB-stack bugs.
- 🐧 **Two engines** — speak usbredir to QEMU, or drive a real Linux
  FunctionFS/ConfigFS gadget with the exact same device description.

## Quick start

**Dependencies**

- C11 compiler (gcc or clang)
- `meson >= 1.1.0`, `ninja`
- `glib-2.0`, `libyaml` (`yaml-0.1`) — runtime deps
- `check` — unit-test framework (optional; tests are skipped if absent)

```sh
# Configure a build directory (debugoptimized by default)
meson setup build

# Compile — produces the `usbvcr` binary under build/src/
ninja -C build

# Run the test suite
meson test -C build
```

**Coverage build**

```sh
meson setup build -Denable_coverage=true
ninja -C build && meson test -C build
gcovr -r . build
```

**Run it**

```sh
# Emulate a YAML-described device, served to QEMU over usbredir
./build/src/usbvcr --yaml tests/data/example1 --host localhost --port 1337

# Replay a captured pcap/pcapng trace instead
./build/src/usbvcr --pcap capture.pcapng --host localhost --port 1337

# Record every transfer of a live session to pcapng
./build/src/usbvcr --yaml tests/data/example1 --record session.pcapng

# Skip QEMU: drive a real Linux USB gadget directly (FunctionFS/ConfigFS)
sudo ./build/src/usbvcr --yaml tests/data/example1 --local
```

For detailed guides, QEMU command lines, and `dummy_hcd` / hardware OTG setup, see [`docs/running_engines.md`](docs/running_engines.md).

## How it works

```
YAML project                         canonical dialect: schemas/ devices/ flows/ payloads/ codecs/
     │
     ▼  src/lib/yaml_loader + codec + flow
UsbNode descriptor tree  +  flow state machine
     │
     ▼  src/backends/yaml_backend          (implements usb_device_ops)
     ▼  src/core/usb_core                  (hello handshake, announce, dispatch)
     ▼  src/core/transport                 (usbredir wire framing)
     ▼  src/engine/{qemu,local}_engine ──▶  QEMU usbredir  or  Linux USB gadget
```

YAML never reaches the wire directly — it's a device *description*; C
serializes it into the binary frames QEMU (or the kernel gadget stack)
expects. One translation layer, one engine, any device.

## A device in YAML

```yaml
# devices/keyboard.yaml
name: "Example Keyboard"
imports: ["schemas/usb_base.yaml"]
device_descriptor:
  template: "usb_base.device_descriptor"
  idVendor: 0x04d9
  idProduct: 0x1702
```

```yaml
# flows/hello_world_flow.yaml
states:
  idle:
    on_enter: []
    rules:
      - match: { request: "GET_DESCRIPTOR" }
        actions: [respond_with: "device_descriptor"]
```

Raw payloads (firmware blobs, opaque reports) drop straight into either file
with `hex: "..."` or `raw_file: "payloads/blob.bin"` — no schema required.
See [`tests/data/example1`](tests/data/example1) for a complete minimal
project and [`docs/yaml_dialect.md`](docs/yaml_dialect.md) for the full key
reference.

## Project layout

| Path              | What lives there                                          |
|-------------------|-------------------------------------------------------------|
| `src/lib/`        | Pure logic: YAML loader, codec tables, flow engine, pcap record/replay. No sockets. |
| `src/core/`       | The generic USB engine: wire transport, protocol, handshake/dispatch. |
| `src/backends/`   | `usb_device_ops` implementations — YAML-driven and pcap-replay. |
| `src/engine/`     | Transports: usbredir-to-QEMU, and the local FunctionFS/ConfigFS gadget. |
| `src/fuzz/`       | A `usb_device_ops` wrapper that mutates traffic for any backend. |
| `data/`           | Built-in asset library: base schemas, codecs, device templates, and turnkey manifests. |
| `examples/`       | Complete ready-to-run device projects (Joystick, Mass Storage, UVC Webcam). |
| `tests/`          | `check`-based unit/integration tests + example YAML projects. |
| `docs/`           | Architecture deep-dive and the full YAML dialect reference. |
| `scripts/`          | pcap → YAML project converter (`pcap_to_yaml.py`). |

## Testing

Unit and integration tests use [libcheck](https://libcheck.github.io/check/)
and run via Meson:

```sh
meson test -C build --print-errorlogs
```

Coverage is tracked with `gcovr` (see the Coverage build above) and enforced
in CI — see [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## Fuzzing

`src/fuzz/` wraps any `usb_device_ops` backend with mutation of descriptors,
IN endpoint data, and (optionally) control transfers. `harness/run_fuzz.sh`
drives a QEMU guest (ideally KASAN/UBSAN-instrumented) with incrementing
seeds and saves a reproducer on crash.

## Examples

Complete, ready-to-run device emulation projects demonstrating full enumeration, custom descriptors, declarative flows, and payload streaming are available in the [`examples/`](examples/README.md) directory:

- [**Joystick**](examples/joystick/README.md) — 4-axis analog joystick with 12 digital buttons (HID class).
- [**Mass Storage**](examples/msc/README.md) — Flash drive (pendrive) using Bulk-Only Transport (BOT) and SCSI commands backed by a FAT16 disk image.
- [**UVC Webcam**](examples/uvc/README.md) — USB Video Class webcam streaming Motion-JPEG video over isochronous endpoints.
- [**Composite Device**](examples/composite/README.md) — Multi-interface UVC webcam and telemetry device demonstrating concurrent orthogonal state machines, global common rules, state inheritance, and dynamic streaming.

See [`examples/README.md`](examples/README.md) for detailed descriptions, running instructions, and guest verification steps.

## Documentation

- [`docs/architecture.md`](docs/architecture.md) — design goals, data flow,
  source layout, the generic codec system, build order.
- [`docs/running_engines.md`](docs/running_engines.md) — comprehensive guide for
  QEMU (`usbredir`) and local Linux gadget (`FunctionFS` / `ConfigFS`) execution.
- [`docs/yaml_dialect.md`](docs/yaml_dialect.md) — canonical YAML directory
  and key reference.
- [`data/README.md`](data/README.md) — built-in base schemas, codecs, reference devices, and manifests.
- [`examples/README.md`](examples/README.md) — guide to the included device examples.

## License

MIT — see [LICENSE](LICENSE).
