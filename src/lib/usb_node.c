/*
 * usb_node.c – USB descriptor tree model using GLib.
 *
 * Provides usb_node_t (tree of named properties) where each property (usb_prop_t)
 * can hold integers (u8/u16/u32), raw byte arrays, or nested child nodes.
 *
 * Usage: yaml_loader parses YAML into a usb_node_t tree; yaml_backend serializes
 * the tree into binary USB descriptors.
 */

#include <stdio.h>
#include <string.h>

#include "usb_node.h"
#include "log.h"

/* ── Node constructors / destructors ── */

usb_node_t *usb_node_create(void)
{
    return g_new0(usb_node_t, 1);
}

static void usb_prop_free(usb_prop_t *p)
{
    if (!p) return;
    g_free(p->name);
    if (p->type == USB_PROP_ARRAY)
        g_free(p->val.array);
    if (p->type == USB_PROP_NODE)
        usb_node_free(p->val.node);
    g_free(p);
}

void usb_node_free(usb_node_t *node)
{
    if (!node) return;
    for (GList *l = node->props; l; l = l->next)
        usb_prop_free(l->data);
    g_list_free(node->props);
    g_free(node);
}

/* ── Property constructors ── */

usb_prop_t *create_prop_u8(const char *name, uint8_t val)
{
    usb_prop_t *p = g_new0(usb_prop_t, 1);
    p->name    = g_strdup(name);
    p->type    = USB_PROP_UINT8;
    p->val.u8  = val;
    p->size    = 1;
    return p;
}

usb_prop_t *create_prop_u16(const char *name, uint16_t val)
{
    usb_prop_t *p = g_new0(usb_prop_t, 1);
    p->name     = g_strdup(name);
    p->type     = USB_PROP_UINT16;
    p->val.u16  = val;
    p->size     = 2;
    return p;
}

usb_prop_t *create_prop_u32(const char *name, uint32_t val)
{
    usb_prop_t *p = g_new0(usb_prop_t, 1);
    p->name     = g_strdup(name);
    p->type     = USB_PROP_UINT32;
    p->val.u32  = val;
    p->size     = 4;
    return p;
}

usb_prop_t *create_prop_array(const char *name, const uint8_t *data, size_t len)
{
    usb_prop_t *p   = g_new0(usb_prop_t, 1);
    p->name      = g_strdup(name);
    p->type      = USB_PROP_ARRAY;
    p->val.array = g_memdup2(data, len);
    p->size      = len;
    return p;
}

usb_prop_t *create_prop_node(const char *name, usb_node_t *child_node)
{
    usb_prop_t *p  = g_new0(usb_prop_t, 1);
    p->name     = g_strdup(name);
    p->type     = USB_PROP_NODE;
    p->val.node = child_node;
    return p;
}

usb_prop_t *create_prop_null(const char *name)
{
    usb_prop_t *p = g_new0(usb_prop_t, 1);
    p->name = g_strdup(name);
    p->type = USB_PROP_UNKNOWN;
    p->size = 0;
    return p;
}

/* ── Cloning / mutation ── */

static usb_prop_t *usb_prop_clone(const usb_prop_t *p)
{
    switch (p->type) {
    case USB_PROP_UINT8:  return create_prop_u8(p->name, p->val.u8);
    case USB_PROP_UINT16: return create_prop_u16(p->name, p->val.u16);
    case USB_PROP_UINT32: return create_prop_u32(p->name, p->val.u32);
    case USB_PROP_ARRAY:  return create_prop_array(p->name, p->val.array, p->size);
    case USB_PROP_NODE:   return create_prop_node(p->name, usb_node_clone(p->val.node));
    case USB_PROP_UNKNOWN:
    default:              return create_prop_null(p->name);
    }
}

usb_node_t *usb_node_clone(const usb_node_t *node)
{
    if (!node) return NULL;

    usb_node_t *copy = usb_node_create();
    for (GList *l = node->props; l; l = l->next)
        copy->props = g_list_append(copy->props, usb_prop_clone(l->data));
    return copy;
}

void usb_node_set_prop(usb_node_t *node, usb_prop_t *prop)
{
    for (GList *l = node->props; l; l = l->next) {
        usb_prop_t *e = l->data;
        if (strcmp(e->name, prop->name) == 0) {
            usb_prop_free(e);
            l->data = prop;   /* replace in place, keeping field order */
            return;
        }
    }
    node->props = g_list_append(node->props, prop);
}

void usb_node_merge(usb_node_t *dst, const usb_node_t *src)
{
    if (!dst || !src) return;
    for (GList *l = src->props; l; l = l->next)
        usb_node_set_prop(dst, usb_prop_clone(l->data));
}

/* ── Serialization ── */

void usb_node_serialize(usb_node_t *node, GByteArray *buffer)
{
    for (GList *l = node->props; l; l = l->next) {
        usb_prop_t *p = l->data;

        switch (p->type) {
        case USB_PROP_UINT8:
            g_byte_array_append(buffer, &p->val.u8, 1);
            break;

        case USB_PROP_UINT16: {
            uint16_t le_val = GUINT16_TO_LE(p->val.u16);
            g_byte_array_append(buffer, (uint8_t *)&le_val, 2);
            break;
        }

        case USB_PROP_UINT32: {
            uint32_t le_val = GUINT32_TO_LE(p->val.u32);
            g_byte_array_append(buffer, (uint8_t *)&le_val, 4);
            break;
        }

        case USB_PROP_ARRAY:
            g_byte_array_append(buffer, p->val.array, p->size);
            break;

        case USB_PROP_NODE:
            usb_node_serialize(p->val.node, buffer);
            break;

        case USB_PROP_UNKNOWN:
            /* Unfilled null field: skipped until the autofill pass. */
            break;
        }
    }
}

int usb_node_validate(const usb_node_t *node, int depth)
{
    char *indent = g_strnfill(depth, ' ');

    for (GList *l = node->props; l; l = l->next) {
        usb_prop_t *p = l->data;

        switch (p->type) {
        case USB_PROP_UINT8:
            LOG_DEBUG("%s%s = (u8) 0x%02x", indent, p->name, p->val.u8);
            break;
        case USB_PROP_UINT16:
            LOG_DEBUG("%s%s = (u16) 0x%04x", indent, p->name, p->val.u16);
            break;
        case USB_PROP_UINT32:
            LOG_DEBUG("%s%s = (u32) 0x%04x", indent, p->name, p->val.u32);
            break;
        case USB_PROP_ARRAY:
            LOG_DEBUG("%s%s = array[%zu]", indent, p->name, p->size);
            break;
        case USB_PROP_NODE:
            LOG_DEBUG("%s%s = {", indent, p->name);
            if (usb_node_validate(p->val.node, depth + 2) < 0) {
                g_free(indent);
                return -1;
            }
            LOG_DEBUG("%s}", indent);
            break;
        case USB_PROP_UNKNOWN:
        default:
            LOG_DEBUG("%s%s = null", indent, p->name);
            g_free(indent);
            return -1;
        }
    }

    g_free(indent);

    return 0;
}
