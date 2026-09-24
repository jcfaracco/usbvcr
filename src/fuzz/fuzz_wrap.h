#ifndef FUZZ_WRAP_H
#define FUZZ_WRAP_H

#include "device.h"
#include "fuzz.h"

/*
 * Wrap a valid device backend in a mutating layer. Enumeration can still
 * succeed (valid descriptors reach the guest class driver), while chosen
 * outputs are corrupted per `cfg`. Works for any backend — the whole point
 * of the generic vtable.
 *
 * Returns a new usb_device_t owning `base`; NULL on allocation failure.
 */
usb_device_t *fuzz_wrap(usb_device_t *base, const fuzz_cfg_t *cfg);

#endif /* FUZZ_WRAP_H */
