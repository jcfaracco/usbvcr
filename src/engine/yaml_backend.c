/*
 * yaml_backend.c – Generic ops backend.  STUB (README §9 build step 5).
 *
 * Maps usb_device_ops -> loaded tree + flow (README §5.1):
 *   get_descriptor -> serialize requested node, cache bytes
 *   control        -> flow_control (match rules)
 *   ep_in / ep_out -> flow_ep_in / flow_ep_out
 *   tick           -> flow_tick
 */

#include "yaml_backend.h"
#include "yaml_loader.h"
#include "flow.h"

#include <stdlib.h>
#include <glib.h>

typedef struct {
    yaml_device_t *dev;
} yaml_backend_state_t;

static int yb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                             uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)type; (void)index; (void)lang; (void)out; (void)len;
    return -1; /* TODO: serialize node from tree */
}

static int yb_control(usb_device_t *dev,
                      const usb_control_packet_header_t *setup,
                      uint8_t *data, uint16_t *len)
{
    yaml_backend_state_t *st = dev->state;
    if (!st || !st->dev)
        return -1;
    return flow_control(st->dev->flow, (const uint8_t *)setup, data, len);
}

static int yb_ep_in(usb_device_t *dev, uint8_t ep, uint8_t *buf, size_t cap)
{
    yaml_backend_state_t *st = dev->state;
    return flow_ep_in(st->dev->flow, ep, buf, cap);
}

static int yb_ep_out(usb_device_t *dev, uint8_t ep, const uint8_t *buf, size_t len)
{
    yaml_backend_state_t *st = dev->state;
    return flow_ep_out(st->dev->flow, ep, buf, len);
}

static void yb_tick(usb_device_t *dev)
{
    yaml_backend_state_t *st = dev->state;
    flow_tick(st->dev->flow);
}

static const usb_device_ops yaml_ops = {
    .name           = "yaml",
    .get_descriptor = yb_get_descriptor,
    .control        = yb_control,
    .ep_in          = yb_ep_in,
    .ep_out         = yb_ep_out,
    .tick           = yb_tick,
};

usb_device_t *yaml_backend_create(const char *dir)
{
    yaml_device_t *yd = yaml_loader_load(dir);
    if (!yd)
        return NULL;

    yaml_backend_state_t *st = g_new(yaml_backend_state_t, 1);
    usb_device_t *dev = g_new(usb_device_t, 1);
    if (!st || !dev) {
        g_free(st);
        g_free(dev);
        yaml_device_free(yd);
        return NULL;
    }
    st->dev  = yd;
    dev->ops = &yaml_ops;
    dev->state = st;
    return dev;
}
