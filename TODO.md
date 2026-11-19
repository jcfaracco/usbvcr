# TODO

Tracked work items found across the codebase. Each entry references the
source location where the gap was identified and describes what is missing
and why it matters.

---

## High Priority

### `[core]` Speed detection heuristic is unreliable for Full-Speed devices
**File:** `src/core/usb_core.c:113`

Device speed is currently derived from `bcdUSB` / `bMaxPacketSize0`. The
heuristic cannot distinguish a Full-Speed device that happens to advertise
`ep0 maxpacket == 64` (which is also valid for High-Speed). The backend
should be able to supply the actual speed directly — for example from a
transport-layer capability field — overriding the descriptor guess.

---

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

### `[ffs]` FunctionFS strings are hard-coded
**File:** `src/lib/ffs/ffs.c` — `ffs_build_strings()`

The string descriptor written to `ep0` is always `"USB VCR Emulated Device"`
regardless of what the loaded device YAML says. The product/manufacturer
strings from the device descriptor (fetched via `get_descriptor`) should be
used instead, forwarding the real device identity through FunctionFS.

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

### `[flow]` `flow_ep_out()` has no match/action support
**File:** `src/lib/flow.c` — `flow_ep_out()`

Host-to-device OUT transfers (e.g. HID SET_REPORT, interrupt OUT endpoints)
are consumed and logged but never matched against any flow rule. The flow
schema has no OUT match/action type. Devices that need to react to OUT data
(e.g. LED state changes) cannot be modelled.

---

### `[yaml_backend]` Descriptor lookup only handles `DEVICE` and `CONFIG`
**File:** `src/backends/yaml_backend.c` — `yb_get_descriptor()`

The `get_descriptor` op hard-codes `type == 1` (DEVICE) and `type == 2`
(CONFIG). Other standard descriptor types — String (`0x03`), BOS (`0x0F`),
Device Qualifier (`0x06`) — are silently returned as "not found". Hosts that
issue `GET_DESCRIPTOR` for strings or BOS will always get a stall.

---

### `[transport]` `connect_to_server()` only supports IPv4
**File:** `src/core/transport.c:38`

`hints.ai_family = AF_INET` is hard-coded. IPv6 hosts (or hosts that only
resolve to AAAA records) will fail to connect. Changing to `AF_UNSPEC` and
iterating the `getaddrinfo` result list would handle both address families.

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

*Last updated: 2026-11-19*
