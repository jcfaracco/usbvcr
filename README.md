# YAML-Driven USB Device Emulator (usbredir)

A generic USB device emulator and fuzzing harness. Devices are described in
**human-readable YAML** — descriptors, behavior, and raw payloads — and served
over the **usbredir** wire protocol to QEMU. No per-device C code: one generic
engine serializes YAML into binary USB frames and drives the state machine.

This document is the **architecture and implementation plan**. It supersedes
the old "hello world" client README.

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

## 2. Data flow

```
YAML files
  (schemas / devices / flows / payloads / codecs / tests)
        │
        ▼  src/lib/yaml_loader
UsbNode descriptor tree  +  flow model
        │
        ▼  src/backends/yaml_backend  (implements usb_device_ops)
        │
        ▼  src/core/usb_core  (hello handshake, announce, dispatch)
        │
        ▼  src/core/transport  (usbredir wire framing)
        │
        ▼  usbredir socket ──▶ QEMU (-device usb-redir)
```

YAML never reaches QEMU. It is a device *description*; C serializes it to the
binary usbredir frames QEMU speaks. One translation layer.

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

### 3.3 Ansible-like behavior (bounded)
Flows already express implicit conditions via `match:` + `transition_state`.
Planned additions, kept **bounded** (no arbitrary expression eval — safety +
deterministic fuzzing):
- `vars:` block + `{{ var }}` interpolation.
- `when:` guards on rules/actions using a limited vocabulary
  (field compare, current state, counters).
- loops already covered by list payloads (`queue_endpoint_data.payloads`).

## 4. Two YAML dialects — do not mix

- **Canonical** (this doc): `device / schema / flow`.
- **Legacy pcap-replay**: `packet / USBpcap / USBpcapSetup / Raw`
  (matches `usb_description_replay.yaml`, parsed today by `src/lib/usb_yaml.c`).

Action: commit to the canonical dialect. Isolate the legacy parser under
`src/lib/replay/` (or `src/pcap/`). Do not overload one parser with both
grammars.

## 5. Target source layout

```
src/lib/            # pure: no socket/engine deps
  usb_node.{c,h}      # UsbProp/UsbNode model + serializer (from glib.c; fix type-name bugs)
  yaml_loader.{c,h}   # canonical dialect -> UsbNode tree; imports/templates/override; null autofill
  codec.{c,h}         # generic table-driven item-stream encoder (see §6)
  flow.{c,h}          # states, rules, match, actions, transitions, when: eval, endpoint queues
  log.{c,h} utils.{c,h} record.{c,h}   # keep
  replay/             # legacy pcap-replay parser (moved usb_yaml.c + usbpcap_hdr.h)

src/core/           # engine — largely unchanged, already generic
  transport.{c,h}     # usbredir framing
  protocol.h          # wire structs / caps / constants
  usb_core.{c,h}      # handshake, announce, dispatch loop
  descriptor.{c,h}    # desc_parse_config: consumes serialized config bytes (reused as-is)
  device.h            # usb_device_ops — the backend seam

src/backends/
  yaml_backend.{c,h}  # ONE generic ops backend driven by loaded tree + flow
  hid_keyboard.c      # demote to demo, or delete

src/fuzz/           # unchanged — fuzz_wrap wraps any usb_device_ops (incl. yaml_backend)
```

**Layering rule:** `lib` knows nothing of sockets/engine; `core` knows nothing
of YAML; `backends` bridges. Keeps `fuzz_wrap` working for free.

### 5.1 yaml_backend → usb_device_ops mapping
- `get_descriptor` — serialize requested node (device / config / string /
  hid_report) from the tree; cache bytes.
- `control` — run flow `match` rules → `respond_with` / `respond_raw` /
  `respond_with_status`.
- `ep_in` — drain current state's `queue_endpoint_data`.
- `ep_out` — feed OUT data to flow (e.g. keyboard LEDs).
- `tick` — run `on_enter`, advance transitions.
- `state` — current flow state + per-endpoint queues.

### 5.2 Reuse note
`yaml_loader` emits serialized config bytes; the existing
`descriptor.c:desc_parse_config` already walks that blob to build
`interface_info` / `ep_info`. So `usb_core` needs **no change** — the only new
seam is swapping the hardcoded backend for `yaml_backend`.

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

## 7. converter.py (pcap → project)

Extracts a device+flow project from a Wireshark JSON export. Current state is a
usable skeleton with known gaps to fix:
- Report-descriptor path writes a placeholder byte instead of real hex —
  extract actual bytes from the packet frame.
- `wValue` fallback concatenation is fragile — harden.
- Emits canonical-dialect `devices/` + `flows/` YAML.

## 8. Validation / CVE tests

`tests/*/tests/*.yaml` (`kind: ValidationSuite`): inject crafted packets
(`inject_packet`, `override_payload`) and assert guest behavior
(`assert_behavior: { kernel_crash: false, driver_status: stalled }`).
A separate test driver reuses `yaml_loader` plus a raw inject path into
`transport`. Landing module: `src/harness/` or `src/tests/`.

## 9. Build order

1. `usb_node` — adopt `glib.c`, fix type-name bugs (`UsbNode`/`UsbProp`
   typedefs, add `create_prop_u32`).
2. `yaml_loader` — imports, templates, override, null-length autofill.
3. `codec` — generic table encoder + `codecs/hid_report.yaml`.
4. `flow` — states, rules, actions, transitions, bounded `when:`.
5. `yaml_backend` — `usb_device_ops` adapter over tree + flow.
6. Wire into `main.c` — `--device-yaml <path>`; keep builtin registry.
7. Isolate legacy pcap-replay parser under `src/lib/replay/`.
8. Fix `converter.py` real-hex extraction.
9. `ValidationSuite` runner.

## 10. License

MIT.
