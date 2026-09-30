---
name: src-file-design
description: Include-dependency map of all .c/.h files under src/. Reference when navigating the C source tree, reasoning about header dependencies, or deciding where a change ripples. Derived from the graphify AST graph (imports edges).
---

# src/ C File Relationship Map

ASCII include graph of every `.c`/`.h` file under `src/`. Arrow `A --> B` means **A `#include`s B** (local project headers only; system/third-party headers like `glib`, `pcap`, `stdlib` omitted). Data source: `graphify-out/graph.json` `imports` edges.

## Layers (bottom = foundation, depended on by many)

```
                              ┌───────────────────────┐
                              │  src/core/protocol.h  │  (foundation, leaf)
                              └───────────────────────┘
                                        ▲
        ┌──────────────┬────────────────┼───────────────┬──────────────┐
        │              │                │               │              │
  descriptor.h     transport.h      device.h        record.h      (direct .c
        │              │                │               │           includers)
        ▼              ▼                ▼               ▼
  descriptor.c     transport.c    ┌── device.h ──┐  pcap/record.c
  usb_core.c       usb_core.c     │  includers:  │
  (protocol.h)     (protocol.h)   │              │
                                  ▼              ▼
                             usb_core.h     pcap_backend.h
                             yaml_backend.h  fuzz_wrap.h
                             main.c
```

## Per-subsystem include edges

### src/core/ (USB protocol runtime)
```
bytes.h        (leaf; syntax-error file, partial extract)
protocol.h     (leaf; foundation header)
device.h       --> protocol.h
descriptor.h   --> protocol.h
descriptor.c   --> descriptor.h
transport.h    --> protocol.h
transport.c    --> transport.h, protocol.h, bytes.h
usb_core.h     --> device.h
usb_core.c     --> usb_core.h, transport.h, descriptor.h, protocol.h, bytes.h
```

This module contains all the generic functions to emulate a USB device properly by defining device in device files, the USB descriptor, how to send data with protocol with transport file, and how to deal with bytes (when we are dealing with raw packets). The usb_core file should handle all the dev_ops implemented by the backends.

In usb_core, the functions should follow a similar implementation of QEMU (object-oriented notation and the struct names are just examples):

```
struct USBDeviceClass {
    /*
     * Called when a packet is canceled.
     */
    void (*cancel_packet)(USBDevice *dev, USBPacket *p);

    /*
     * Attach the device
     */
    void (*handle_attach)(USBDevice *dev);

    /*
     * Reset the device
     */
    void (*handle_reset)(USBDevice *dev);

    /*
     * Process control request.
     * Called from handle_packet().
     *
     * Status gets stored in p->status, and if p->status == USB_RET_SUCCESS
     * then the number of bytes transferred is stored in p->actual_length
     */
    void (*handle_control)(USBDevice *dev, USBPacket *p, int request, int value,
                           int index, int length, uint8_t *data);

    /*
     * Process data transfers (both BULK and ISOC).
     * Called from handle_packet().
     *
     * Status gets stored in p->status, and if p->status == USB_RET_SUCCESS
     * then the number of bytes transferred is stored in p->actual_length
     */
    void (*handle_data)(USBDevice *dev, USBPacket *p);

    void (*set_interface)(USBDevice *dev, int interface,
                          int alt_old, int alt_new);

    /*
     * Called when the hcd is done queuing packets for an endpoint, only
     * necessary for devices which can return USB_RET_ADD_TO_QUEUE.
     */
    void (*flush_ep_queue)(USBDevice *dev, USBEndpoint *ep);

    /*
     * Called by the hcd to let the device know the queue for an endpoint
     * has been unlinked / stopped. Optional may be NULL.
     */
    void (*ep_stopped)(USBDevice *dev, USBEndpoint *ep);

    /*
     * Called by the hcd to alloc / free streams on a bulk endpoint.
     * Optional may be NULL.
     */
    int (*alloc_streams)(USBDevice *dev, USBEndpoint **eps, int nr_eps,
                         int streams);
    void (*free_streams)(USBDevice *dev, USBEndpoint **eps, int nr_eps);

    const char *product_desc;
    const USBDesc *usb_desc;
}
```

QEMU also has a nice machine state for packets that can probably be useful for record files either.

```
typedef void USBCallback(USBPacket * packet, void *opaque);

typedef enum USBPacketState {
    USB_PACKET_UNDEFINED = 0,
    USB_PACKET_SETUP,
    USB_PACKET_QUEUED,
    USB_PACKET_ASYNC,
    USB_PACKET_COMPLETE,
    USB_PACKET_CANCELED,
} USBPacketState;

/* Structure used to hold information about an active USB packet.  */
struct USBPacket {
    /* Data fields for use by the driver.  */
    int pid;
    uint64_t id;
    USBEndpoint *ep;
    unsigned int stream;
    QEMUIOVector iov;
    uint64_t parameter; /* control transfers */
    bool short_not_ok;
    bool int_req;
    int status; /* USB_RET_* status code */
    int actual_length; /* Number of bytes actually transferred */
    /* Internal use by the USB layer.  */
    USBPacketState state;
    USBCombinedPacket *combined;
    QTAILQ_ENTRY(USBPacket) queue;
    QTAILQ_ENTRY(USBPacket) combined_entry;
};

struct USBCombinedPacket {
    USBPacket *first;
    QTAILQ_HEAD(, USBPacket) packets;
    QEMUIOVector iov;
};
```


### src/backends/ (output backends, keyed on device.h)
```
pcap_backend.h --> core/device.h
pcap_backend.c --> pcap_backend.h
yaml_backend.h --> core/device.h
yaml_backend.c --> yaml_backend.h
```

The backend files implements the functions used by usb_core like the examples above. The file usb_core cannot implement specific code that can prevent implementing new backends.

The current design of this tool (the backends available), you can reproduce any USB emulation by using YAML high-level description and using pcap to replay USB behaviours.

There are several projects that tries to reconstruct the USB behaviour by reading pcap files like:

- https://github.com/wcooley/usbrevue
- https://github.com/JohnDMcMaster/usbrply (data repo: https://github.com/JohnDMcMaster/usbrply-test)

### src/fuzz/ (fuzzing harness)
```
fuzz.h         (leaf)
fuzz.c         --> fuzz.h, core/protocol.h
fuzz_wrap.h    --> fuzz.h, core/device.h
fuzz_wrap.c    --> fuzz_wrap.h, core/protocol.h
```

By using YAML descriptions, we could easily create fuzzer logics like we have in several project examples:

- https://github.com/purseclab/fuzzusb
- https://github.com/google/syzkaller/blob/master/docs/linux/external_fuzzing_usb.md

### src/lib/ (support library)
```
log.h          (leaf)
log.c          --> log.h
utils.h        (leaf)
utils.c        --> utils.h
version.h      (leaf)
version.c      --> version.h, utils.h
usb_node.h     (leaf)
usb_node.c     --> usb_node.h, log.h
schema.h       --> usb_node.h
schema.c       --> schema.h, log.h
codec.h        (leaf)
codec.c        --> codec.h, log.h, yaml/util.h
flow.h         (leaf)
flow.c         --> flow.h, log.h, utils.h, yaml/util.h
```

In src/lib/, we have all the functions and helpers that controls the main structures of the project (used by main.c). There are the structures of the workflow (defined by flow file). The high-level definition of a USB device in the format of a node (YAML definition converts it into a tree list that can be serialized into a USB device - used by usb_core). For codecs and schemas the idea is the same, convert the definitions described in YAML to the right format of a GLib representation to help us to serialize easily to usb_core. In version files, we handle the YAML manifest version if the dialect changes drastically between versions. There are other helper files like log and utils. They consist in general functions used accross the project.

### src/lib/pcap/ (pcap writer)
```
pcap.h         (leaf)
pcap.c         --> pcap.h
record.h       --> pcap.h, core/protocol.h
record.c       --> record.h
```

In pcap subdir, we have all the helper functions that handle pcap and pcapng files. The pcap files read, open, write and handle those files. In record, we aim precisely how we record USB packets properly. We do not want to create specific code to record packets in usb_core or transport. We should record the packets everytime we send via usb_conn_send or when we receive via usb_conn_recv. No matter which part of the workflow we are in: announcing the device, sending data, etc...

### src/lib/yaml/ (YAML loader)
```
util.h         (leaf; pulled in by lib/codec.c, lib/flow.c)
loader.{c,h}   (no local edges resolved by AST)
tree.{c,h}     (no local edges resolved by AST)
util.c         (no local edges resolved by AST)
```

All the helpers and functions to read and parse YAML files using the defined dialect.

### top level
```
main.c         --> core/device.h
```

## Key observations (for further improvment in the future)

- **`core/protocol.h`** is the root dependency — nearly every core/fuzz/pcap file pulls it in directly or via `device.h`.
- **`core/device.h`** is the public seam between core and the backends/fuzz/main: `pcap_backend.h`, `yaml_backend.h`, `fuzz_wrap.h`, `usb_core.h`, and `main.c` all include it.
- **`core/usb_core.c`** needs to be generic enough to support new backends.
- **`lib/log.h`** is the shared logging leaf across `codec.c`, `flow.c`, `schema.c`, `usb_node.c`.
- **`lib/yaml/util.h`** bridges the YAML layer into `codec.c` and `flow.c`.
- `src/core/bytes.h` and `src/lib/yaml/{loader,tree,util}.c` had no local include edges resolved (bytes.h has a syntax error at L26; yaml internals may only include system headers or were not AST-resolved).

## Regenerate

We use graphify (if installed and available) to understand the edges and code dependency. Prefer graphify plugins to understand code dependencies here.

```
/graphify query "which src files include core/protocol.h"
```
Or rebuild the graph: `/graphify src`.

## Important details

1. Only update this skill if a change is explicitly requested.
2. Try to follow the design principles. If something does not follow that, propose changes and directions before doing any code change.
3. Do not waste tokens by implementing anything without explicitly requested.
