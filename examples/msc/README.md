# Virtual USB Flash Drive (MSC Pendrive) Example

This example demonstrates how to emulate a USB Mass Storage Class (MSC) flash drive
(pendrive) using the Bulk-Only Transport (BOT) protocol and SCSI transparent command set.

## Structure

- `project.yaml`: Project manifest specifying connection and devices.
- `devices/msc_disk.yaml`: Device descriptor, MSC interface descriptor (Class `0x08`, Subclass `0x06`, Protocol `0x50`), Bulk IN endpoint `0x81`, and Bulk OUT endpoint `0x02`.
- `flows/msc_flow.yaml`: State machine handling enumeration and Bulk-Only Transport class requests (`GET_MAX_LUN`, `BOT Reset`), with `disk_from_file` attaching `payloads/disk.img`.
- `payloads/disk.img`: 10 MiB disk image formatted with an MBR partition table and FAT16 filesystem containing `HELLO.TXT`.

## Features

- **Class**: USB Mass Storage Class (`0x08`).
- **Subclass**: SCSI Transparent Command Set (`0x06`).
- **Protocol**: Bulk-Only Transport (BBB / BOT) (`0x50`).
- **Endpoints**:
  - `0x81` Bulk IN (512 bytes packet size).
  - `0x02` Bulk OUT (512 bytes packet size).
- **Controls**:
  - Responds to standard USB enumeration requests.
  - Implements BOT `GET_MAX_LUN` (`bRequest=0xfe`, `bmRequestType=0xa1`) reporting 1 LUN (LUN 0).
  - Implements BOT Mass Storage Reset (`bRequest=0xff`, `bmRequestType=0x21`).

## Running the Example

### Start usbvcr

```bash
usbvcr --yaml examples/msc/project.yaml
```

### Connect to QEMU

Launch QEMU with a redirected USB device:
```bash
qemu-system-x86_64 \
    -enable-kvm -m 2048 \
    -chardev socket,id=usbredir,host=127.0.0.1,port=4000 \
    -device qemu-xhci \
    -device usb-redir,chardev=usbredir \
    -cdrom linux.iso
```

### Verification inside Guest

```bash
lsusb -v -d 0781:5567
dmesg | grep -i "usb-storage"
sudo fdisk -l /dev/sdb
sudo mkdir -p /mnt/usb && sudo mount /dev/sdb1 /mnt/usb
cat /mnt/usb/HELLO.TXT
```
