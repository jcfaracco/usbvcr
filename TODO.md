# TODO

Tracked work items found across the codebase. Each entry references the
source location where the gap was identified, estimates implementation complexity,
and describes what is missing and why it matters.

---

## High Priority

### `[yaml/diagnostics]` Rich error diagnostics and source-location reporting for malformed device trees
**File:** `src/lib/usb_node.c:282-321`, `src/lib/yaml/tree.c`, `src/backends/yaml_backend.c:308-312`
**Complexity:** Medium

When a device YAML has missing required properties, type mismatches, or malformed
scalars, `usb_node_validate()` immediately returns `-1` on the first encountered
`USB_PROP_UNKNOWN` with only a debug-level log. The backend simply outputs:
`"There are some NULL props in devices for <path> (`--debug` shows them)."`.
This makes debugging malformed manifests difficult because it provides:
1. No source location (file path, line, or column number).
2. No descriptor hierarchy path (e.g. `device -> configurations[0] -> interfaces[0] -> endpoints[1] -> wMaxPacketSize`).
3. Immediate abort on first failure rather than collecting and displaying all validation errors at once.
4. No schema-level checks for missing mandatory USB descriptor fields (e.g., missing `bLength`, `bDescriptorType`, or `bNumInterfaces`).

**Target Improvements:**
* Preserve libyaml `start_mark.line` and `start_mark.column` during `yaml_tree_load()`.
* Implement a diagnostic collector (`GList` of formatted errors) that walks the entire tree and accumulates all invalid/null properties.
* Print standard GCC-style diagnostic messages:
  `<file>:<line>:<col>: error: field '<name>' in <descriptor_path> is undefined or malformed (expected uint8_t, got null)`
* Add schema cross-validation to warn about omitted fields that are required by the USB 2.0 / 3.x specifications before attempting serialization.

---

### `[flow/decouple]` Decouple protocol-specific logic (MSC BOT / SCSI) and streaming from generic flow engine
**File:** `src/lib/flow.c:65, 542-551, 662-667`, `src/lib/flow.h`, `src/lib/storage/disk_provider.c`, `src/lib/stream/stream_provider.c`
**Complexity:** High

The generic flow engine currently has device-class-specific framing and protocol logic
hardcoded directly into its structures and packet handlers:
* `flow_t` carries USB Mass Storage (MSC) Command Block Wrapper (CBW) tracking state (`last_cbw_tag[4]` and `has_cbw_tag`).
* `flow_ep_out()` inspects payload length and checks `memcmp(buf, "USBC", 4) == 0` to intercept MSC CBW tags.
* `execute_actions()` (`ACT_QUEUE_ENDPOINT_DATA`) dynamically rewrites bytes 4..7 of 13-byte Command Status Wrappers (`USBS`) to match the intercepted CBW tag.
* `ACT_DISK_FROM_FILE` and `ACT_STREAM_FROM_FILE` are baked as monolithic action types into `flow_action_type_t`, embedding SCSI command execution (`disk_provider`) and MJPEG parsing (`stream_provider`) inside `flow.c`.
* `stream_provider` is duplicated: checked both as a flow action and separately on `yaml_backend_state_t` (`yb_ep_in` and `yb_tick`).

**Target Improvements:**
* Remove CBW/CSW tag tracking and magic byte checks from `flow.c`.
* Replace `ACT_DISK_FROM_FILE` and `ACT_STREAM_FROM_FILE` with a generic endpoint provider/filter interface (`flow_bind_endpoint(ep, provider_ops)`).
* Encapsulate BOT framing and SCSI CDB handling entirely inside `disk_provider`.
* Unify endpoint streaming and packet consumption into a single dispatch path.

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

### `[architecture/engine-ops]` Standardize execution engines on an `engine_ops_t` interface
**File:** `src/engine/qemu_engine.h`, `src/engine/local_engine.h`, `src/main.c:241-261`
**Complexity:** Medium

Backends are cleanly abstracted behind `usb_device_ops_t` (`yaml_backend`, `pcap_backend`),
allowing `main.c` to swap them seamlessly. However, execution engines (`qemu_engine` vs
`local_engine`) are hardcoded in `main.c` behind branching `if (use_local) ... else ...` blocks.
Introducing a standardized `usb_engine_ops_t` interface (`init`, `run`, `stop`, `cleanup`) will:
* Decouple `main.c` from target engine runtime specifics.
* Allow engines to be selected and configured dynamically via CLI or manifest (`connection.engine`).
* Provide a clean extension point for future execution targets (e.g. USBIP daemon, WebUSB, or Linux kernel vhci-hcd).

---

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
**File:** `src/lib/yaml/loader.c`, `src/lib/yaml/tree.c`, `src/lib/flow.c`, `src/lib/schema.c`
**Complexity:** Medium

Public parsing and loading functions (`yaml_tree_load()`, `flow_load()`, `schema_register()`,
`usb_node_validate()`) currently return a bare `-1` or `NULL` upon failure, losing the specific
cause of error. Callers are unable to distinguish between I/O errors, YAML syntax errors,
schema validation failures, or missing files. Standardizing these APIs to follow the GLib
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

### `[storage/msc]` Automated unit tests for disk_provider and SCSI commands
**File:** `src/lib/storage/disk_provider.c`, `tests/`
**Complexity:** Low

The `disk_provider` component implements SCSI transparent command set (SPC/SBC)
over Bulk-Only Transport (BOT), verified end-to-end against real Linux guest VMs.
A dedicated unit test suite (`tests/lib/test_disk_provider.c`) should be added to
exercise and validate individual SCSI CDB parsing (INQUIRY, READ CAPACITY 10/16,
MODE SENSE, READ 10, WRITE 10, SYNCHRONIZE CACHE, REQUEST SENSE) and CSW tag/residue
pairing against synthetic test images.

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

## Low Priority

*(No open low-priority items currently tracked.)*

---

*Last updated: 2026-11-26*
