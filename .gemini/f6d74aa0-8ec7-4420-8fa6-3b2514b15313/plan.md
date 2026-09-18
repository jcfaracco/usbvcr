# Restructuring Plan

Based on the README.md §5 "Target source layout", here are the changes needed:

## Current vs Target Layout

### Changes needed:

1. **Adopt `glib.c` → `src/lib/usb_node.{c,h}`**
   - Move `glib.c` from root into `src/lib/usb_node.c`
   - Create `src/lib/usb_node.h` (header with declarations)
   - Fix type-name bugs: adopt `UsbNode`/`UsbProp` consistently
   - Add `create_prop_u32()` helper
   - Remove demo `main()` from it

2. **Create stub files for new modules** (§5):
   - `src/lib/yaml_loader.{c,h}` — canonical dialect → UsbNode tree
   - `src/lib/codec.{c,h}` — generic table-driven item-stream encoder
   - `src/lib/flow.{c,h}` — states, rules, match, actions, transitions
   - `src/backends/yaml_backend.{c,h}` — ONE generic ops backend

3. **Isolate legacy pcap-replay parser** (§4, §5):
   - Move `src/lib/usb_yaml.{c,h}` → `src/lib/replay/usb_yaml.{c,h}`
   - Move `src/lib/usbpcap_hdr.h` → `src/lib/replay/usbpcap_hdr.h`

4. **Create `codecs/` directory** with `hid_report.yaml` (§6)

5. **Update all meson.build files** to reflect new layout

6. **Update `#include` paths** everywhere affected

7. **Update tests** include paths

## Logical commits:

1. **lib: adopt glib.c as usb_node module** — move+fix glib.c → src/lib/usb_node.{c,h}
2. **lib: isolate legacy pcap-replay parser under replay/** — move usb_yaml + usbpcap_hdr
3. **lib: add stub modules for yaml_loader, codec, and flow**
4. **backends: add yaml_backend stub**
5. **data: add HID report codec table**
6. **build: update meson.build for new source layout**
