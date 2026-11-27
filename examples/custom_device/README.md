# Custom Vendor-Specific USB Dongle Example

This example demonstrates how to emulate an **unknown or proprietary USB device** (`bDeviceClass: 0xFF`, `bInterfaceClass: 0xFF`) using `usbvcr`'s generic provider architecture.

Instead of writing C code to emulate custom hardware, this project binds its Bulk endpoints to an external protocol service script (`scripts/dongle_service.py`) via the `bind_provider` action with `driver: "exec"`.

---

## Project Structure

```
examples/custom_device/
├── project.yaml             # Project manifest (connection and device mappings)
├── devices/
│   └── custom_dongle.yaml   # USB Descriptors (VID: 0x1234, PID: 0x5678, Class: 0xFF)
├── flows/
│   └── custom_dongle_flow.yaml # State machine binding scripts/dongle_service.py
├── scripts/
│   └── dongle_service.py    # Project-specific protocol handler (stdin/stdout)
└── README.md                # This guide
```

---

## Device Specification

* **Vendor ID:** `0x1234`
* **Product ID:** `0x5678`
* **Device Class:** `0xFF` (Vendor Specific)
* **Endpoints:**
  * Endpoint `0x01` (Bulk OUT): Commands sent from Host to Dongle
  * Endpoint `0x81` (Bulk IN): Responses sent from Dongle to Host

---

## Proprietary Protocol Overview

Communication happens in 4-byte header packets over Bulk endpoints:

| Byte Offset | Field | Description |
|---|---|---|
| `0` | `Command` | `0x01` (PING), `0x02` (VERSION), `0x03` (SENSOR), `0x04` (SET_LED) |
| `1` | `Sequence` | Request sequence number (echoed in response) |
| `2` | `Param 1` | Command-specific parameter (e.g. LED state) |
| `3` | `Param 2` | Optional parameter |

Responses set bit 7 on the command byte (`0x80 | Command`) followed by status and optional payload bytes.

---

## How It Works in YAML

In `flows/custom_dongle_flow.yaml`, once the host selects configuration 1 (`SET_CONFIGURATION`), the flow enters the `operational` state and binds the script:

```yaml
states:
  - name: "operational"
    on_enter:
      - bind_provider:
          driver: "exec"
          endpoints:
            in: 0x81
            out: 0x01
          config:
            command: "python3 scripts/dongle_service.py"
```

* Incoming transfers on **Bulk OUT (`0x01`)** are delivered directly to the script's `stdin`.
* When the host requests **Bulk IN (`0x81`)**, `usbvcr` reads bytes from the script's `stdout` and returns them to the host.

---

## Running the Example

### 1. Start usbvcr

```sh
./build/src/usbvcr --yaml examples/custom_device/project.yaml
```

### 2. Connect via QEMU or Test with PyUSB

In the VM or host attached to the virtual USB bus, you can communicate with the emulated dongle using Python (`pyusb`):

```python
import usb.core
import usb.util
import struct

# Locate the emulated custom device
dev = usb.core.find(idVendor=0x1234, idProduct=0x5678)
if dev is None:
    raise ValueError("Custom dongle not found")

dev.set_configuration()

# 1. Send PING (0x01, seq=42)
dev.write(0x01, struct.pack("<BBBB", 0x01, 42, 0, 0))
reply = dev.read(0x81, 64)
print("Reply to PING:", list(reply))  # [0x81, 42, 0x00, 0x00]

# 2. Query Version (0x02, seq=43)
dev.write(0x01, struct.pack("<BBBB", 0x02, 43, 0, 0))
reply = dev.read(0x81, 64)
cmd, seq, status, length = struct.unpack("<BBBB", reply[:4])
print("Version:", reply[4:4+length].decode())  # "DONGLE_V2.1.0"
```
