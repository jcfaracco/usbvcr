# USB‑redir "Hello World" Example

This repository contains a minimal C program that connects to a QEMU SPICE server, detects the **USB‑redir** channel, and prints a friendly *Hello, SPICE USB‑redir world!* message.  It also includes a Makefile, build instructions, runtime usage, and several ways to **monitor the USB‑redir traffic** (library debug, `usbredirclient`, `tcpdump`, and QEMU tracing).

---

## 1. Directory layout
```
usbredir/
├─ meson.build             # Top-level build definition
├─ README.md               # This document
└─ src/
   ├─ meson.build
   ├─ client/              # SPICE USB-redir "hello" client
   │  ├─ meson.build
   │  ├─ main.c
   │  ├─ connection.c / connection.h
   │  ├─ usb_core.c / usb_core.h
   │  └─ log.h
   └─ pcap/                # Standalone usbmon pcap filter
      ├─ meson.build
      ├─ usb_pcap.c
      └─ usb_pcap.h
```

## 2. Prerequisites (Fedora)
```bash
# Fedora
sudo dnf install -y \
    gcc make pkgconfig \
    glib2-devel libspice-client-glib-devel \
    libusbredir-devel
```

* `glib2-devel` provides `<glib.h>`.
* `libspice-client-glib-devel` gives the SPICE client library.
* `libusbredir-devel` supplies the USB‑redir parser (used for optional debug logging).

## 3. Build (Meson)
```bash
meson setup build
meson compile -C build
```
This produces two executables under `build/src/`:
* `client/usb_redir_hello` – the SPICE USB-redir client.
* `pcap/usb_pcap_filter` – the standalone usbmon pcap filter.

## 4. Run a QEMU VM with a USB‑redir channel
```bash
qemu-system-x86_64 \
    -enable-kvm -m 2G -cpu host \
    -drive file=/path/to/guest.img,format=qcow2,if=virtio \
    -spice port=5930,disable-ticketing=on \
    -device usb-redir,chardev=usbredir0 \
    -chardev socket,id=usbredir0,path=/tmp/usbredir.sock,server,nowait \
    -display none &
```
*The VM runs without a graphical window; the SPICE server listens on port 5930.*

## 5. Run the client
```bash
./usb_redir_hello spice://127.0.0.1:5930
```
Typical output:
```
[+] New channel: usbredir (type=5)
[+] USB‑redir channel is now CONNECTED!
    Hello, SPICE USB‑redir world!
```
The program quits after printing the greeting.

---

## 6. Monitoring the USB‑redir traffic
### 6.1  Library‑level debug (usbredirparser)
```c
#include <usbredirparser.h>
usbredirparser_set_log_level(USBREDIRPARSER_LOG_DEBUG);
usbredirparser_set_log_func((usbredirparser_log_func)fprintf, stderr);
```
Re‑compile after adding the code.  The program will now emit lines such as:
```
[usbredir] TX: 0x55 0x53 0x42 … (44 bytes)
[usbredir] RX: 0x85 0x01 …
```
You can redirect the log to a file if desired.

### 6.2  SPICE client debug output
```bash
export SPICE_DEBUG=1
export SPICE_LOG=debug
./usb_redir_hello spice://127.0.0.1:5930
```
All SPICE framing messages (CHANNEL_OPEN, CHANNEL_DATA, …) appear on stdout.

### 6.3  Host‑side `usbredirclient` verbose mode
```bash
# Forward a real USB device (example: /dev/bus/usb/001/004)
usbredirclient -vv -d /dev/bus/usb/001/004 /tmp/usbredir.sock 2>&1 | tee /tmp/usbredirclient.log
```
The log shows each packet with `>>>` (TX) and `<<<` (RX).

### 6.4  Capture the TCP stream with `tcpdump` / Wireshark
```bash
sudo tcpdump -i any -w /tmp/spice_usbredir.pcap tcp port 5930
```
Open the resulting `*.pcap` in Wireshark and filter with `tcp.port == 5930`.

### 6.5  QEMU tracing (`-trace usb_redir`)
```bash
qemu-system-x86_64 … \
    -trace usb_redir \
    -tracing-file /tmp/qemu_usbredir.trace
```
The binary file contains every USB‑redir event (packet TX/RX, state changes, errors).  Decode it with:
```bash
tracetool -i /tmp/qemu_usbredir.trace -o /tmp/qemu_usbredir.log
less /tmp/qemu_usbredir.log
```
Sample decoded lines:
```
[000001.234567] usb_redir_tx: len=44 data=55 53 42 20 …
[000001.235123] usb_redir_rx: len=8  data=85 01 …
[000001.236000] usb_redir_state: state=CONNECTED
```
You can also enable a subset of events, e.g. `-trace events=usb_redir_tx,usb_redir_rx`.

### 6.6  Quick all‑in‑one monitoring script
```bash
#!/usr/bin/env bash
set -euo pipefail

# start tcpdump in background
sudo tcpdump -i any -w /tmp/spice_usbredir.pcap tcp port 5930 2>/dev/null &
TCPDUMP_PID=$!

# enable SPICE debug output
export SPICE_DEBUG=1

# run the client
./usb_redir_hello spice://127.0.0.1:5930

# stop tcpdump
sudo kill "$TCPDUMP_PID"
echo "Capture saved to /tmp/spice_usbredir.pcap"
```
Save it as `run_monitor.sh`, `chmod +x run_monitor.sh`, and execute it.

---

## 7. FAQ
* **Why do I get `fatal error: glib.h: No such file or directory`?**
  Install `glib2-devel` (Fedora) and ensure the Makefile includes the `glib-2.0` pkg‑config flags (already done in the provided Makefile).
* **Can I use this on a non‑Linux host?**
  The client is portable as long as the required libraries (`glib`, `spice-client-glib`) are available.  On macOS you can install them via Homebrew (`brew install glib spice-client-glib`).
* **Is the binary trace file portable?**
  Yes – it is architecture‑agnostic.  Any machine with QEMU’s `tracetool` can decode it.

---

## 8. License
This example code is released under the **MIT License** – feel free to copy, modify, and distribute.

---

*Happy hacking with SPICE and USB‑redir!*
