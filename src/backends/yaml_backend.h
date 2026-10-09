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

#include "device.h"

/* Create a backend from a canonical device project directory.
 * Returns NULL on error. */
usb_device_t *yaml_backend_create(const char *path);

/* Project `name:` field the backend was loaded from, or NULL if the
 * manifest doesn't set one. */
const char *yaml_backend_get_name(usb_device_t *dev);

#endif /* YAML_BACKEND_H */
