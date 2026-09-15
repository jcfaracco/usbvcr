#ifndef USBREDIR_CLIENT_DEVICE_H
#define USBREDIR_CLIENT_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/*
 * Generic emulated-device backend interface.
 *
 * The usb_core engine owns the usbredir transport, the handshake, the
 * standard control requests and endpoint routing. A backend supplies only
 * what is class-specific: its descriptors and its endpoint I/O. The same
 * engine drives a HID keyboard, a mass-storage disk, a UVC camera, etc.
 */

typedef struct usb_device usb_device_t;

typedef struct {
    const char *name;   /* e.g. "hid-keyboard" */

    /* Return a descriptor by (type, index). On success set out and len,
     * return 0; return -1 if the descriptor is unknown (engine will stall). */
    int (*get_descriptor)(usb_device_t *dev, uint8_t type, uint8_t index,
                          uint16_t lang, const uint8_t **out, uint16_t *len);

    /* Handle a class/vendor control request the engine doesn't know.
     * For IN transfers, fill `data` and set *len. Return 0 on success
     * (ACK), -1 to stall. */
    int (*control)(usb_device_t *dev,
                   const usbredir_control_packet_header *setup,
                   uint8_t *data, uint16_t *len);

    /* Endpoint data. `ep` is the full bEndpointAddress.
     *   ep_in : produce up to `cap` bytes, return count (0 = NAK), -1 error.
     *   ep_out: consume `len` bytes, return 0/-1. */
    int (*ep_in )(usb_device_t *dev, uint8_t ep, uint8_t *buf, size_t cap);
    int (*ep_out)(usb_device_t *dev, uint8_t ep, const uint8_t *buf, size_t len);

    /* Optional: periodic hook for streaming producers. May be NULL. */
    void (*tick)(usb_device_t *dev);
} usb_device_ops;

struct usb_device {
    const usb_device_ops *ops;
    void *state;   /* backend-private */
};

/* Backend factories (registry lives in main.c). */
usb_device_t *hid_keyboard_create(void);

#endif
