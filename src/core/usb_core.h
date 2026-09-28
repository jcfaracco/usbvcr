#ifndef USB_CORE_H
#define USB_CORE_H

#include "device.h"

/*
 * usb engine. Performs the hello handshake, announces the device
 * (derived from its descriptors), then serves control requests and
 * endpoint traffic by delegating to the backend `dev`.
 *
 * Blocks until the peer disconnects or an error occurs.
 * Returns 0 on clean shutdown, -1 on error.
 */
int usb_core_run(int sock, usb_device_t *dev);

#endif /* USB_CORE_H */
