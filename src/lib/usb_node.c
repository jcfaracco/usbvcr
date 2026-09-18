/*
 * usb_node.c – USB descriptor tree model using GLib.
 *
 * Provides UsbNode (tree of named properties) where each property (UsbProp)
 * can hold integers (u8/u16/u32), raw byte arrays, or nested child nodes.
 *
 * Usage: yaml_loader parses YAML into a UsbNode tree; yaml_backend serializes
 * the tree into binary USB descriptors.
 */

#include <stdio.h>
#include <string.h>

#include "usb_node.h"

/* ── Node constructors / destructors ── */

UsbNode *usb_node_create(void)
{
    return g_new0(UsbNode, 1);
}

static void usb_prop_free(UsbProp *p)
{
    if (!p) return;
    g_free(p->name);
    if (p->type == USB_PROP_ARRAY)
        g_free(p->val.array);
    if (p->type == USB_PROP_NODE)
        usb_node_free(p->val.node);
    g_free(p);
}

void usb_node_free(UsbNode *node)
{
    if (!node) return;
    for (GList *l = node->props; l; l = l->next)
        usb_prop_free(l->data);
    g_list_free(node->props);
    g_free(node);
}

/* ── Property constructors ── */

UsbProp *create_prop_u8(const char *name, uint8_t val)
{
    UsbProp *p = g_new0(UsbProp, 1);
    p->name    = g_strdup(name);
    p->type    = USB_PROP_UINT8;
    p->val.u8  = val;
    p->size    = 1;
    return p;
}

UsbProp *create_prop_u16(const char *name, uint16_t val)
{
    UsbProp *p = g_new0(UsbProp, 1);
    p->name     = g_strdup(name);
    p->type     = USB_PROP_UINT16;
    p->val.u16  = val;
    p->size     = 2;
    return p;
}

UsbProp *create_prop_u32(const char *name, uint32_t val)
{
    UsbProp *p = g_new0(UsbProp, 1);
    p->name     = g_strdup(name);
    p->type     = USB_PROP_UINT32;
    p->val.u32  = val;
    p->size     = 4;
    return p;
}

UsbProp *create_prop_array(const char *name, const uint8_t *data, size_t len)
{
    UsbProp *p   = g_new0(UsbProp, 1);
    p->name      = g_strdup(name);
    p->type      = USB_PROP_ARRAY;
    p->val.array = g_memdup2(data, len);
    p->size      = len;
    return p;
}

UsbProp *create_prop_node(const char *name, UsbNode *child_node)
{
    UsbProp *p  = g_new0(UsbProp, 1);
    p->name     = g_strdup(name);
    p->type     = USB_PROP_NODE;
    p->val.node = child_node;
    return p;
}

/* ── Serialization ── */

void usb_node_serialize(UsbNode *node, GByteArray *buffer)
{
    for (GList *l = node->props; l; l = l->next) {
        UsbProp *p = l->data;

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
        }
    }
}
