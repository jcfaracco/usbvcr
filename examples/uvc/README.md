# UVC Webcam Video Streaming Example

This example demonstrates how to emulate a USB Video Class (UVC) webcam device
in `usbvcr` using a Motion-JPEG (`.mjpeg`) video file streamed in a continuous loop.

## Structure

- `project.yaml`: Project manifest specifying connection and devices.
- `devices/uvc_camera.yaml`: Extends the default `uvc_camera` descriptor with custom vendor/product strings.
- `flows/uvc_streaming_flow.yaml`: Behavior flow rules for video streaming endpoints.
- `payloads/stream.mjpeg`: Backing Motion-JPEG file streamed over Endpoint `0x81`.

## How It Works

1. The device descriptor is loaded and inherits standard UVC Video Control (VC) and Video Streaming (VS) interfaces.
2. The `file: "payloads/stream.mjpeg"` property directs the stream provider to scan frame boundaries (`0xFF 0xD8 ... 0xFF 0xD9`).
3. On Endpoint `0x81` IN polls, the stream provider slices frames into packet sizes (`wMaxPacketSize`), prefixes a 2-byte UVC Payload Header (`FID` and `EOF`), and streams frames continuously.
4. With `loop: true`, once the final frame reaches EOF, playback automatically wraps back to the first frame.

## Running the Example

### With QEMU (`usbredir`)

Start the `usbvcr` server:
```bash
usbvcr --yaml examples/uvc_streaming/project.yaml
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
sudo usbvcr --yaml examples/uvc_streaming/project.yaml --engine local --udc dummy_udc.0
```
