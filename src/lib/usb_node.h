/*
 * usb_node.h – USB descriptor tree model.
 *
 * Provides usb_node_t (tree of named properties) where each property (usb_prop_t)
 * can hold integers (u8/u16/u32), raw byte arrays, or nested child nodes.
 *
 * Usage: yaml_loader parses YAML into a usb_node_t tree; yaml_backend serializes
 * the tree into binary USB descriptors.
 */

#ifndef USB_NODE_H
#define USB_NODE_H

#include <glib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    USB_PROP_UNKNOWN,
    USB_PROP_UINT8,
    USB_PROP_UINT16,
    USB_PROP_UINT32,
    USB_PROP_ARRAY,
    USB_PROP_NODE,
} usb_prop_type_enum_t;

typedef struct usb_node usb_node_t;

typedef struct {
    char *name;
    usb_prop_type_enum_t type;
    size_t size;
    union {
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        uint8_t *array;
        usb_node_t *node;
    } val;
} usb_prop_t;

struct usb_node {
    GList *props;  /* list of usb_prop* */
};

/* ── Node constructors / destructors ── */

usb_node_t *usb_node_create(void);
void        usb_node_free(usb_node_t *node);

/* Deep copy of a node (and every nested child). */
usb_node_t *usb_node_clone(const usb_node_t *node);

/* ── Property constructors ── */

usb_prop_t *create_prop_u8(const char *name, uint8_t val);
usb_prop_t *create_prop_u16(const char *name, uint16_t val);
usb_prop_t *create_prop_u32(const char *name, uint32_t val);
usb_prop_t *create_prop_array(const char *name, const uint8_t *data, size_t len);
usb_prop_t *create_prop_node(const char *name, usb_node_t *child_node);

/* A named but value-less property (schema field set to `null`). Serializes to
 * nothing; a later autofill pass replaces it with a computed value. */
usb_prop_t *create_prop_null(const char *name);

/* ── Property mutation ── */

/* Append `prop`, or if a property with the same name already exists, replace it
 * in place (freeing the old one). Takes ownership of `prop`. */
void usb_node_set_prop(usb_node_t *node, usb_prop_t *prop);

/* Copy every property of `src` into `dst` via usb_node_set_prop (deep copy,
 * same-name fields overridden). Used to fold a schema template into a device
 * node before the device's own overrides are applied. */
void usb_node_merge(usb_node_t *dst, const usb_node_t *src);

/* ── Serialization ── */

void usb_node_serialize(usb_node_t *node, GByteArray *buffer);

/* ── Debug ── */

/* Pretty-print the tree to `out` (e.g. stdout/stderr), one property per line,
 * nested nodes indented. For inspection/debugging, not a wire format. */
void usb_node_dump(const usb_node_t *node, FILE *out);

#endif /* USB_NODE_H */
