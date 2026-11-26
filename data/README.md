# USB VCR Built-in Data Assets (`data/`)

This directory contains `usbvcr`'s built-in standard library of descriptor schemas, table-driven codecs, reference device models, reusable state machine flows, and turnkey manifests.

These assets serve as the foundational templates that user projects (such as those in [`examples/`](../examples/README.md)) import and extend.

---

## Directory Structure

```
data/
├── schemas/      # Reusable descriptor templates with autofill placeholders
│   ├── usb_base.yaml  # Base USB device and configuration descriptor templates
│   └── hid_base.yaml  # Base HID interface and class descriptor templates
├── codecs/       # Generic table-driven item-stream encoding definitions
│   └── hid_report.yaml# HID report descriptor token, type, tag, and flag mappings
├── devices/      # Default concrete USB device descriptor definitions
│   ├── keyboard.yaml  # Standard USB boot keyboard (VID 0x04d9, PID 0x1702)
│   ├── mouse.yaml     # Standard USB optical mouse (VID 0x046d, PID 0xc00e)
│   └── uvc_camera.yaml# Standard UVC webcam descriptor tree (VID 0x046d, PID 0x082d)
├── flows/        # Declarative behavior state machine flows
│   ├── enumeration_flow.yaml   # Standard enumeration responder (GET_DESCRIPTOR rules)
│   └── uvc_streaming_flow.yaml # UVC streaming flow with idle/streaming states
└── manifests/    # Turnkey project manifests combining default devices and flows
    ├── keyboard.yaml  # Boots the default keyboard with enumeration_flow
    ├── mouse.yaml     # Boots the default mouse with enumeration_flow
    └── uvc_camera.yaml# Boots the default UVC camera with uvc_streaming_flow
```

---

## Role & Usage

### 1. Schema Templates (`schemas/`)
Define standard descriptor shapes with `null` length/count placeholders that the loader automatically calculates (e.g. `bLength`, `wTotalLength`, `bNumInterfaces`, `bNumEndpoints`). Custom devices import these to avoid redefining boilerplate standard USB fields:

```yaml
imports: ["schemas/usb_base.yaml"]
device_descriptor:
  template: "usb_base.device_descriptor"
  idVendor: 0x1234
  idProduct: 0x5678
```

### 2. Codecs (`codecs/`)
Provide data-driven encoding tables that translate symbolic DSL representations (such as HID report descriptors) into binary bytes without C code modifications. See [`codecs/hid_report.yaml`](codecs/hid_report.yaml) and [docs/architecture.md](../docs/architecture.md#6-generic-codec-tables-replaces-hid-special-casing).

### 3. Built-in Manifests (`manifests/`)
Allow running standard devices directly from the command line without creating custom YAML projects:

```sh
# Emulate a standard USB keyboard
./build/src/usbvcr --yaml data/manifests/keyboard.yaml

# Emulate a standard USB optical mouse
./build/src/usbvcr --yaml data/manifests/mouse.yaml

# Emulate a standard UVC webcam
./build/src/usbvcr --yaml data/manifests/uvc_camera.yaml
```

---

## Runtime Asset Discovery

`usbvcr` locates these files at runtime via `usbvcr_find_data_file()`, searching in the following priority order:

1. **Environment Variable**: Path specified in `$USBVCR_DATA_DIR` (if set and directory exists).
2. **Current Working Directory**: `./data/` or `../data/`.
3. **Binary Location**: Relative to the executable (`../../data/`, `../data/`, or `../share/usbvcr/`).
4. **Compiled-in Path**: Directory configured at build time (`-Ddatadir=...`).
5. **System XDG Data Directories**: `$XDG_DATA_DIRS/usbvcr/` (e.g. `/usr/local/share/usbvcr`, `/usr/share/usbvcr`).
