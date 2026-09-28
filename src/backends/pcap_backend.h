/*
 * pcap_backend.h – usb_device_ops backend that replays a usbmon pcap/pcapng
 * capture as if the recorded device were live.
 *
 * Sibling of yaml_backend: same engine seam (core/device.h), different source
 * of truth. Where yaml_backend answers from a YAML device tree + flow, this
 * one answers from decoded pcap transfers (descriptors, control replies and
 * endpoint IN data captured from a real device via usbmon).
 */

#ifndef PCAP_BACKEND_H
#define PCAP_BACKEND_H

#include "device.h"

/* Create a backend replaying the usbmon capture at `path`
 * (pcap or pcapng). Returns NULL on error. */
usb_device_t *pcap_backend_create(const char *path);

#endif /* PCAP_BACKEND_H */
