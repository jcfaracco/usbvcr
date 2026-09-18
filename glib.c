#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    USB_PROP_UINT8,
    USB_PROP_UINT16,
    USB_PROP_UINT32,
    USB_PROP_ARRAY,
    USB_PROP_NODE 
} usb_prop_type_t;

// Forward declaration for recursive nesting
typedef struct usb_node usb_node_t;

typedef struct {
    char *name;
    UsbPropType type;
    size_t size; 
    union {
        uint8_t  u8;
        uint16_t u16;
        uint32_t u32;
        uint8_t  *array;
        UsbNode  *node; // Nested child node
    } val;
} UsbProp;

struct UsbNode {
    GList *props; // List of UsbProp*
};

// --- Constructors ---

UsbNode* create_node() {
    return g_new0(UsbNode, 1);
}

UsbProp* create_prop_u8(const char *name, uint8_t val) {
    UsbProp *p = g_new0(UsbProp, 1);
    p->name = g_strdup(name);
    p->type = USB_PROP_UINT8;
    p->val.u8 = val;
    p->size = 1;
    return p;
}

UsbProp* create_prop_u16(const char *name, uint16_t val) {
    UsbProp *p = g_new0(UsbProp, 1);
    p->name = g_strdup(name);
    p->type = USB_PROP_UINT16;
    p->val.u16 = val;
    p->size = 2;
    return p;
}

UsbProp* create_prop_array(const char *name, const uint8_t *data, size_t len) {
    UsbProp *p = g_new0(UsbProp, 1);
    p->name = g_strdup(name);
    p->type = USB_PROP_ARRAY;
    p->val.array = g_memdup2(data, len);
    p->size = len;
    return p;
}

UsbProp* create_prop_node(const char *name, UsbNode *child_node) {
    UsbProp *p = g_new0(UsbProp, 1);
    p->name = g_strdup(name);
    p->type = USB_PROP_NODE;
    p->val.node = child_node;
    return p;
}

// --- Recursive Serializer ---

void serialize_node(UsbNode *node, GByteArray *buffer) {
    for (GList *l = node->props; l != NULL; l = l->next) {
        UsbProp *p = (UsbProp *)l->data;
        
        switch (p->type) {
            case USB_PROP_UINT8:
                g_byte_array_append(buffer, &p->val.u8, 1);
                break;
                
            case USB_PROP_UINT16: {
                uint16_t le_val = GUINT16_TO_LE(p->val.u16); 
                g_byte_array_append(buffer, (uint8_t*)&le_val, 2);
                break;
            }
                
            case USB_PROP_UINT32: {
                uint32_t le_val = GUINT32_TO_LE(p->val.u32);
                g_byte_array_append(buffer, (uint8_t*)&le_val, 4);
                break;
            }
                
            case USB_PROP_ARRAY:
                g_byte_array_append(buffer, p->val.array, p->size);
                break;
                
            case USB_PROP_NODE:
                // Recursively flatten nested descriptors
                serialize_node(p->val.node, buffer);
                break;
        }
    }
}

// --- Memory Cleanup ---

void free_node(UsbNode *node) {
    for (GList *l = node->props; l != NULL; l = l->next) {
        UsbProp *p = (UsbProp *)l->data;
        g_free(p->name);
        if (p->type == USB_PROP_ARRAY) g_free(p->val.array);
        if (p->type == USB_PROP_NODE) free_node(p->val.node);
        g_free(p);
    }
    g_list_free(node->props);
    g_free(node);
}

// --- Execution Example ---

int main() {
    // 1. Build the Endpoint Descriptor (Nested level 2)
    UsbNode *ep_node = create_node();
    ep_node->props = g_list_append(ep_node->props, create_prop_u8("bLength", 7));
    ep_node->props = g_list_append(ep_node->props, create_prop_u8("bDescriptorType", 0x05));
    ep_node->props = g_list_append(ep_node->props, create_prop_u8("bEndpointAddress", 0x81));
    ep_node->props = g_list_append(ep_node->props, create_prop_u8("bmAttributes", 0x03));
    ep_node->props = g_list_append(ep_node->props, create_prop_u16("wMaxPacketSize", 8));
    ep_node->props = g_list_append(ep_node->props, create_prop_u8("bInterval", 10));

    // 2. Build the Interface Descriptor (Nested level 1)
    UsbNode *if_node = create_node();
    if_node->props = g_list_append(if_node->props, create_prop_u8("bLength", 9));
    if_node->props = g_list_append(if_node->props, create_prop_u8("bDescriptorType", 0x04));
    if_node->props = g_list_append(if_node->props, create_prop_u8("bInterfaceNumber", 0));
    if_node->props = g_list_append(if_node->props, create_prop_u8("bAlternateSetting", 0));
    if_node->props = g_list_append(if_node->props, create_prop_u8("bNumEndpoints", 1));
    if_node->props = g_list_append(if_node->props, create_prop_u8("bInterfaceClass", 0x03)); // HID
    if_node->props = g_list_append(if_node->props, create_prop_u8("bInterfaceSubClass", 0x01));
    if_node->props = g_list_append(if_node->props, create_prop_u8("bInterfaceProtocol", 0x01));
    if_node->props = g_list_append(if_node->props, create_prop_u8("iInterface", 0));
    
    // Attach the endpoint inside the interface
    if_node->props = g_list_append(if_node->props, create_prop_node("endpoints", ep_node));

    // 3. Build the Configuration Descriptor (Root)
    UsbNode *cfg_node = create_node();
    cfg_node->props = g_list_append(cfg_node->props, create_prop_u8("bLength", 9));
    cfg_node->props = g_list_append(cfg_node->props, create_prop_u8("bDescriptorType", 0x02));
    cfg_node->props = g_list_append(cfg_node->props, create_prop_u16("wTotalLength", 25)); // 9 + 9 + 7
    cfg_node->props = g_list_append(cfg_node->props, create_prop_u8("bNumInterfaces", 1));
    cfg_node->props = g_list_append(cfg_node->props, create_prop_u8("bConfigurationValue", 1));
    cfg_node->props = g_list_append(cfg_node->props, create_prop_u8("iConfiguration", 0));
    cfg_node->props = g_list_append(cfg_node->props, create_prop_u8("bmAttributes", 0xA0));
    cfg_node->props = g_list_append(cfg_node->props, create_prop_u8("bMaxPower", 50));
    
    // Attach the interface inside the configuration
    cfg_node->props = g_list_append(cfg_node->props, create_prop_node("interfaces", if_node));

    // 4. Serialize the entire tree to binary
    GByteArray *buffer = g_byte_array_new();
    serialize_node(cfg_node, buffer);

    // 5. Output the result
    printf("Serialized USB Configuration Length: %u bytes\n", buffer->len);
    printf("Raw Data (Hex): ");
    for (guint i = 0; i < buffer->len; i++) {
        printf("%02X ", buffer->data[i]);
    }
    printf("\n");

    // Cleanup
    g_byte_array_free(buffer, TRUE);
    free_node(cfg_node);

    return 0;
}
