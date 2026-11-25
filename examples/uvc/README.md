# UVC Webcam Video Streaming Example

This example demonstrates how to emulate a USB Video Class (UVC) webcam device
in `usbvcr` using a Motion-JPEG (`.mjpeg`) video file streamed under a declarative
state machine flow.

## Structure

- `project.yaml`: Project manifest specifying connection and devices.
- `devices/uvc_camera.yaml`: Extends the default `uvc_camera` descriptor with custom vendor/product strings.
- `flows/uvc_streaming_flow.yaml`: State machine behavior flow controlling `idle` and `streaming` states.
- `payloads/stream.mjpeg`: Backing Motion-JPEG file streamed over Endpoint `0x82`.

## How It Works

1. **State Machine (`idle` vs `streaming`)**:
   - The device starts in the `idle` state. Endpoints `0x81` (control interrupt) and `0x82` (isochronous video) return NAK while inactive.
   - When the host video driver issues `SET_INTERFACE (altsetting = 1)` to claim video bandwidth, the flow transitions to the `streaming` state.
   - When the host video driver issues `SET_INTERFACE (altsetting = 0)` to close the camera stream, the flow transitions back to `idle`.
2. **Stream Provider Action**:
   - Inside `flows/uvc_streaming_flow.yaml`, the `streaming` state contains:
     ```yaml
     - match:
         endpoint_poll: 0x82
       actions:
         - stream_from_file:
             file: "payloads/stream.mjpeg"
             loop: true
     ```
   - On Endpoint `0x82` IN polls during `streaming`, the stream provider slices frames into packet sizes (`wMaxPacketSize`), prefixes a 2-byte UVC Payload Header (`FID` toggle and `EOF` bit), and streams frames continuously.
3. **Looping**:
   - With `loop: true`, once the final frame reaches EOF, playback automatically wraps back to the first frame while maintaining consecutive FID toggles across loop boundaries.

## Running the Example

### With QEMU (`usbredir`)

Start the `usbvcr` server:
```bash
usbvcr --yaml examples/uvc/project.yaml
```

Launch QEMU with a redirected USB device:
```bash
qemu-system-x86_64 \
    -enable-kvm -m 2048 \
    -chardev socket,id=usbredir,host=127.0.0.1,port=4000 \
    -device qemu-xhci \
    -device usb-redir,chardev=usbredir \
    -cdrom ubuntu.iso
```

Inside the guest, verify the video device:
```bash
v4l2-ctl --list-devices
ffplay /dev/video0
```

### With Linux FunctionFS Gadget (`local`)

Edit `connection:` in `project.yaml` to set `engine: "local"`, then run:
```bash
sudo modprobe dummy_hcd
sudo usbvcr --yaml examples/uvc/project.yaml --engine local --udc dummy_udc.0
```
