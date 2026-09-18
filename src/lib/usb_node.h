/*
 * usb_node.h – USB descriptor tree model.
 *
 * Provides UsbNode (tree of named properties) where each property (UsbProp)
 * can hold integers (u8/u16/u32), raw byte arrays, or nested child nodes.
 *
 * Usage: yaml_loader parses YAML into a UsbNode tree; yaml_backend serializes
 * the tree into binary USB descriptors.
 */

#ifndef USB_NODE_H
#define USB_NODE_H

#include <glib.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    USB_PROP_UINT8,
    USB_PROP_UINT16,
    USB_PROP_UINT32,
    USB_PROP_ARRAY,
    USB_PROP_NODE,
} UsbPropType;

typedef struct _UsbNode UsbNode;

typedef struct {
    char        *name;
    UsbPropType  type;
    size_t       size;
    union {
        uint8_t   u8;
        uint16_t  u16;
        uint32_t  u32;
        uint8_t  *array;
        UsbNode  *node;
    } val;
} UsbProp;

struct _UsbNode {
    GList *props;  /* list of UsbProp* */
};

/* ── Node constructors / destructors ── */

UsbNode *usb_node_create(void);
void     usb_node_free(UsbNode *node);

/* ── Property constructors ── */

UsbProp *create_prop_u8(const char *name, uint8_t val);
UsbProp *create_prop_u16(const char *name, uint16_t val);
UsbProp *create_prop_u32(const char *name, uint32_t val);
UsbProp *create_prop_array(const char *name, const uint8_t *data, size_t len);
UsbProp *create_prop_node(const char *name, UsbNode *child_node);

/* ── Serialization ── */

void usb_node_serialize(UsbNode *node, GByteArray *buffer);

#endif /* USB_NODE_H */
