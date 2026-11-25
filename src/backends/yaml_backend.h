/*
 * yaml_backend.h – Generic usb_device_ops backend driven by a loaded YAML
 * device tree + flow (README §5.1).
 *
 * This is the ONE backend: it bridges pure-lib data (yaml_loader tree, flow,
 * codec output) to the engine seam (core/device.h). Keeps fuzz_wrap working
 * for free since it wraps any usb_device_ops.
 */

#ifndef YAML_BACKEND_H
#define YAML_BACKEND_H

#include <glib.h>

#include "device.h"

/* Create a backend from a canonical device project directory.
 * Returns NULL on error. */
usb_device_t *yaml_backend_create(const char *path);

/* Free a backend created by yaml_backend_create(). No-op on NULL. */
void yaml_backend_destroy(usb_device_t *dev);

/* Project `name:` field the backend was loaded from, or NULL if the
 * manifest doesn't set one. */
const char *yaml_backend_get_name(usb_device_t *dev);

/* Project `connection:` block fields (engine/udc/host/port), or NULL if the
 * manifest doesn't set them. Lets `usbvcr --yaml <file>` run without CLI
 * flags; main.c's CLI flags take precedence when explicitly passed. */
const char *yaml_backend_get_engine(usb_device_t *dev);
const char *yaml_backend_get_udc(usb_device_t *dev);
const char *yaml_backend_get_host(usb_device_t *dev);
const char *yaml_backend_get_port(usb_device_t *dev);

/* Project `connection:` block's record/debug fields, or NULL/FALSE if unset.
 * Same CLI-takes-precedence rule as above. */
const char *yaml_backend_get_record(usb_device_t *dev);
gboolean yaml_backend_get_debug(usb_device_t *dev);

/* Top-level `pcap:` field: alternate to `devices:` — a capture path to
 * replay instead of emulating, or NULL if unset. When set, main.c swaps
 * this backend for a pcap_backend built from this path, keeping this
 * backend alive only to read the fields above. */
const char *yaml_backend_get_pcap(usb_device_t *dev);

#endif /* YAML_BACKEND_H */
