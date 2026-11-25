# TODO

Tracked work items found across the codebase. Each entry references the
source location where the gap was identified and describes what is missing
and why it matters.

---

## High Priority

### `[yaml/loader]` Flow model is not loaded from `flows/` (manifest step 4 is dead)
**File:** `src/lib/yaml/loader.h:39`, `src/lib/yaml/loader.c:10`

`yaml_dev_entry_t.flow` is populated by `flow_load(e->flow_path)` already,
but the YAML loader's own manifest step 4 — scanning the `flows/` directory
and wiring each loaded `Flow*` into the device model — is not implemented.
Devices that rely on a flow for their `ep_in` / `ep_out` / `control`
behaviour will silently get a NULL flow and fall through every match.

---

### `[yaml/loader]` `parse_tests()` is a no-op stub
**File:** `src/lib/yaml/loader.c:101-105`

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

### `[yaml/loader]` Null `length`/`count` fields are not auto-filled
**File:** `src/lib/yaml/loader.c:9`

README §3.1 specifies that null `length` or `count` fields in a device YAML
should be automatically derived from the actual serialized byte length of the
surrounding descriptor. Currently, fields left unset in the YAML stay zero in
the serialized descriptor, producing malformed output that may confuse a host
or silently truncate data.

---

### `[storage/msc]` Automated unit tests for disk_provider and SCSI commands
**File:** `src/lib/storage/disk_provider.c`, `tests/`

The `disk_provider` component implements SCSI transparent command set (SPC/SBC)
over Bulk-Only Transport (BOT), verified end-to-end against real Linux guest VMs.
A dedicated unit test suite (`tests/lib/test_disk_provider.c`) should be added to
exercise and validate individual SCSI CDB parsing (INQUIRY, READ CAPACITY 10/16,
MODE SENSE, READ 10, WRITE 10, SYNCHRONIZE CACHE, REQUEST SENSE) and CSW tag/residue
pairing against synthetic test images.

---

### `[flow]` Orthogonal (and hierarchical) state machine support
**File:** `src/lib/flow.c`, `src/lib/flow.h`

The current flow state machine is flat: it tracks a single global scalar state
(`flow->state`), which requires duplicating common rules (such as standard
descriptors and control transfers) across all operational states (`idle`,
`streaming`, etc.).

For composite USB devices (e.g., webcam with VideoControl, VideoStreaming,
and Audio interfaces), alternate settings and endpoint lifecycles across
interfaces are completely independent. Supporting **orthogonal state machines**
(concurrent, parallel state machines running per interface or component) and
**hierarchical states** (nested sub-states inheriting rules from parent states)
will prevent combinatorial state explosion and naturally model independent
interface behaviors.

---

### `[yaml_backend]` Descriptor lookup does not handle `BOS` or `Device Qualifier`
**File:** `src/backends/yaml_backend.c` — `yb_get_descriptor()`

The `get_descriptor` op handles `type == 1` (DEVICE), `type == 2` (CONFIG),
and `type == 3` (STRING). Other standard descriptor types — BOS (`0x0F`),
Device Qualifier (`0x06`) — are returned as "not found". Hosts that
issue `GET_DESCRIPTOR` for BOS will receive a stall.

---

### `[transport]` `connect_to_server()` only supports IPv4
**File:** `src/core/transport.c:38`

`hints.ai_family = AF_INET` is hard-coded. IPv6 hosts (or hosts that only
resolve to AAAA records) will fail to connect. Changing to `AF_UNSPEC` and
iterating the `getaddrinfo` result list would handle both address families.

---

### `[scripts/pcap_to_yaml]` Automated tests for PCAP-to-YAML heuristics
**File:** `scripts/pcap_to_yaml.py`, `tests/`

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

The `usb_functionfs_descs_head_v2` is built with
`FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC`. SuperSpeed (SS) and
SuperSpeedPlus (SSP) descriptor sets (`FUNCTIONFS_HAS_SS_DESC`,
`FUNCTIONFS_HAS_MS_OS_DESC`) are not provided. A USB 3.x UDC will not
enumerate correctly.

---

## Low Priority

### `[lib/utils]` `trim_string()` could be replaced by a GLib call
**File:** `src/lib/utils.c:47`

The hand-rolled `trim_string` duplicates functionality already available in
GLib. `g_strstrip()` (in-place) or a combination of `g_strchomp()` /
`g_strchug()` could replace it. Worth evaluating once the dependency on the
`#` comment-stripping behaviour is clarified — GLib's strip does not strip
`#`-delimited comments.

---

### `[fuzz]` Fuzzer has no corpus coverage for multi-configuration devices
**File:** `src/fuzz/fuzz.c` — `fuzz_mutate_descriptor()`

The structure-aware descriptor mutator walks descriptors by `bLength` but
starts at offset 0, implicitly treating the whole buffer as a single
configuration. A device with multiple `bNumConfigurations > 1` and a
wTotalLength-delimited config boundary is not modelled. The corpus should
include multi-config descriptors and the walker should respect
`wTotalLength`.

---

### `[pcap]` pcap writer does not check `fwrite` return values
**File:** `src/lib/pcap/pcap.c` — `pcap_writer_add_pcap()`,
`pcap_writer_add_pcapng()`, `pcap_writer_open_legacy_header()`,
`pcap_writer_open_ng_header()`

All `fwrite` calls in the writer path silently discard their return value.
A full disk or a closed file descriptor would corrupt the output file without
any error being surfaced to the caller. Each write should be checked and the
writer should propagate an error code up to `pcap_writer_add()`.

---

### `[ffs]` `ffs_teardown()` does not check `umount()` / `rmdir()` errors
**File:** `src/lib/ffs/ffs.c` — `ffs_teardown()`

`umount()`, `rmdir()`, and `unlink()` return values are silently discarded.
A failed unmount (e.g. the mount point is busy) leaves a stale gadget bound
to the UDC. Each syscall should be checked and logged on failure.

---

*Last updated: 2026-11-25*
