# usbvcr

**YAML in, real USB traffic out.** Describe a USB device — descriptors,
behavior, raw payloads — in human-readable YAML, and usbvcr serves it live to
QEMU (or a real Linux USB gadget) over the wire.

- [Architecture & Implementation Plan](ARCHITECTURE.md)
- [Canonical YAML Dialect — Directory & Key Reference](yaml_dialect.md)
- [Running with QEMU and the Local FunctionFS Engine](running_engines.md)

Source: [github.com/jcfaracco/usbvcr](https://github.com/jcfaracco/usbvcr)
