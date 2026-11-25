# Virtual USB Joystick Example

This example demonstrates how to emulate a standard USB HID Joystick / Gamepad
device in `usbvcr` with 4 analog axes and 12 digital buttons.

## Structure

- `project.yaml`: Project manifest specifying connection and devices.
- `devices/joystick.yaml`: Device descriptor, HID class descriptor, and 63-byte HID report descriptor.
- `flows/joystick_flow.yaml`: State machine handling enumeration and queueing joystick input events on Endpoint `0x81`.

## Features

- **Axes**:
  - `X`, `Y` analog stick (-127 to +127 signed 8-bit).
  - `Z` (Throttle) and `Rz` (Rudder / Twist) (0 to 255 unsigned 8-bit).
- **Buttons**:
  - 12 digital action buttons (trigger, weapon fire, thumb switches).
- **Compatibility**:
  - Automatically binds to the Linux kernel `joydev` and `evdev` drivers (`/dev/input/js*` and `/dev/input/event*`).

## Running the Example

### Start usbvcr

```bash
usbvcr --yaml examples/joystick/project.yaml
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

### Inside Guest / Host

Verify the joystick device:
```bash
# Check device enumeration
dmesg | grep -i joystick
ls -la /dev/input/js*

# Test joystick events
jstest /dev/input/js0
# or with evtest
evtest /dev/input/eventX
```
