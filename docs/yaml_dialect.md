# Canonical YAML Dialect — Directory & Key Reference

A device project is a directory tree. Each **subdirectory name is authoritative**
for the *scope* (role) of the YAML files inside it — no `kind:`/`scope:` field is
written in the files themselves. The loader (`src/lib/yaml_loader.c`) maps the
parent directory to a `yaml_scope_t`.

```
<project>/
├── schemas/     reusable descriptor templates
├── devices/     concrete device instances
├── flows/       behavior state machines
├── codecs/      symbolic item-stream encoding tables
├── payloads/    opaque binary blobs (raw mode)
└── tests/       validation / CVE suites
```

Every YAML file starts with `version: "1.0"` (validated by `match_version`).

---

## Project manifest — `<project>/<name>.yaml`

The file passed to `--yaml` is the project manifest. It sits at the project
root (siblings of `devices/`, `flows/`, ...) and lists which device+flow
pairs make up the project:

```yaml
version: "1.0"
name: "Hello World Injector Keyboard"   # optional; used as the gadget name
                                        # for the local engine (ConfigFS/
                                        # FunctionFS dir), falls back to
                                        # "usbvcr" if unset.

connection:                            # optional; run config normally given
                                        # on the CLI. Any field left out here
                                        # falls back to its CLI flag/default.
                                        # Explicit CLI flags always win.
  engine: local                        # "local" or "qemu" (default: qemu)
  udc: dummy_udc.0                     # local engine only (default: auto-detect)
  host: 127.0.0.1                      # qemu engine only (default: localhost)
  port: "1337"                         # qemu engine only (default: 1337)

devices:
  - device: "devices/keyboard_hello_world.yaml"
    flow: "flows/hello_world_flow.yaml"
```

With a `connection:` block set, `usbvcr --yaml <project>.yaml` alone is
enough to run — no `--local`/`--udc`/`--host`/`--port` needed. See
[`running_engines.md`](running_engines.md) for the CLI-flag equivalents.

---

## `schemas/` — descriptor templates

Reusable, namespaced descriptor fragments. Fields left `null` are
**auto-computed** by the loader after the tree is built (lengths/counts), or
explicitly overridden by a device (including to inject bad values for fuzzing).

| Key | Value | Meaning |
|-----|-------|---------|
| `version` | string | dialect version |
| `namespace` | string | prefix used when a device references a template (`usb_base.device_descriptor`) |
| `templates` | map | named descriptor templates |

A template is a map of USB descriptor fields, e.g.:

```yaml
namespace: "usb_base"
templates:
  device_descriptor:
    bLength: 18
    bDescriptorType: 0x01
    idVendor: null          # concrete device fills this
    bNumConfigurations: 1
  configuration_descriptor:
    wTotalLength: null      # auto-computed
    bNumInterfaces: null    # auto-computed
```

Auto-computed (`null`-eligible) fields: `bLength`, `wTotalLength`,
`bNumInterfaces`, `bNumEndpoints`, `wDescriptorLength`.

---

## `devices/` — concrete device instances

Import schemas, apply a template, override named fields, and lay out the full
descriptor tree (configurations → interfaces → endpoints).

| Key | Value | Meaning |
|-----|-------|---------|
| `version` | string | dialect version |
| `name` | string | human label |
| `imports` | list of paths | schema files to pull templates from |
| `device_descriptor` | map | uses `template:` + field overrides |
| `strings` | map `index: text` | USB string descriptors |
| `configurations` | list | one entry per configuration |
| `flows` | list of `include:` | behavior files driving this device |

Inside a configuration:

| Key | Value | Meaning |
|-----|-------|---------|
| `template` | string | schema template to base on |
| `bConfigurationValue`, `bmAttributes`, `bMaxPower` | int | standard config fields |
| `interfaces` | list | interface descriptors |

Inside an interface:

| Key | Value | Meaning |
|-----|-------|---------|
| `bInterfaceNumber`, `bInterfaceClass`, `bInterfaceSubClass`, `bInterfaceProtocol` | int | standard interface fields |
| `hid_descriptor` | map | e.g. `wDescriptorLength` |
| `endpoints` | list | `address`, `attributes`, `max_packet_size`, `interval` |
| `report_descriptor` | list **or** `raw_file` | symbolic item list (see `codecs/`) or an opaque blob path |

Field override example:

```yaml
device_descriptor:
  template: "usb_base.device_descriptor"
  idVendor: 0x04d9          # override the schema's null
  idProduct: 0x1702
```

Two authoring modes coexist:
- **Schema mode** — named fields, serialized to bytes.
- **Raw mode** — `raw_file: "payloads/…"` when no schema fits.

---

## `flows/` — behavior state machines

Declarative response logic: states, match rules, actions, transitions, and
per-endpoint data queues. No arbitrary expression evaluation (deterministic for
fuzzing).

| Key | Value | Meaning |
|-----|-------|---------|
| `version` | string | dialect version |
| `states` | list | named states, evaluated in order |

A state:

| Key | Value | Meaning |
|-----|-------|---------|
| `name` | string | state identifier |
| `rules` | list | `match:` → `actions:` pairs |
| `on_enter` | list | actions run when the state is entered |

A rule's `match:` keys match an incoming control setup packet:
`bmRequestType`, `bRequest`, `wValue`, `wIndex` — or `endpoint_poll: 0x81` for
IN-endpoint polling.

Actions:

| Action | Meaning |
|--------|---------|
| `respond_with` | serialize a tree node, e.g. `"$device.device_descriptor"`, `"$device.configurations[0].interfaces[0].report_descriptor"` |
| `respond_raw` | send an opaque payload: `{ file: "payloads/blob.bin" }` |
| `respond_with_status` | ACK/stall a setup packet (`0` = ACK) |
| `transition_state` | switch to another state by name |
| `queue_endpoint_data` | queue `payloads` (list of `hex:`) to an `endpoint:` |
| `return_nak_if_queue_empty` | NAK polls when the endpoint queue is drained |

```yaml
states:
  - name: "enumeration"
    rules:
      - match: { bmRequestType: 0x80, bRequest: 6, wValue: 0x0100 }
        actions:
          - respond_with: "$device.device_descriptor"
      - match: { bmRequestType: 0x00, bRequest: 9, wValue: 1 }   # SET_CONFIGURATION
        actions:
          - respond_with_status: 0
          - transition_state: "type_hello_world"
  - name: "type_hello_world"
    on_enter:
      - queue_endpoint_data:
          endpoint: 0x81
          payloads:
            - hex: "02000B0000000000"   # Shift + H
            - hex: "0000000000000000"   # release
```

---

## `codecs/` — symbolic encoding tables

Data tables that turn symbolic descriptors (HID report items, etc.) into bytes,
so no device-class knowledge lives in C. Dispatched by the `codec:` field.

| Key | Value | Meaning |
|-----|-------|---------|
| `version` | string | dialect version |
| `codec` | string | table identity, e.g. `"hid_report"` |
| `item_types` | map | type-name → numeric code |
| `tokens` | map | token → `{ type, tag, width, flagset? }` |
| `symbols` | map | per-token named value → number (e.g. usages) |
| `flagsets` | map | named bit → shift, OR-folded from comma lists |

```yaml
codec: "hid_report"
tokens:
  usage_page: { type: global, tag: 0x0, width: auto }
  input:      { type: main,   tag: 0x8, width: 1, flagset: io }
symbols:
  usage_page: { "Generic Desktop": 0x01, "Key Codes": 0x07 }
flagsets:
  io: { Data: 0, Const: 1, Var: 2, Rel: 4 }
```

`width: auto` picks 0/1/2/4 bytes by magnitude. Encoder emits the prefix
`tag<<4 | type<<2 | bSize` then the operand little-endian. Unknown token = hard
error (determinism).

---

## `payloads/` — opaque binaries

Raw byte blobs referenced by `raw_file:` (descriptors) or `respond_raw: { file }`
(flow actions). Not parsed as dialect — copied verbatim into the wire. Example:
`hid_consumer_report.bin` (a real HID report descriptor), an opaque firmware blob.

---

## `tests/` — validation / CVE suites

Inject crafted packets and assert guest behavior. Scope inferred from the
`tests/` directory.

| Key | Value | Meaning |
|-----|-------|---------|
| `version` | string | dialect version |
| `target_device` | path | device project under test |
| `test_case` | map | `id`, `description`, `sequence` |

A `sequence` step:

| Key | Value | Meaning |
|-----|-------|---------|
| `inject_packet` | map | `direction`, `type`, `override_payload` (a template + bad fields) |
| `assert_behavior` | map | expected guest state, e.g. `kernel_crash: false`, `driver_status: "stalled"` |

```yaml
target_device: "devices/keyboard_04d9_1702.yaml"
test_case:
  id: "Stress Test"
  sequence:
    - inject_packet:
        direction: "device_to_host"
        type: "control_response"
        override_payload:
          template: "usb_base.configuration_descriptor"
          wTotalLength: 0xFFFF        # oversized on purpose
          bNumInterfaces: 2
    - assert_behavior:
        kernel_crash: false
        driver_status: "stalled"
```
