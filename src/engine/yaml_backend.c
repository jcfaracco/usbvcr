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
#include "yaml/loader.h"
#include "usb_node.h"
#include "flow.h"
#include "log.h"

#include <stdlib.h>
#include <glib.h>

typedef struct {
    yaml_device_t *dev;
    GByteArray *desc_buf;
} yaml_backend_state_t;

static usb_node_t *find_node_by_name_and_index(usb_node_t *parent, const char *name, int target_index)
{
    if (!parent || !name) return NULL;
    int current_index = 0;
    for (GList *l = parent->props; l; l = l->next) {
        usb_prop_t *p = l->data;
        if (strcmp(p->name, name) == 0) {
            if (current_index == target_index) {
                return (p->type == USB_PROP_NODE) ? p->val.node : NULL;
            }
            current_index++;
        }
    }
    return NULL;
}

static int yb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                             uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)lang;
    yaml_backend_state_t *st = dev->state;
    if (!st || !st->dev || !st->dev->tree)
        return -1;

    usb_node_t *target = NULL;
    if (type == 1) { // USB_DESC_DEVICE
        target = find_node_by_name_and_index(st->dev->tree, "device_descriptor", index);
    } else if (type == 2) { // USB_DESC_CONFIG
        target = find_node_by_name_and_index(st->dev->tree, "configurations", index);
    }

    if (!target)
        return -1;

    g_byte_array_set_size(st->desc_buf, 0);
    usb_node_serialize(target, st->desc_buf);
    
    if (out) *out = st->desc_buf->data;
    if (len) *len = st->desc_buf->len;
    return 0;
}

static int yb_control(usb_device_t *dev,
                      const usb_control_packet_header_t *setup,
                      uint8_t *data, uint16_t *len)
{
    yaml_backend_state_t *st = dev->state;
    if (!st || !st->dev)
        return -1;
    
    /* flow_control matches on the standard 8-byte USB setup packet layout
     * (bmRequestType, bRequest, wValue, wIndex, wLength). The wire header
     * usb_control_packet_header_t stores the same fields in a different order
     * with a status byte and alignment pad, so marshal to canonical setup
     * bytes rather than casting the struct. */
    uint8_t su[8] = {
        setup->requesttype,
        setup->request,
        (uint8_t)(setup->value & 0xff), (uint8_t)(setup->value >> 8),
        (uint8_t)(setup->index & 0xff), (uint8_t)(setup->index >> 8),
        (uint8_t)(setup->length & 0xff), (uint8_t)(setup->length >> 8),
    };
    int rc = flow_control(st->dev->flow, su, data, len);
    
    if (rc == 0 && *len > 0 && data[0] == '$') {
        char path[1024];
        memcpy(path, data, *len);
        path[*len] = '\0';
        
        usb_node_t *target = st->dev->tree;
        char *p = path;
        if (strncmp(p, "$device.", 8) == 0) p += 8;
        
        char **parts = g_strsplit(p, ".", 0);
        for (int i = 0; parts[i] && target; i++) {
            char *name = parts[i];
            char *bracket = strchr(name, '[');
            int idx = 0;
            if (bracket) {
                *bracket = '\0';
                idx = atoi(bracket + 1);
            }
            target = find_node_by_name_and_index(target, name, idx);
        }
        g_strfreev(parts);
        
        if (target) {
            g_byte_array_set_size(st->desc_buf, 0);
            usb_node_serialize(target, st->desc_buf);
            uint16_t n = (uint16_t)st->desc_buf->len;
            /* Honor the host's wLength: standard enumeration probes the
             * descriptor with a short request (e.g. 8 bytes) before asking
             * for the full length. Over-serving desyncs the host parser. */
            if (n > setup->length) n = setup->length;
            if (n > USB_VCR_BUF_SIZE) n = USB_VCR_BUF_SIZE;
            memcpy(data, st->desc_buf->data, n);
            *len = n;
        } else {
            return -1;
        }
    }
    
    return rc;
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

usb_device_t *yaml_backend_create(const char *path)
{
    yaml_device_t *yd = yaml_parse_from_file(path);
    if (!yd)
        return NULL;

    if (usb_node_validate(yd->tree, 0) < 0) {
        LOG_ERR("There are some NULL props in devices for %s (`--debug` shows them).\n", path);
        return NULL;
    }

    yaml_backend_state_t *st = g_new(yaml_backend_state_t, 1);
    usb_device_t *dev = g_new(usb_device_t, 1);
    if (!st || !dev) {
        g_free(st);
        g_free(dev);
        yaml_device_free(yd);
        return NULL;
    }
    st->dev  = yd;
    st->desc_buf = g_byte_array_new();
    dev->ops = &yaml_ops;
    dev->state = st;
    return dev;
}
