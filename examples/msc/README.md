# Virtual USB Flash Drive (MSC Pendrive) Example

This example demonstrates how to emulate a USB Mass Storage Class (MSC) flash drive
(pendrive) using the Bulk-Only Transport (BOT) protocol and SCSI transparent command set,
leveraging **schema templates (`schemas/`)** to cleanly derive multiple devices without
descriptor duplication.

## Directory Structure

```
examples/msc/
├── schemas/
│   └── msc_disk.yaml          # Base schema template (namespace: "msc")
├── devices/
│   ├── msc_sandisk.yaml       # SanDisk Cruzer Blade (uses msc.* templates)
│   ├── msc_kingston.yaml      # Kingston DataTraveler (uses msc.* templates)
│   └── msc_corsair.yaml       # Corsair Flash Voyager (uses msc.* templates)
├── flows/
│   └── msc_flow.yaml          # Bulk-Only Transport state machine & SCSI disk handler
├── payloads/
│   └── disk.img               # 10 MiB disk image formatted with MBR + FAT16 (HELLO.TXT)
├── project.yaml               # Port 4000 manifest (SanDisk)
├── project_kingston.yaml      # Port 4001 manifest (Kingston)
└── project_corsair.yaml       # Port 4002 manifest (Corsair)
```

---

## Schema Templating Architecture

### 1. Base Schema Template (`schemas/msc_disk.yaml`)

Defines reusable USB Mass Storage descriptor templates under the `msc` namespace:
- `msc.device_descriptor`: Standard USB 2.0 device descriptor with `idVendor: null` and `idProduct: null`.
- `msc.configuration_descriptor`: Standard 1-interface, 2-endpoint Bulk-Only Transport layout (`0x08:0x06:0x50`, EPs `0x81` and `0x02`, 512-byte max packet size).

```yaml
namespace: "msc"

device_descriptor:
  bLength: 18
  bDescriptorType: 0x01
  bcdUSB: 0x0200
  bDeviceClass: 0x00
  bMaxPacketSize0: 64
  idVendor: null
  idProduct: null
  bcdDevice: 0x0100
  ...
```

### 2. Concrete Devices (`devices/`)

Each device references the templates and overrides only its own specific identifiers:

```yaml
# devices/msc_sandisk.yaml
device_descriptor:
  template: "msc.device_descriptor"
  idVendor: 0x0781          # SanDisk Corp.
  idProduct: 0x5567         # Cruzer Blade

configurations:
  - template: "msc.configuration_descriptor"
```

```yaml
# devices/msc_kingston.yaml
device_descriptor:
  template: "msc.device_descriptor"
  idVendor: 0x0951          # Kingston Technology
  idProduct: 0x1666         # DataTraveler 100 G3

configurations:
  - template: "msc.configuration_descriptor"
```

```yaml
# devices/msc_corsair.yaml
device_descriptor:
  template: "msc.device_descriptor"
  idVendor: 0x1b1c          # Corsair
  idProduct: 0x1a03         # Flash Voyager

configurations:
  - template: "msc.configuration_descriptor"
```

---

## Running Three Devices Simultaneously (Ports 4000, 4001, 4002)

### 1. Launch QEMU with 3 Redirection Ports

```bash
qemu-system-x86_64 \
    -enable-kvm -m 2048 \
    -device qemu-xhci,id=xhci \
    -chardev socket,id=usbredir0,host=127.0.0.1,port=4000,server=on,wait=off \
    -device usb-redir,bus=xhci.0,chardev=usbredir0 \
    -chardev socket,id=usbredir1,host=127.0.0.1,port=4001,server=on,wait=off \
    -device usb-redir,bus=xhci.0,chardev=usbredir1 \
    -chardev socket,id=usbredir2,host=127.0.0.1,port=4002,server=on,wait=off \
    -device usb-redir,bus=xhci.0,chardev=usbredir2 \
    -cdrom linux.iso
```

### 2. Start the Three USB VCR Instances

In three separate terminals:

```bash
# Terminal 1: Port 4000 (SanDisk)
usbvcr --yaml examples/msc/project.yaml

# Terminal 2: Port 4001 (Kingston)
usbvcr --yaml examples/msc/project_kingston.yaml

# Terminal 3: Port 4002 (Corsair)
usbvcr --yaml examples/msc/project_corsair.yaml
```

### 3. Verification inside the Guest VM

```bash
# All three devices will be listed with their respective vendor and product IDs:
lsusb
# ID 0781:5567 SanDisk Corp. Cruzer Blade
# ID 0951:1666 Kingston Technology DataTraveler 100 G3
# ID 1b1c:1a03 Corsair Flash Voyager

# Check SCSI block device attachments
dmesg | grep -i "usb-storage"
lsblk
```
