# Virtual Composite Device (UVC Webcam & Telemetry)

This example demonstrates an advanced **USB Composite Device** that showcases all modern `usbvcr` flow state machine features working together:

1. **Common Rules (`common_rules`)**: Global rules for device-level standard enumeration requests (GET_DESCRIPTOR, SET_CONFIGURATION) evaluated across all states and machines.
2. **Orthogonal State Machines (`machines`)**: Multiple concurrent state machines running in parallel for distinct USB interfaces without state-space explosion.
3. **State Inheritance (`extends: <parent_state>`)**: Child states inherit rules and behavior from parent states, overriding only what changes.
4. **Synchronous State Transitions (`transition_state`)**: State changes execute immediately upon matching a packet, ensuring back-to-back packets hit the new state.
5. **Cross-Machine Transitions (`machine.state` or `machine: / state:`)**: One machine can trigger transitions in another machine (e.g. enabling privacy shutter forces video streaming to idle).
6. **Streaming Endpoint Providers (`stream_from_file`)**: Dynamically streaming live MJPEG frames directly from a file to an isochronous endpoint.

---

## Device Architecture

The device presents 3 USB interfaces bundled using an Interface Association Descriptor (IAD):

| Interface | Class | Endpoints | State Machine | Description |
|-----------|-------|-----------|---------------|-------------|
| **Interface 0** | Video Control (`0x0E`/`0x01`) | EP `0x81` (Interrupt IN) | Global / `common_rules` | Camera terminal, processing unit, and selector descriptors |
| **Interface 1** | Video Streaming (`0x0E`/`0x02`) | EP `0x82` (Isochronous IN) | `video_stream` | Manages AltSetting 0 (idle) and AltSetting 1 (streaming video) |
| **Interface 2** | Telemetry / Status (`0xFF`) | EP `0x83` (Interrupt IN) | `camera_telemetry` | Reports camera health, shutter status, and privacy controls |

---

## Flow Architecture

```
                               ┌──────────────────────────────────────────────┐
                               │                 common_rules                 │
                               │  - Standard GET_DESCRIPTOR (device/config)   │
                               │  - Standard SET_CONFIGURATION                │
                               └──────────────────────┬───────────────────────┘
                                                      │
                       ┌──────────────────────────────┴──────────────────────────────┐
                       │                                                             │
                       ▼                                                             ▼
           ┌───────────────────────┐                                     ┌───────────────────────┐
           │ Machine: video_stream │                                     │ Machine: camera_tele  │
           │ (Interface 1)         │                                     │ (Interface 2)         │
           └───────────┬───────────┘                                     └───────────┬───────────┘
                       │                                                             │
       ┌───────────────┴───────────────┐                             ┌───────────────┴───────────────┐
       ▼                               ▼                             ▼                               ▼
 ┌───────────┐  SET_INTERFACE alt=1  ┌───────────┐             ┌───────────┐  Vendor Request (0x10)  ┌───────────────┐
 │   idle    │ ────────────────────▶ │ streaming │             │  normal   │ ──────────────────────▶ │ privacy_mode  │
 │  (NAK)    │ ◀──────────────────── │ (extends: │             │ (telemetry│ ◀────────────────────── │ (extends:     │
 └───────────┘  SET_INTERFACE alt=0  │   idle)   │             │   queue)  │  Vendor Request (0x00)  │    normal)    │
                                     │  [stream] │             └───────────┘                         └───────┬───────┘
                                     └───────────┘                                                           │
                                           ▲                                  cross-machine transition       │
                                           └─────────────────────────────────────────────────────────────────┘
                                                  (Entering privacy_mode forces video_stream to idle)
```

---

## Understanding Flow State Machines: How Device Behavior Works

If USB descriptors describe what a device **looks like** (its vendor ID, product ID, interfaces, and endpoints), the **flow** describes what the device **does** when plugged into a host.

Traditionally, writing USB device firmware requires hundreds of lines of complex C code full of nested `switch/case` statements and state flags. In `usbvcr`, you declare behavior as a **finite state machine (FSM) directly in YAML**.

Here is how each feature works, using this composite webcam as a concrete base:

```
                       ┌────────────────────────┐
   USB Packet In ────▶ │  1. match: condition?  │
                       └───────────┬────────────┘
                                   │ YES
                                   ▼
                       ┌────────────────────────┐
                       │  2. execute actions:   │
                       │     - respond_with     │
                       │     - queue data       │
                       │     - transition_state │
                       └────────────────────────┘
```

### 1. The Core Idea: Match and Act
Every rule in [`flows/composite_flow.yaml`](flows/composite_flow.yaml) has two components:
- **`match:`** — *"When the host sends this packet..."* Matches on setup packet fields (`bmRequestType`, `bRequest`, `wValue`, etc.) or endpoint events (`endpoint_poll`, `endpoint_out`).
- **`actions:`** — *"...do this."* Send data (`respond_with: "$device.device_descriptor"`), acknowledge a status (`respond_with_status: 0`), queue packets (`queue_endpoint_data:`), or stream live video from a file (`stream_from_file:`).

### 2. Common Rules (`common_rules`): "Don't Repeat Yourself"
Standard USB requests (like `GET_DESCRIPTOR` or `SET_CONFIGURATION`) must work in every state. Instead of copy-pasting the same enumeration rules into every state:
```yaml
# Evaluated across ALL states and machines automatically:
common_rules:
  - match:
      bmRequestType: 0x80
      bRequest: 6
      wValue: 0x0100 # GET_DESCRIPTOR (device)
    actions:
      - respond_with: "$device.device_descriptor"

  - match:
      bmRequestType: 0x00
      bRequest: 9
      wValue: 1      # SET_CONFIGURATION(1)
    actions:
      - respond_with_status: 0
```

### 3. Synchronous State Transitions: "Switch Mode Immediately"
When a video player (such as Cheese, VLC, or `mpv`) opens the camera, the Linux kernel sends a `SET_INTERFACE` request with Alternate Setting 1 to allocate video bandwidth. `usbvcr` changes state **synchronously during packet execution**:
```yaml
- match:
    bmRequestType: 0x01
    bRequest: 0x0B # SET_INTERFACE
    wValue: 0x0001 # AltSetting 1 (active video)
    wIndex: 0x0001 # Interface 1
  actions:
    - respond_with_status: 0
    - transition_state: "streaming" # Switched right NOW!
```
Because the transition is synchronous, the very next isochronous packet requested by the kernel on EP `0x82` is serviced by the `streaming` state immediately — with zero frame lag.

### 4. State Inheritance (`extends`): "Subclassing for States"
In USB Video Class, both `idle` and `streaming` states must respond to probe/commit queries and camera controls (brightness, contrast, focus). Copying 30 control rules into both states would be messy and error-prone.

With `extends: "idle"`, the `streaming` state **inherits all parent rules automatically** and only specifies what changes:
```yaml
states:
  - name: "idle"
    rules:
      # ... all UVC probe/commit and unit controls live here ...
      # When idle, endpoint poll returns NAK (no video sent):
      - match: { endpoint_poll: 0x82 }
        actions: [{ return_nak_if_queue_empty: true }]

  # 'streaming' inherits all control rules from 'idle'!
  - name: "streaming"
    extends: "idle"
    rules:
      # Only override EP 0x82: stream live MJPEG frames instead of NAK:
      - match: { endpoint_poll: 0x82 }
        actions:
          - stream_from_file:
              file: "payloads/stream.mjpeg"
              loop: true
```

### 5. Orthogonal State Machines (`machines`): "Multi-tasking Without State Explosion"
This device has two independent interfaces:
- **Interface 1**: Video streaming (idle ↔ streaming)
- **Interface 2**: Camera telemetry (normal ↔ privacy shutter closed)

If combined into a single state machine, you would suffer a combinatorial explosion of states (`idle_normal`, `streaming_normal`, `idle_privacy`, `streaming_privacy`).

With `machines:`, each interface runs its **own independent state machine concurrently**:
```yaml
machines:
  - name: "video_stream"
    interface: 1
    states:
      - name: "idle"
      - name: "streaming"

  - name: "camera_telemetry"
    interface: 2
    states:
      - name: "normal"
      - name: "privacy_mode"
```

### 6. Cross-Machine Transitions: "Coordinating Between Interfaces"
When the user closes a physical privacy shutter, the telemetry interface receives a vendor control request (`bRequest: 0x10`).

Entering `privacy_mode` in `camera_telemetry` can directly force the orthogonal `video_stream` machine into `idle`:
```yaml
- name: "privacy_mode"
  extends: "normal"
  on_enter:
    - queue_endpoint_data:
        endpoint: 0x83
        payloads:
          - hex: "0101" # Telemetry event: Privacy Enabled

    # Cross-machine transition! Forces video_stream to idle:
    - transition_state:
        machine: "video_stream"
        state: "idle"
```

---

## File Layout

```
examples/composite/
├── project.yaml                 # Manifest: device & flow declarations, usbredir port
├── devices/
│   └── composite_device.yaml    # USB descriptor tree (IAD + UVC + Vendor Telemetry)
├── flows/
│   └── composite_flow.yaml      # Multi-machine state machine flow
├── payloads/
│   └── stream.mjpeg             # MJPEG video stream source
└── README.md                    # This documentation
```

---

## Running the Example

### With QEMU (`usbredir`)

Start `usbvcr` targeting the composite project:

```sh
./build/src/usbvcr --yaml examples/composite/project.yaml
```

Attach to your QEMU virtual machine:

```sh
qemu-system-x86_64 \
    -enable-kvm -m 2048 \
    -chardev socket,id=usbredir,host=127.0.0.1,port=4000 \
    -device qemu-xhci \
    -device usb-redir,chardev=usbredir \
    -cdrom linux.iso
```

### Guest Verification

In the Linux guest:

1. **Verify Enumeration**:
   ```sh
   lsusb -v -d 046d:082f
   ```
   You will see both the UVC Video and Vendor Telemetry interfaces bound.

2. **Test Video Streaming**:
   ```sh
   # View streaming video with mpv / ffplay / cheese / gstreamer
   mpv /dev/video0
   ```
   When the guest opens `/dev/video0`, the kernel sends `SET_INTERFACE(alt=1)`, synchronously transitioning `video_stream` into `streaming` and emitting MJPEG frames from `payloads/stream.mjpeg`.
