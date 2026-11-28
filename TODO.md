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
* Add schema cross-validation for mandatory USB descriptor fields before attempting serialization.

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

## Low Priority

*(No open low-priority items currently tracked.)*

---

*Last updated: 2026-11-27*
