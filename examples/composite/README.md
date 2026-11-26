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
   # View streaming video with mpv / ffplay / gstreamer
   mpv /dev/video0
   ```
   When the guest opens `/dev/video0`, the kernel sends `SET_INTERFACE(alt=1)`, synchronously transitioning `video_stream` into `streaming` and emitting MJPEG frames from `payloads/stream.mjpeg`.
