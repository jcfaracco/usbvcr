# TODO

Tracked work items found across the codebase. Each entry references the
source location where the gap was identified, estimates implementation complexity,
and describes what is missing and why it matters.

---

## High Priority

### `[yaml/diagnostics]` Rich error diagnostics, dry-run validation, and semantic descriptor checks
**File:** `src/lib/usb_node.c:337-375`, `src/lib/yaml/tree.c`, `src/backends/yaml_backend.c:345`, `src/main.c`
**Complexity:** Medium

When a device YAML has missing required properties, type mismatches, or malformed
scalars, `usb_node_validate()` immediately returns `-1` on the first encountered
`USB_PROP_UNKNOWN` with only a debug-level log. The backend simply outputs:
`"There are some NULL props in devices for <path> (`--debug` shows them)."`.
Furthermore, there is no standalone `--check` or `--dry-run` CLI mode to inspect
descriptors without immediately attempting a network connection.

**Why it matters:**
1. **Unfilled `null` placeholders break binary wire layout:** Templates use `null` as abstract parameters (e.g. `idVendor: null`). If left unfilled, `usb_node_serialize()` skips the field completely, shrinking the descriptor byte length and corrupting all subsequent field offsets.
2. **No `--check` / `--dry-run` flag:** Callers cannot validate manifests in CI or scripts without launching a real QEMU connection.
3. **No string descriptor validation:** String index references (`iManufacturer`, `iProduct`, `iSerialNumber`) are not verified against declared entries in the `strings:` map.
4. **No spec-level validation:** Descriptor lengths (`bLength != 18` for device descriptor, `9` for config/interface, `7` for endpoints) and standard descriptor types are not verified before serialization.
5. **No source location:** No file path, line, or column numbers reported when validation fails.

**Target Improvements:**
* Add `--check` / `--dry-run` CLI flag to parse, validate, and exit with code 0/1 without starting the execution engine.
* Implement a diagnostic collector (`GList` of formatted errors) that walks the entire tree and accumulates all invalid/null properties.
* Validate string descriptor index references and string table integrity.
* Print standard GCC-style diagnostic messages:
  `<file>:<line>:<col>: error: field '<name>' in <descriptor_path> is undefined or malformed (expected uint8_t, got null)`
* Add template cross-validation for mandatory USB descriptor fields before attempting serialization.

---

### `[yaml/report_descriptor]` Hierarchy decoupling and per-interface HID report descriptors
**File:** `src/lib/usb_node.c:281-287`, `src/lib/yaml/tree.c`, `docs/yaml_dialect.md:140, 223`, `data/devices/`
**Complexity:** Medium

`docs/yaml_dialect.md` documents `report_descriptor` inside an `interface` (e.g.
`$device.configurations[0].interfaces[0].report_descriptor`), which aligns with the
USB HID specification where report descriptors belong to specific HID interfaces.
However, `usb_node_serialize()` in `src/lib/usb_node.c` recursively flattens and serializes
every child node and `USB_PROP_ARRAY` in a configuration descriptor tree. Consequently,
if `report_descriptor` is placed inside an interface, its binary bytes are appended
directly into the configuration descriptor TLV stream returned on `GET_DESCRIPTOR`
(type `0x02`), corrupting the configuration descriptor wire layout.

To bypass this serialization bug, YAML authors moved `report_descriptor` to the root
level of device manifests (`$device.report_descriptor`). This hack causes architectural
limitations:
1. It contradicts the canonical documentation.
2. It breaks multi-interface composite devices (e.g. keyboard + mouse, dual joysticks)
   because a device cannot define distinct per-interface report descriptors under a
   single root-level key.

**Target Improvements:**
* Tag non-TLV interface child descriptors (such as HID report descriptors or raw blobs)
  with a distinct property type or attribute so that `usb_node_serialize()` skips them
  during configuration descriptor serialization.
* Restore `report_descriptor` inside `interfaces:` as the canonical location across
  `data/` and `examples/`.
* Support resolving `$device.configurations[C].interfaces[I].report_descriptor` in `yaml_backend.c`
  and update `enumeration_flow.yaml` to address interface report descriptors properly.

---

### `[flow/ipc]` Eliminate in-band `$device.*` ASCII payload channel and byte collision risk
**File:** `src/lib/flow.c:961-967`, `src/backends/yaml_backend.c:176-200`
**Complexity:** Medium

In `flow_control()`, when an action matches `ACT_RESPOND_WITH` with a symbolic path
(such as `"$device.device_descriptor"`), `flow.c` writes the ASCII string path
directly into the control transfer's raw payload buffer (`data`).
`yb_control()` in `yaml_backend.c` intercepts the response by checking
`if (rc == 0 && *len > 0 && data[0] == '$')` and performs the string split and node
tree lookup.

**Why it matters:**
1. **Layer violation:** The data packet output buffer is being reused as an internal IPC
   channel between the flow state machine and the backend serializer.
2. **Data collision hazard:** If any USB device or vendor class request legitimately
   returns payload data whose first byte is ASCII `'$'` (`0x24`), `yaml_backend.c`
   will erroneously intercept the packet, attempt to parse it as a `$device.*` path,
   fail resolution, and drop or stall the transfer.

**Target Improvements:**
* Decouple symbolic descriptor reference resolution from the raw payload buffer.
* Pass descriptor references out-of-band via an explicit return structure or tagged
  result enum in `flow_control()` rather than writing strings into the wire buffer.

---

### `[transport/send_all]` Handle partial TCP writes and `SIGPIPE` in `usb_conn_send()`
**File:** `src/core/transport.c:147-151`
**Complexity:** Low

`usb_conn_send()` invokes `send(sock, frame, frame_len, 0)` only once. On TCP stream sockets,
`send()` is not guaranteed to transmit the full buffer in a single system call (particularly
during large isochronous video frame bursts or bulk SCSI data transfers where `frame_len`
approaches 128 KB). If `send()` returns a partial write (`0 < sent < frame_len`), the remaining
unsent bytes are dropped without retry, permanently desynchronizing the usbredir wire protocol.
Additionally, if the peer (e.g. QEMU) terminates abruptly, the next write raises `SIGPIPE`
and terminates the process without cleanup.

**Target Improvements:**
* Introduce a robust `send_all(int sock, const void *buf, size_t len)` helper (mirroring `recv_all()`)
  that loops until the entire buffer is sent or an unrecoverable error occurs.
* Use `MSG_NOSIGNAL` on `send()` calls (or ignore `SIGPIPE` globally) so that broken connections
  return `-1` with `EPIPE` rather than killing the process.

---

### `[record/signals]` Graceful signal handling in `main.c` to prevent corrupted `.pcapng` captures
**File:** `src/main.c:270-320`, `src/lib/pcap/record.c`
**Complexity:** Low

When running the default `qemu` engine with `--record <file.pcapng>`, users routinely terminate
the emulation session by pressing `Ctrl+C` (`SIGINT`). Because `main.c` does not install a
signal handler for `SIGINT` or `SIGTERM`, the process exits immediately via the kernel's default
signal handler. Consequently, `record_close()` is never executed: in-flight buffered packets are
dropped, block headers are left unfinished, and PCAPNG section headers/interfaces are not
finalized, creating corrupted or unreadable capture files.

**Target Improvements:**
* Install a top-level `sigaction` handler in `main.c` that intercepts `SIGINT` and `SIGTERM`.
* Set an atomic shutdown flag to break the engine run loop cleanly, or guarantee that
  `record_close()` runs via an `atexit()` handler or signal-safe flush.

---

### `[yaml/loader]` `parse_tests()` is a no-op stub
**File:** `src/lib/yaml/loader.c:101-105`
**Complexity:** Medium

```c
static int parse_tests(const char *path, yaml_device_t *dev)
{
    (void)path; (void)dev;
    return 0; /* TODO: build ValidationSuite (README §3.4) */
}
```

The `tests/` directory is dir-scanned and each `.yaml` file is handed to
`parse_tests`, but the function does nothing. The `ValidationSuite` described
in README §3.4 has no implementation. Test files are silently ignored at
runtime.

---

## Medium Priority


### `[style/memory]` Standardize memory management on GLib primitives and `g_auto*` macros
**File:** `src/core/usb_core.c:194, 558, 611`, `src/lib/pcap/pcap.c`, `src/lib/`
**Complexity:** Medium

The codebase currently mixes standard libc allocators (`malloc()`, `free()`, `strdup()`)
with the GLib allocation ecosystem (`g_new0()`, `g_free()`, `g_strdup()`).
For example, `usb_core.c` and `pcap.c` allocate wire packet buffers using `malloc()` / `free()`,
while `yaml_backend.c` and `flow.c` rely on GLib structures.
Standardizing across the entire codebase on GLib memory primitives and modern automatic
cleanup macros (`g_autofree`, `g_autoptr`) prevents accidental allocator mismatches and
eliminates boilerplate cleanup in error handling code paths.

---

### `[api/error-handling]` Standardize error propagation across parsing APIs using `GError`
**File:** `src/lib/yaml/loader.c`, `src/lib/yaml/tree.c`, `src/lib/flow.c`, `src/lib/template.c`
**Complexity:** Medium

Public parsing and loading functions (`yaml_tree_load()`, `flow_load()`, `template_register()`,
`usb_node_validate()`) currently return a bare `-1` or `NULL` upon failure, losing the specific
cause of error. Callers are unable to distinguish between I/O errors, YAML syntax errors,
template validation failures, or missing files. Standardizing these APIs to follow the GLib
`GError **error` convention (`USBVCR_ERROR` quark) enables rich, context-aware error propagation
directly up to CLI diagnostic outputs and test suites.

---

### `[yaml/loader]` Null `length`/`count` fields are not auto-filled
**File:** `src/lib/yaml/loader.c:9`
**Complexity:** Medium

README §3.1 specifies that null `length` or `count` fields in a device YAML
should be automatically derived from the actual serialized byte length of the
surrounding descriptor. Currently, fields left unset in the YAML stay zero in
the serialized descriptor, producing malformed output that may confuse a host
or silently truncate data.

---

### `[engine/multi-device]` Concurrent multi-device emulation across multiple ports/connections
**File:** `src/lib/yaml/loader.c:480-495`, `src/backends/yaml_backend.c`, `src/main.c`, `src/engine/`
**Complexity:** Medium

A project manifest's `devices:` block is parsed as a list of `{device, flow}` pairs,
but `yaml_loader.c` currently aliases only the first entry (`dev->tree = first->tree`)
and `main.c` launches a single engine on a single TCP port. Connecting multiple
devices (e.g., three MSC drives on ports 4000, 4001, and 4002) currently requires
spawning separate `usbvcr` processes.

**Target Improvements:**
* Allow per-device `connection:` blocks in `devices:` (e.g. `{ device: "...", connection: { port: "4001" } }`).
* Support spawning and multiplexing multiple engine/usbredir instances in an event-driven loop within a single `usbvcr` process.
* Enable multi-device testing from a single unified project manifest.

---

### `[yaml_backend/perf]` Pre-resolve `$device.*` descriptor references at load time
**File:** `src/backends/yaml_backend.c:176-238`
**Complexity:** Low

In `yb_control()`, responding with `$device.device_descriptor` or `$device.configurations[0]`
executes `g_strsplit()` and walks the `usb_node_t` property tree on the hot path for
every single control transaction. Resolving these symbolic references once during
flow loading and caching direct byte slice pointers will eliminate repeated heap
allocations and string parsing during USB enumeration.

---

### `[flow/timing]` Asynchronous tick-driven delayed responses and state dwell times
**File:** `src/lib/flow.c:34-47, 504-573, 695-725`, `src/lib/flow.h`, `src/core/usb_core.c:250-289, 638-650`
**Complexity:** Medium

Real USB hardware exhibits non-zero processing latencies (EEPROM reads, sensor configuration,
flash write delays, bootloader handoffs), and OS drivers often contain subtle race conditions
or timeout handlers during probing. Currently, all responses in `flow.c` and `usb_core.c` are
emitted immediately and synchronously. Adding support for temporal state delays and paced
queue emission allows high-fidelity hardware modeling and targeted driver resilience testing.

**Target Improvements:**
* **Response Delays (`delay_ticks`):** Add optional `delay_ticks` to response actions (`ACT_RESPOND_WITH`, `ACT_RESPOND_WITH_STATUS`).
* **Asynchronous Completion Queue:** In `usb_core.c`, leverage usbredir packet IDs to buffer deferred responses in a pending queue `(sock, id, setup, data, deadline_tick)` rather than replying immediately. Flush ready packets during `usb_core_run()` tick processing. Never use blocking `sleep()`/`usleep()` calls, which freeze the single-threaded event loop.
* **State Dwell Times (`dwell_ticks` & `auto_transition`):** Allow a state to define a mandatory dwell duration before automatically advancing to another state (e.g. modeling firmware boot stages or sensor stabilization).
* **Endpoint Pacing (`interval_ticks`):** Add inter-packet pacing to `queue_endpoint_data` so bursts of keystrokes or sensor reports are emitted over realistic time intervals.
* **Determinism for Fuzzing:** Quantize all delays in discrete logical event-loop ticks (socket timeout intervals) rather than wall-clock time, preserving reproducible execution across runs.

---

### `[yaml_backend]` Descriptor lookup does not handle `BOS` or `Device Qualifier`
**File:** `src/backends/yaml_backend.c` — `yb_get_descriptor()`
**Complexity:** Low

The `get_descriptor` op handles `type == 1` (DEVICE), `type == 2` (CONFIG),
and `type == 3` (STRING). Other standard descriptor types — BOS (`0x0F`),
Device Qualifier (`0x06`) — are returned as "not found". Hosts that
issue `GET_DESCRIPTOR` for BOS will receive a stall.

---

### `[transport]` `connect_to_server()` only supports IPv4
**File:** `src/core/transport.c:38`
**Complexity:** Low

`hints.ai_family = AF_INET` is hard-coded. IPv6 hosts (or hosts that only
resolve to AAAA records) will fail to connect. Changing to `AF_UNSPEC` and
iterating the `getaddrinfo` result list would handle both address families.

---

### `[scripts/pcap_to_yaml]` Automated tests for PCAP-to-YAML heuristics
**File:** `scripts/pcap_to_yaml.py`, `tests/`
**Complexity:** Medium

The `scripts/pcap_to_yaml.py` conversion script uses several heuristics to infer
symbolic devices, configurations, HID report descriptors, and operational flow
rules from raw usbmon capture files. Currently, there is no automated test suite
to validate these heuristics against edge cases (e.g., malformed captures, complex
or vendor-specific HID reports falling back to raw payloads, multi-interface
devices, non-standard control requests, and round-trip fidelity against existing
test captures like `tests/data/pcap/hid_capture.pcapng`). A dedicated Python test
suite should be added to exercise and validate each heuristic stage.

---

### `[ffs]` FunctionFS descriptors are emitted for FS and HS only
**File:** `src/lib/ffs/ffs.c` — `ffs_build_descriptors()`
**Complexity:** Medium

The `usb_functionfs_descs_head_v2` is built with
`FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC`. SuperSpeed (SS) and
SuperSpeedPlus (SSP) descriptor sets (`FUNCTIONFS_HAS_SS_DESC`,
`FUNCTIONFS_HAS_MS_OS_DESC`) are not provided. A USB 3.x UDC will not
enumerate correctly.

---

### `[yaml/templates]` Align template registry parser with documented `templates:` block
**File:** `src/lib/template.c:37-50`, `docs/yaml_dialect.md:75-90`, `data/templates/`
**Complexity:** Low

`docs/yaml_dialect.md` documents reusable templates wrapped in a `templates:` dictionary
block under the namespace:
```yaml
namespace: "usb_base"
templates:
  device_descriptor: ...
```
However, `template_register()` in `src/lib/template.c` registers top-level properties
directly as `"%s.%s"`. Wrapping descriptors inside `templates:` results in registering
`"usb_base.templates"`, which fails all subsequent `extends: "usb_base.device_descriptor"`
lookups. Built-in templates in `data/templates/` currently omit the wrapper and place
descriptors directly at the root.

**Target Improvements:**
* Update `template_register()` to inspect whether a child node named `templates` is present.
  If present, iterate and register its children; if absent, retain root property registration
  for backward compatibility.
* Ensure documentation and template asset files are synchronized.

---

### `[yaml/schema]` Harmonize `device_descriptor` sequence vs. mapping representation
**File:** `src/lib/yaml/tree.c:514-578`, `docs/yaml_dialect.md:119`, `data/devices/`
**Complexity:** Low

`docs/yaml_dialect.md` defines `device_descriptor` as a `map`. While examples in `examples/msc/`
and `tests/data/example2/` define it as a map (`device_descriptor:\n  idVendor: ...`),
canonical data files in `data/devices/` (keyboard, mouse, uvc_camera) and `examples/composite/`
define it as a sequence containing a single map (`device_descriptor:\n  - bLength: 18`).
While `find_node_by_name_and_index()` currently accommodates both by searching for index 0,
this inconsistency creates divergent AST structures (`USB_PROP_NODE` list append vs set)
and complicates static linting and schema validation.

**Target Improvements:**
* Normalize `device_descriptor` to mapping syntax in tree construction.
* Standardize all `data/devices/*.yaml` manifests to canonical mapping syntax.
* Add validation in `--check` mode to warn when `device_descriptor` is wrapped in a sequence.

---

### `[flow/boilerplate]` Standardize automatic string descriptor fallback to reduce flow duplication
**File:** `src/backends/yaml_backend.c:75-120`, `examples/msc/flows/`, `examples/composite/flows/`
**Complexity:** Low

`yb_get_descriptor()` in `yaml_backend.c` already provides built-in generation of USB String
Descriptors (including LangID 0x0409 and indexing into the `strings:` map). However, sample
flows (such as `msc_flow.yaml` and `composite_flow.yaml`) repeatedly duplicate 30+ lines of
control match rules returning manually encoded UTF-16LE hex strings for standard strings.
Conversely, `data/flows/enumeration_flow.yaml` has no string rules at all, leaving devices
with string indices unhandled if the flow is relied on.

**Target Improvements:**
* Provide transparent fallback in `yaml_backend.c` / `usb_core.c` for standard string queries
  (`wValue == 0x03xx`) from the device's `strings:` table whenever an explicit flow rule
  does not intercept the request.
* Add `$device.strings[N]` symbolic reference support in flow actions to allow explicit rules
  without hardcoding raw UTF-16LE hex bytes.

---

### `[flow/wlength]` Setup packet `wLength` clamping on inline hex responses
**File:** `src/lib/flow.c:955-968`
**Complexity:** Low

When `ACT_RESPOND_WITH` returns a symbolic `$device.*` descriptor, `yaml_backend.c` correctly
clamps the emitted length to the setup packet's `setup->length`. However, when returning an
inline hex payload (`a->respond_bytes`), `flow.c` copies up to the full buffer length
without checking against the requested transfer length in `su` (`wLength`). A host issuing
a short status probe (e.g. 1 or 2 bytes) may receive an oversized packet, leading to host
USB stack desynchronization.

**Target Improvements:**
* Pass the request's `wLength` into flow action execution and clamp `a->respond_bytes`
  transfers: `if (n > req_len) n = req_len;`.

---

### `[local_engine/record]` Live packet recording support in `local_engine` and FunctionFS
**File:** `src/engine/local_engine.c`, `src/lib/ffs/ffs.c`, `src/lib/pcap/record.c`
**Complexity:** Medium

`usbvcr` allows passing `--record <file>` alongside `--local`, and `main.c` successfully
initializes the capture output file. However, `local_engine.c` and `ffs.c` do not integrate
with `src/lib/pcap/record.h`. While `qemu_engine` records transactions during `usb_conn_send()`
and `usb_conn_recv()`, zero packets are captured during gadget execution on real Linux hardware.

**Target Improvements:**
* Hook `record_device_announce()` during initial FunctionFS setup.
* Call `record_conn_send()` / `record_conn_recv()` (or adapt `record_add()`) inside
  `handle_control_event()` and `service_endpoints()` to record live gadget transactions.

---

### `[engine/local_poll]` Event-driven endpoint polling in `local_engine`
**File:** `src/engine/local_engine.c:265-285`
**Complexity:** Medium

In `local_engine_run()`, the main event loop calls `poll()` with a 1-element array containing
only `ctx->ep0_fd`. Active data endpoint file descriptors (`eps`) are never registered in the
`poll()` wait set. Instead, `service_endpoints()` is called unconditionally on every loop
iteration (with a 10ms timeout), causing a 100 Hz busy loop that repeatedly issues non-blocking
`read()` and `write()` calls across all endpoints even when no USB traffic is present.

**Target Improvements:**
* Construct a dynamic `struct pollfd` array in `local_engine_run()` that includes `ep0_fd`
  and all active endpoint file descriptors with appropriate `POLLIN` (OUT endpoints) or
  event readiness flags.
* Only invoke endpoint service handlers when `poll()` reports revents on the corresponding fd.

---

### `[pcap_backend/loop]` Configurable looping/rewind for non-isochronous endpoint traces
**File:** `src/backends/pcap_backend.c:504-512`
**Complexity:** Low

In `pcap_backend`, IN endpoint packet looping is currently hard-coded exclusively for endpoints
detected as isochronous (`st->isoc_endpoints`). For interrupt endpoints (e.g. keyboard keystrokes,
mouse movements) or bulk endpoints, once the captured packets in the trace are read, the queues
remain drained and return NAK (`0`) indefinitely.

**Target Improvements:**
* Add a CLI flag or project setting (e.g. `--loop` or `connection.loop: true`) to allow looping
  all endpoint queues across the entire trace.
* Implement a queue reset/rewind mechanism that reloads or clones endpoint payloads when empty.

---

### `[tests/cli]` End-to-end test suite for `usbvcr` CLI arguments and execution flags
**File:** `src/main.c`, `tests/`
**Complexity:** Low

While `usbvcr` maintains 24 C-level unit and integration tests under `tests/`, there is no
test suite verifying the command-line interface itself (`src/main.c`). Edge cases such as
invalid flag combinations (passing both `--yaml` and `--pcap`), invalid file paths, `--version`,
`--help`, `--check` (once added), and precedence of CLI flags over `connection:` manifest blocks
are currently untested.

**Target Improvements:**
* Add an automated Python or shell test script in `tests/` that executes the built `usbvcr`
  binary with various argument combinations and validates return codes and stderr outputs.

---

## Low Priority

### `[fuzz/cleanup]` Memory cleanup and `fuzz_destroy()` destructor for fuzzing wrapper
**File:** `src/fuzz/fuzz_wrap.c:103-118`, `src/main.c:267-270`
**Complexity:** Low

`fuzz_wrap()` allocates a `usb_device_t` wrapper and a `fuzz_state_t` structure (which holds
a 128 KB internal scratch buffer). No corresponding `fuzz_unwrap()` or `fuzz_destroy()`
function is provided. When fuzzing is enabled (`--fuzz`), this heap allocation is leaked
at session termination.

**Target Improvements:**
* Implement `fuzz_destroy(usb_device_t *dev)` to free `fuzz_state_t` and the wrapper node.
* Call it during `main.c` shutdown.

---

### `[yaml/version]` Clarify or enforce `version: "1.0"` semantics across sub-scopes
**File:** `src/lib/yaml/loader.c:345`, `docs/yaml_dialect.md:18`, `data/`
**Complexity:** Low

`docs/yaml_dialect.md` states that every YAML file starts with `version: "1.0"` and is
validated by `match_version()`. In practice, `match_version()` is only called on the root
manifest (`<project>.yaml`). Almost none of the YAML files in `data/devices/`,
`data/templates/`, `data/flows/`, or `data/codecs/` include a `version:` header.

**Target Improvements:**
* Harmonize the documentation to clarify that `version: "1.0"` is strictly required on
  root project manifests and optional on sub-scope fragments, OR enforce validation
  across sub-files with backward compatibility for existing unversioned templates.

---

*Last updated: 2026-11-28*
