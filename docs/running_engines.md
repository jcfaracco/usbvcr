# Execution Engines: QEMU (usbredir) and Local Linux Gadget (FunctionFS)

`usbvcr` is designed around a single principle: **YAML/PCAP in, real USB traffic out**. The core emulation layer (`usb_core` + `yaml_backend` / `pcap_backend`) is decoupled from the target transport by the engine interface (`src/core/device.h`):

```
                   ┌─────────────────────────────────────────┐
                   │    YAML Device / Flow  or  PCAP Replay  │
                   └────────────────────┬────────────────────┘
                                        │
                                        ▼
                   ┌─────────────────────────────────────────┐
                   │     usbvcr Core & Backend Translation    │
                   └──────────┬───────────────────┬──────────┘
                              │                   │
                usbredir wire │                   │ FunctionFS ABI
                              ▼                   ▼
                   ┌─────────────────────┐ ┌─────────────────────┐
                   │     QEMU Engine     │ │    Local Engine     │
                   │ (src/engine/qemu_*) │ │(src/engine/local_*) │
                   └──────────┬──────────┘ └──────────┬──────────┘
                              │ TCP                   │ ep0 / epX fds
                              ▼                       ▼
                   ┌─────────────────────┐ ┌─────────────────────┐
                   │  QEMU Virtual Guest │ │ Linux Kernel Gadget │
                   │    (usb-redir)      │ │   (ConfigFS / UDC)  │
                   └─────────────────────┘ └─────────────────────┘
```

---

## 1. QEMU Engine (`usbredir`)

The QEMU engine connects `usbvcr` to virtual machines over TCP using the standard **usbredir** protocol. QEMU handles the virtual USB Host Controller (xHCI/EHCI/UHCI), while `usbvcr` services USB packets over the network socket.

### Basic Usage

Start `usbvcr` specifying the host interface and TCP port to bind:

```sh
# Emulate a YAML project
./build/src/usbvcr --yaml tests/data/example1 --host 127.0.0.1 --port 1337

# Or replay a captured pcap/pcapng trace
./build/src/usbvcr --pcap capture.pcapng --host 127.0.0.1 --port 1337
```

### Launching QEMU

Connect QEMU to `usbvcr` using a TCP character device (`chardev socket`) and attach a `usb-redir` device:

```sh
qemu-system-x86_64 \
  -enable-kvm -m 2G -smp 2 -cpu host \
  -drive file=guest.qcow2,format=qcow2 \
  -chardev socket,id=usbredir_chardev,host=127.0.0.1,port=1337 \
  -device usb-redir,chardev=usbredir_chardev,id=emulated_dev
```

> **Note on Start Order:** By default, `usbvcr` listens on the specified port. If you want QEMU to reconnect automatically or wait for `usbvcr`, you can append `,reconnect=2` to the `-chardev` string in modern QEMU:
> ```sh
> -chardev socket,id=usbredir_chardev,host=127.0.0.1,port=1337,reconnect=2
> ```

### Live Inspection in QEMU Monitor

You can monitor and hotplug the device live via the QEMU monitor (`Ctrl+A, C` in stdio mode or via `-monitor stdio`):

```text
(qemu) info usb
  Device 0.1, Port 1, Speed 480 Mb/s, Product USB VCR Emulated Device, ID: emulated_dev

(qemu) device_del emulated_dev
(qemu) device_add usb-redir,chardev=usbredir_chardev,id=emulated_dev
```

### Recording VM Sessions

Capture all live packets exchanged between the guest and `usbvcr` into a standard pcapng file:

```sh
./build/src/usbvcr --yaml tests/data/example1 --port 1337 --record session.pcapng
```

The resulting `.pcapng` file can be directly opened in Wireshark (using the `USBPCAP` dissector) or fed back into `usbvcr --pcap session.pcapng` for deterministic replay.

---

## 2. Local Linux Gadget Engine (FunctionFS & ConfigFS)

The local engine (`--local`) turns the host machine running `usbvcr` into a physical or virtual USB device using the Linux **ConfigFS** gadget infrastructure and **FunctionFS** (`drivers/usb/gadget/function/f_fs.c`).

No QEMU or virtualization is required: the emulated device appears directly on the Linux host's USB subsystem.

### Kernel Requirements

Your Linux kernel must have USB gadget support enabled:

| Kernel Config Symbol | Description | Required |
|---|---|---|
| `CONFIG_USB_GADGET` | USB Gadget Support | Yes |
| `CONFIG_USB_CONFIGFS` | USB Gadget Configfs support | Yes |
| `CONFIG_USB_CONFIGFS_F_FS` | Function filesystem (FunctionFS) | Yes |
| `CONFIG_USB_DUMMY_HCD` | Virtual USB Host Controller & UDC | Recommended for local testing |

Verify ConfigFS is mounted at `/sys/kernel/config`:
```sh
mountpoint /sys/kernel/config || sudo mount -t configfs none /sys/kernel/config
```

### Testing Setup: Virtual Controller (`dummy_hcd`)

If testing on a desktop or server without a physical USB OTG port, use the Linux `dummy_hcd` driver. It pairs a virtual USB Device Controller (UDC) with a virtual USB Host Controller on the same machine.

1. Load `dummy_hcd`:
   ```sh
   sudo modprobe dummy_hcd
   ```
2. Verify the virtual UDC appeared:
   ```sh
   ls /sys/class/udc/
   # Typical output: dummy_udc.0
   ```
3. Monitor the kernel log in a separate terminal:
   ```sh
   dmesg -w
   ```

### Hardware Setup: USB OTG Devices

On embedded boards with physical USB OTG or Type-C peripheral controllers (e.g., Raspberry Pi 4 / Pi Zero, BeagleBone, Allwinner):

1. Ensure the board's peripheral controller module is loaded (e.g. `dwc2`, `ci_hdrc_imx`, or `dwc3`).
2. Connect the OTG/peripheral port to the target host computer with a data-capable USB cable.
3. Check available UDCs:
   ```sh
   ls /sys/class/udc/
   # Examples: fe980000.usb, 20980000.usb, etc.
   ```

### Running `usbvcr --local`

Because ConfigFS manipulation and FunctionFS filesystem mounting require root privileges, execute with `sudo`:

```sh
# Auto-detects the first available UDC in /sys/class/udc/
sudo ./build/src/usbvcr --yaml tests/data/example1 --local

# Or specify a target UDC explicitly
sudo ./build/src/usbvcr --yaml tests/data/example1 --local --udc dummy_udc.0
```

### What Happens Behind the Scenes

```
1. ConfigFS Setup     Creates /sys/kernel/config/usb_gadget/<name>
                      Sets idVendor, idProduct, and string descriptors.
                      Creates configuration c.1 and functions/ffs.<name>.
                      Links functions/ffs.<name> -> configs/c.1/ffs.<name>.

2. FunctionFS Mount   Mounts functionfs filesystem at /dev/ffs-<name>.

3. Descriptor Write   Serializes FunctionFS v2 binary descriptors + strings
                      and writes them to /dev/ffs-<name>/ep0.

4. UDC Binding        Echoes target UDC name into /sys/kernel/config/.../UDC.
                      The kernel initiates the USB enumeration sequence.

5. Event Loop         Polls /dev/ffs-<name>/ep0 for host setup packets and
                      services IN/OUT endpoints via /dev/ffs-<name>/epX.

6. Teardown (Ctrl+C)  Unbinds UDC (echo "" > UDC), closes ep fds,
                      unmounts /dev/ffs-<name>, and cleans up ConfigFS directories.
```

### Verifying Enumeration

While `usbvcr --local` is running, check the local USB bus:

```sh
lsusb
# Example: Bus 002 Device 005: ID 04d9:1702 Holtek Semiconductor, Inc.

lsusb -v -d 04d9:1702
```

In `dmesg -w`, you will see the host controller probe and attach the appropriate class driver (such as `usbhid` or `uvcvideo`).

### Capturing Local Gadget Traffic with `usbmon`

You can trace all gadget packets directly on the Linux host using the kernel's `usbmon` interface:

1. Load `usbmon`:
   ```sh
   sudo modprobe usbmon
   ```
2. Identify the bus number corresponding to your virtual or physical controller from `lsusb`.
3. Capture traffic with `tcpdump` or Wireshark:
   ```sh
   sudo tcpdump -i usbmon2 -w gadget_traffic.pcap
   ```

---

## 3. Engine Comparison & Troubleshooting

| Feature | QEMU Engine (`usbredir`) | Local Engine (`FunctionFS`) |
|---|---|---|
| **Primary Use-Case** | Hypervisor VM testing, KASAN fuzzing, regression CI | Real hardware emulation, testing host USB stack directly |
| **Permissions** | Normal unprivileged user | Root (`sudo`) required for ConfigFS & mount |
| **Transport** | TCP socket (usbredir wire protocol) | Kernel character devices (`ep0`, `epX`) |
| **Virtual Testing** | Supported out of the box with QEMU | Requires `dummy_hcd` kernel module |
| **Capture Options** | Built-in `--record session.pcapng` | `usbmon` / Wireshark on host bus |

### Troubleshooting Common Issues

* **`ffs: failed to setup FunctionFS / ConfigFS`**:
  - Ensure ConfigFS is mounted: `sudo mount -t configfs none /sys/kernel/config`.
  - Check that kernel module `dummy_hcd` (or your board's UDC driver) is loaded and `/sys/class/udc/` is not empty.
  - Verify that the target UDC is not already claimed by another gadget (`cat /sys/kernel/config/usb_gadget/*/UDC`).

* **QEMU fails to connect to port 1337**:
  - Ensure `usbvcr` is started before QEMU, or configure `,reconnect=2` on the QEMU `-chardev socket` option.
  - Check firewall settings if connecting across different machines or network namespaces.
