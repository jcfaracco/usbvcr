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
    GByteArray *desc_buf;    /* scratch for control ($device.*) resolution */
    GHashTable *desc_bufs;   /* descriptor type -> its own GByteArray */
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

/* Like find_node_by_name_and_index but returns the prop itself, so callers can
 * tell a nested descriptor (USB_PROP_NODE) from a codec-encoded leaf such as
 * report_descriptor (USB_PROP_ARRAY). */
static usb_prop_t *find_prop_by_name_and_index(usb_node_t *parent, const char *name, int target_index)
{
    if (!parent || !name) return NULL;
    int current_index = 0;
    for (GList *l = parent->props; l; l = l->next) {
        usb_prop_t *p = l->data;
        if (strcmp(p->name, name) == 0) {
            if (current_index == target_index)
                return p;
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

    /* Each (type,index) gets its own persistent buffer. announce_device holds
     * the DEVICE pointer while it then fetches CONFIG; a single shared scratch
     * buffer would alias them and corrupt the device descriptor. Key by
     * (type<<8 | index). */
    guint key = ((guint)type << 8) | index;
    GByteArray *buf = g_hash_table_lookup(st->desc_bufs, GUINT_TO_POINTER(key));
    if (!buf) {
        buf = g_byte_array_new();
        g_hash_table_insert(st->desc_bufs, GUINT_TO_POINTER(key), buf);
    }
    g_byte_array_set_size(buf, 0);
    usb_node_serialize(target, buf);

    if (out) *out = buf->data;
    if (len) *len = buf->len;
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
        
        char *p = path;
        if (strncmp(p, "$device.", 8) == 0) p += 8;

        char **parts = g_strsplit(p, ".", 0);
        int nparts = (int)g_strv_length(parts);
        usb_node_t *parent = st->dev->tree;
        const uint8_t *out_bytes = NULL;
        size_t out_n = 0;

        for (int i = 0; i < nparts && parent; i++) {
            char *name = parts[i];
            char *bracket = strchr(name, '[');
            int idx = 0;
            if (bracket) {
                *bracket = '\0';
                idx = atoi(bracket + 1);
            }

            usb_prop_t *prop = find_prop_by_name_and_index(parent, name, idx);
            if (!prop) {
                parent = NULL;
                break;
            }

            if (i == nparts - 1) {
                /* Final component: a node is serialized to its descriptor
                 * bytes; a codec-encoded leaf (e.g. report_descriptor) is
                 * already raw bytes and emitted directly. */
                if (prop->type == USB_PROP_NODE) {
                    g_byte_array_set_size(st->desc_buf, 0);
                    usb_node_serialize(prop->val.node, st->desc_buf);
                    out_bytes = st->desc_buf->data;
                    out_n = st->desc_buf->len;
                } else if (prop->type == USB_PROP_ARRAY) {
                    out_bytes = prop->val.array;
                    out_n = prop->size;
                }
            } else {
                parent = (prop->type == USB_PROP_NODE) ? prop->val.node : NULL;
            }
        }
        g_strfreev(parts);

        if (out_bytes) {
            uint16_t n = (uint16_t)out_n;
            /* Honor the host's wLength: standard enumeration probes the
             * descriptor with a short request (e.g. 8 bytes) before asking
             * for the full length. Over-serving desyncs the host parser. */
            if (n > setup->length) n = setup->length;
            if (n > USB_VCR_BUF_SIZE) n = USB_VCR_BUF_SIZE;
            memcpy(data, out_bytes, n);
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

static const usb_device_ops_t yaml_ops = {
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
    st->desc_bufs = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, (GDestroyNotify)g_byte_array_unref);
    dev->ops = &yaml_ops;
    dev->state = st;
    return dev;
}

const char *yaml_backend_get_name(usb_device_t *dev)
{
    yaml_backend_state_t *st = dev->state;
    return st->dev->name;
}

const char *yaml_backend_get_engine(usb_device_t *dev)
{
    yaml_backend_state_t *st = dev->state;
    return st->dev->engine;
}

const char *yaml_backend_get_udc(usb_device_t *dev)
{
    yaml_backend_state_t *st = dev->state;
    return st->dev->udc;
}

const char *yaml_backend_get_host(usb_device_t *dev)
{
    yaml_backend_state_t *st = dev->state;
    return st->dev->host;
}

const char *yaml_backend_get_port(usb_device_t *dev)
{
    yaml_backend_state_t *st = dev->state;
    return st->dev->port;
}
