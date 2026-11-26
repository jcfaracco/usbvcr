# Architecture

This document describes the architecture and design of usbvcr: data flow, execution engines, backends, the YAML dialect, the generic codec system, and project structure.

> **usbredir** here is the wire protocol/transport spoken to QEMU
> (`-device usb-redir`), not this project. usbvcr is the emulator that speaks it.

---

## 1. Goals

1. Describe any USB device in YAML only — no recompilation per device.
2. Two authoring modes, both first-class:
   - **Schema mode** — named descriptor fields, serialized to raw packets.
   - **Raw mode** — opaque `hex` / `raw_file` payloads when no schema exists.
3. Behavior as a declarative **flow / state machine** (Ansible-like:
   `match`, `actions`, `transition_state`, bounded `when:` conditionals).
4. Fuzz-friendly: every field overridable, malformed values injectable.
5. Extensible to new device classes by **data** (codec tables), not new C.
6. Multi-target execution: drive virtual guests over network (QEMU via usbredir) or real hosts via Linux USB gadget hardware (FunctionFS/ConfigFS).

---

## 2. Data flow & System Architecture

```
                       ┌───────────────────────────────┐
                       │     Input Specifications      │
                       │ YAML Project    PCAP Capture  │
                       └───────┬───────────────┬───────┘
                               │               │
                               ▼               ▼
                       ┌───────────────┐┌───────────────┐
                       │  yaml_loader  ││  pcap_backend │
                       │ (lib/yaml,    ││ (lib/pcap)    │
                       │  lib/flow)    ││               │
                       └───────┬───────┘└───────┬───────┘
                               │                │
                               ▼                │
                       ┌───────────────┐        │
                       │ yaml_backend  │        │
                       └───────┬───────┘        │
                               │                │
                               ▼                ▼
                     ┌───────────────────────────────────┐
                     │   usb_device_ops (device seam)    │
                     └─────────────────┬─────────────────┘
                                       │
                         [ optional fuzz_wrap layer ]
                                       │
                     ┌─────────────────┴─────────────────┐
                     │                                   │
                     ▼ (usbredir)                        ▼ (FunctionFS ABI)
         ┌───────────────────────┐           ┌───────────────────────┐
         │      qemu_engine      │           │     local_engine      │
         │  (src/engine/qemu_*)  │           │  (src/engine/local_*) │
         │           │           │           │           │           │
         │   src/core/usb_core   │           │     src/lib/ffs/      │
         │           │           │           └───────────┬───────────┘
         │  src/core/transport   │                       │
         └───────────┬───────────┘                       ▼
                     │ TCP socket               Linux Gadget / UDC
                     ▼                       (ConfigFS FunctionFS fds)
             QEMU Virtual Guest
             (-device usb-redir)
```

YAML never reaches QEMU or FunctionFS directly. It is a device *description*; C serializes it to the binary descriptors and packet streams expected by the target engine.

---

## 3. YAML dialect (canonical)

The `tests/data/example{1,2}` layout is the canonical format. Separation of
concerns:

| Directory   | Role                                                        |
|-------------|-------------------------------------------------------------|
| `schemas/`  | Reusable descriptor **templates** (namespaced), with `null` placeholders for auto-computed fields. |
| `devices/`  | Concrete device instances: `import` schemas, apply `template`, override fields. |
| `flows/`    | Behavior: states, `match`/`actions` rules, transitions, endpoint queues. |
| `payloads/` | Opaque binary blobs (`raw_file`), for raw mode.             |
| `codecs/`   | Symbolic item-stream tables (e.g. HID report). See §6.      |
| `tests/`    | `ValidationSuite` cases: inject packets, assert behavior.   |

See [docs/yaml_dialect.md](yaml_dialect.md) for the full directory/key reference.

### 3.1 Auto-computed fields
Templates leave length/count fields `null`; the loader fills them after the
tree is built:
`bLength`, `wTotalLength`, `bNumInterfaces`, `bNumEndpoints`,
`wDescriptorLength`. Any of these may be **explicitly overridden** — this is
how fuzz/CVE cases inject bad values (e.g. `wTotalLength: 0xFFFF`).

### 3.2 Raw escape hatch
When no schema fits, bytes go through directly:
```yaml
report_descriptor:
  raw_file: "payloads/hid_consumer_report.bin"
# or
actions:
  - respond_raw: { file: "payloads/opaque_firmware_blob.bin" }
# or inline
payloads:
  - hex: "02000B0000000000"
```

### 3.3 State Machine Behavior
Flows express declarative response logic via states, `match:` rules, synchronous `transition_state`
transitions, and per-endpoint queues:
- **Hierarchical states (`extends:`):** Sub-states inherit rules from parent states without duplicating descriptors.
- **Global rules (`common_rules:`):** Rules evaluated across all states and machines to eliminate boilerplate.
- **Orthogonal machines (`machines:`):** Concurrent, independent state machines running per interface or component.

Planned additions (tracked in [TODO.md](../TODO.md)), kept **bounded** (no arbitrary expression eval — safety +
deterministic fuzzing):
- `vars:` block + `{{ var }}` interpolation.
- `when:` guards on rules/actions using a limited vocabulary
  (field compare, current state, counters).

---

## 4. One YAML dialect (canonical)

There is a single dialect: **canonical** — `device / schema / flow`
(see [docs/yaml_dialect.md](yaml_dialect.md)). The old pcap-replay grammar
(`packet / USBpcap / USBpcapSetup / Raw`) has been dropped; do not reintroduce a
second grammar into the loader.

Record/replay of captured traffic is handled at the **pcap layer**
(`src/lib/pcap/`), not as an alternate YAML dialect. A project manifest may
still carry a top-level `pcap:` field naming a capture to replay instead of
`devices:`/`flows:` (see [docs/yaml_dialect.md](yaml_dialect.md)) — that is
run-config convenience (same manifest, same `connection:` block), not a
second descriptor grammar; `src/lib/yaml/loader.c` never parses pcap framing.

---

## 5. Source layout

```
src/lib/                  # Pure utility & protocol logic (no socket/engine deps)
  usb_node.{c,h}          # UsbProp/UsbNode descriptor tree model + serializer
  yaml/                   # YAML parsing & tree construction
    loader.{c,h}          # Canonical dialect parser, manifest loading, import/template resolution
    tree.{c,h}            # UsbNode tree builder and null-field autofill (bLength, wTotalLength)
    util.{c,h}            # Path and file utilities for YAML loading
  schema.{c,h}            # YAML schema definitions & structural validation
  codec.{c,h}             # Generic table-driven item-stream encoder (see §6)
  flow.{c,h}              # Declarative state machine: states, rules, match, actions, queues
  stream/                 # Isochronous streaming infrastructure
    stream_provider.{c,h} # Packet-sliced streaming provider (e.g. UVC video)
    video_parser.{c,h}    # Frame parser (MJPEG container)
  storage/                # Mass storage infrastructure
    disk_provider.{c,h}   # SCSI transparent command set (SPC/SBC) provider for MSC BOT
  ffs/                    # Linux USB gadget management
    ffs.{c,h}             # ConfigFS & FunctionFS gadget creation, mounting, and teardown
  pcap/                   # Packet capture and replay
    pcap.{c,h}            # usbmon pcap/pcapng reader & writer
    record.{c,h}          # Live USB transfer recording
  log.{c,h} utils.{c,h}   # Logging and general helper utilities
  version.{c,h}           # Build version reporting

src/core/                 # Generic USB wire handling and abstractions
  device.h                # usb_device_ops — the central backend seam
  bytes.h                 # Endian-safe byte packing/unpacking helpers
  protocol.h              # usbredir wire protocol definitions, packet types, constants
  descriptor.{c,h}        # Configuration parsing from raw serialized descriptors
  transport.{c,h}         # usbredir framing, packet sending/receiving
  usb_core.{c,h}          # Handshake, announce, dispatch loop, ISO streaming & pacing

src/backends/             # Device implementations behind usb_device_ops
  yaml_backend.{c,h}      # Ops backend driven by loaded UsbNode tree + flow FSM
  pcap_backend.{c,h}      # Replay-from-capture ops backend

src/engine/               # Target execution transports
  qemu_engine.{c,h}       # Connects via TCP usbredir socket to QEMU
  local_engine.{c,h}      # Exposes USB device locally using Linux FunctionFS/ConfigFS

src/fuzz/                 # Fuzzing and mutation harness
  fuzz.{c,h}              # Descriptor mutation engine (multi-config, corrupt fields)
  fuzz_wrap.{c,h}         # Decorator wrapping any usb_device_ops with randomized mutations

src/main.c                # CLI entry point, configuration, backend and engine setup
```

**Layering rule:** `lib` knows nothing of sockets/engine; `core` knows nothing
of YAML; `backends` bridges data to `usb_device_ops`; `engine` drives the device
over a transport. Keeps `fuzz_wrap` functioning cleanly over any backend.

### 5.1 yaml_backend → usb_device_ops mapping
- `get_descriptor` — serialize requested node (device / config / string /
  hid_report) from the tree; cache bytes.
- `control` — run flow `match` rules → `respond_with` / `respond_raw` /
  `respond_with_status`.
- `ep_in` — drain current state's `queue_endpoint_data` or produce stream
  packets via `stream_provider` when configured with `stream_from_file`.
- `ep_out` — feed OUT data to flow (e.g. keyboard LEDs, or SCSI CBWs dispatched to `disk_provider` via `disk_from_file`).
- `tick` — run `on_enter`, advance transitions.
- `state` — current flow state + per-endpoint queues.

### 5.2 Reuse note
`src/lib/yaml/tree.c` emits serialized config bytes; `src/core/descriptor.c:desc_parse_config`
already walks that blob to build `interface_info` / `ep_info`. So `usb_core`
needs no YAML awareness — the only seam is which backend is plugged in.

---

## 6. Generic codec tables (replaces HID special-casing)

Symbolic descriptors (HID report items, etc.) are **not** hardcoded in C. They
are resolved through a data table imported like a schema. C keeps one generic
encoder; new device classes = new table, zero new code.

```
report_descriptor (symbolic)  +  codecs/hid_report.yaml  ──▶  codec encoder  ──▶  bytes
```

### 6.1 Table schema (HID example)
```yaml
version: "1.0"
codec: "hid_report"
# HID short item prefix byte = bTag<<4 | bType<<2 | bSize
item_types: { main: 0, global: 1, local: 2 }

tokens:
  usage_page:      { type: global, tag: 0x0, width: auto }
  logical_minimum: { type: global, tag: 0x1, width: auto }
  logical_maximum: { type: global, tag: 0x2, width: auto }
  report_size:     { type: global, tag: 0x7, width: auto }
  report_count:    { type: global, tag: 0x9, width: auto }
  usage:           { type: local,  tag: 0x0, width: auto }
  usage_minimum:   { type: local,  tag: 0x1, width: auto }
  usage_maximum:   { type: local,  tag: 0x2, width: auto }
  collection:      { type: main,   tag: 0xA, width: 1 }
  end_collection:  { type: main,   tag: 0xC, width: 0 }
  input:           { type: main,   tag: 0x8, width: 1, flagset: io }
  output:          { type: main,   tag: 0x9, width: 1, flagset: io }

symbols:
  usage_page: { "Generic Desktop": 0x01, "Key Codes": 0x07 }
  usage:      { "Keyboard": 0x06 }
  collection: { "Application": 0x01, "Physical": 0x00 }

flagsets:
  io: { Data: 0, Const: 1, Array: 0, Var: 2, Abs: 0, Rel: 4 }
```

### 6.2 Encoder contract (table-driven, no HID knowledge in C)
1. For each `{key: value}` item, look up `key` in `tokens` →
   `(type, tag, width, symbol-map, flagset)`.
2. Resolve value: number as-is; string via `symbols[key]`; comma-list
   (`"Data,Var,Abs"`) OR-folded via `flagsets`.
3. `width: auto` → pick 0/1/2/4 bytes by magnitude → `bSize` code.
4. Emit prefix `tag<<4 | type<<2 | bSize`, then operand little-endian.

HID report is a flat byte stream — `collection` / `end_collection` are just
tokens; no nesting/recursion required.

### 6.3 Generalization
Other symbolic descriptors get their own table
(`codecs/audio_class.yaml`, `codecs/vendor_tlv.yaml`), dispatched by the
`codec:` field. If a class needs different framing (fixed TLV, length-prefixed),
add an `encoding:` mode field to the table and branch in the encoder — still
data, not per-class C.

### 6.4 Notes
- HID usage tables are huge — ship a curated subset; numeric fallback always
  allowed (`usage: 0x06`).
- Pin the `width: auto` magnitude rule (signed vs unsigned matters for
  `logical_minimum`).
- Unknown token → hard error, not silent skip (fuzzing needs determinism).

---

## 7. scripts/pcap_to_yaml.py (pcap → project)

Extracts a device+flow project directly from a usbmon pcap/pcapng capture
(the same framing `src/lib/pcap/pcap.c` reads/writes — see
[docs/yaml_dialect.md](yaml_dialect.md)), emitting canonical-dialect
`devices/` + `flows/` (+ `codecs/` when the HID report descriptor decompiles
cleanly) YAML. Anything the heuristics cannot turn into symbolic
dialect fields falls back to a raw payload file, so the emitted project still
reproduces the capture byte-for-byte.

---

## 8. Validation / CVE tests

`tests/*/tests/*.yaml` (scope inferred from the `tests/` directory): inject crafted packets
(`inject_packet`, `override_payload`) and assert guest behavior
(`assert_behavior: { kernel_crash: false, driver_status: stalled }`).
The test harness runs suites against `usb_core` and the execution backends.

---

## 9. Development & Roadmap

Active and upcoming architectural improvements are tracked in [TODO.md](../TODO.md). Key areas include:
- Standardizing engines around an `engine_ops_t` interface.
- Asynchronous tick-driven delayed responses and state dwell times.
