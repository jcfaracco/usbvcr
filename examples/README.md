# USB VCR Examples

This directory contains complete, ready-to-run USB device emulation projects demonstrating `usbvcr`'s canonical YAML dialect, descriptor authoring, declarative state machine flows, and payload streaming.

---

## Available Examples

| Example | Class | Description | Backing Assets |
|---------|-------|-------------|----------------|
| [**joystick/**](joystick/README.md) | HID (`0x03`) | 4-axis analog joystick with 12 digital buttons. Emulates input events queued on interrupt endpoint `0x81`. Binds to Linux `joydev` / `evdev`. | None (declarative events) |
| [**msc/**](msc/README.md) | Mass Storage (`0x08`) | USB flash drive (pendrive) using Bulk-Only Transport (BOT) and SCSI transparent commands (SPC/SBC). Mounts as a block device. | `payloads/disk.img` (10 MiB FAT16 image) |
| [**composite/**](composite/README.md) | Composite (`0xEF`) | Multi-interface UVC webcam + Telemetry device showcasing **all** flow FSM features: orthogonal machines (`machines`), global `common_rules`, state inheritance (`extends`), synchronous transitions, and cross-machine transitions. | `payloads/stream.mjpeg` (Motion-JPEG video) |
| [**custom_device/**](custom_device/README.md) | Vendor Specific (`0xFF`) | Unknown / proprietary USB dongle using generic `bind_provider` (`driver: "exec"`). Bridges Bulk I/O to a project-scoped Python protocol service script. | `scripts/dongle_service.py` (Protocol handler) |

---

## Example Project Anatomy

Each example follows the canonical directory layout:

```
examples/<device>/
├── project.yaml     # Manifest specifying project name, connection settings, and device/flow mappings
├── devices/         # Concrete USB descriptor configurations (vendor/product IDs, interfaces, endpoints)
├── flows/           # Declarative behavior state machines (control requests, endpoint queues, streaming)
├── payloads/        # Optional binary assets (disk images, video streams, raw firmware blobs)
└── README.md        # Detailed instructions, driver verification, and test commands for this device
```

---

## Running the Examples

### 1. With QEMU (`usbredir`)

Start `usbvcr` targeting your desired example manifest:

```sh
# Emulate the virtual joystick
./build/src/usbvcr --yaml examples/joystick/project.yaml

# Emulate the MSC pendrive
./build/src/usbvcr --yaml examples/msc/project.yaml

# Emulate the Composite webcam & telemetry
./build/src/usbvcr --yaml examples/composite/project.yaml

# Emulate the Custom vendor-specific dongle
./build/src/usbvcr --yaml examples/custom_device/project.yaml
```

Then attach the device to a QEMU virtual machine via `usb-redir`:

```sh
qemu-system-x86_64 \
    -enable-kvm -m 2048 \
    -chardev socket,id=usbredir,host=127.0.0.1,port=4000 \
    -device qemu-xhci \
    -device usb-redir,chardev=usbredir \
    -cdrom linux.iso
```

*(Note: port and host can be configured in each `project.yaml` or overridden via CLI flags `--host` and `--port`.)*

### 2. With Local Linux USB Gadget (`local`)

Run directly on the host using the Linux FunctionFS / ConfigFS gadget subsystem (using `dummy_hcd` or physical OTG UDC):

```sh
# Load the dummy HCD kernel module if testing locally
sudo modprobe dummy_hcd

# Run usbvcr against the local gadget engine
sudo ./build/src/usbvcr --yaml examples/joystick/project.yaml --local
```

For detailed engine configuration and guest verification commands, consult each example's `README.md` and [docs/running_engines.md](../docs/running_engines.md).
