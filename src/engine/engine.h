/*
 * engine.h – Generic execution engine interface for USB VCR.
 */

#ifndef USB_ENGINE_H
#define USB_ENGINE_H

#include "device.h"
#include <glib.h>

/* Generic configuration collected from CLI flags or YAML manifests. */
typedef struct usb_engine_config {
    const char *host;          /* Network host (for qemu/usbredir) */
    const char *port;          /* Network port */
    const char *udc;           /* Target UDC name (for local FunctionFS gadget) */
    const char *instance_name; /* Gadget instance name */
    gboolean debug;
} usb_engine_config_t;

typedef struct usb_engine usb_engine_t;

typedef struct usb_engine_ops {
    const char *name; /* Engine identifier: "qemu", "local", etc. */
    const char *description;

    /* Initialize target transport, sockets, or gadget setup */
    int (*init)(usb_engine_t *engine, const usb_engine_config_t *config);

    /* Run the main execution/polling loop with the USB device */
    int (*run)(usb_engine_t *engine, usb_device_t *dev);

    /* Optional graceful stop hook */
    void (*stop)(usb_engine_t *engine);

    /* Clean up resources (sockets, mounts, memory) */
    void (*destroy)(usb_engine_t *engine);
} usb_engine_ops_t;

struct usb_engine {
    const usb_engine_ops_t *ops;
    void *priv;
};

/* Factory and runner API */
usb_engine_t *usb_engine_create(const char *name, const usb_engine_config_t *config);
int usb_engine_run(usb_engine_t *engine, usb_device_t *dev);
void usb_engine_stop(usb_engine_t *engine);
void usb_engine_destroy(usb_engine_t *engine);

#endif /* USB_ENGINE_H */
