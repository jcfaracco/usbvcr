#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/*
 * usb wire protocol definitions (little-endian on the wire).
 * NOTE: structs are memcpy'd to/from the socket assuming a little-endian
 * host. On a big-endian host these would need byte swapping.
 */

#define USB_VCR_BUF_SIZE 4096

/* In-memory packet header. The `id` is widened to 64 bits so it can hold a
 * 64-bit wire id; the transport layer serializes it as 4 or 8 bytes on the
 * wire depending on whether cap_64bits_ids was negotiated (12- vs 16-byte
 * header). This struct is NOT memcpy'd to the wire. */
typedef struct {
    uint32_t type;
    uint32_t length;
    int64_t  id;
} usb_header_t;

/* usb packet types (subset we use). */
enum {
    RT_HELLO                    = 0,
    RT_DEVICE_CONNECT           = 1,
    RT_DEVICE_DISCONNECT        = 2,
    RT_RESET                    = 3,
    RT_INTERFACE_INFO           = 4,
    RT_EP_INFO                  = 5,
    RT_SET_CONFIGURATION        = 6,
    RT_GET_CONFIGURATION        = 7,
    RT_CONFIGURATION_STATUS     = 8,
    RT_START_INTERRUPT_RECEIVING = 15,
    RT_STOP_INTERRUPT_RECEIVING  = 16,
    RT_CONTROL_PACKET           = 100,
    RT_BULK_PACKET              = 101,
    RT_ISO_PACKET               = 102,
    RT_INTERRUPT_PACKET         = 103,
};

/* usb capability bits (indices into hello capabilities[]). Several
 * of these change wire struct sizes, so what we advertise MUST match the
 * layouts we actually send below. */
enum {
    USB_VCR_CAP_BULK_STREAMS            = 0,  /* also gates ep_info.max_streams */
    USB_VCR_CAP_CONNECT_DEVICE_VERSION  = 1,  /* device_connect.device_version_bcd */
    USB_VCR_CAP_FILTER                  = 2,
    USB_VCR_CAP_DEVICE_DISCONNECT_ACK   = 3,
    USB_VCR_CAP_EP_INFO_MAX_PACKET_SIZE = 4,  /* ep_info.max_packet_size */
    USB_VCR_CAP_64BITS_IDS              = 5,
    USB_VCR_CAP_32BITS_BULK_LENGTH      = 6,
    USB_VCR_CAP_BULK_RECEIVING          = 7,
};

/* Capabilities we support. The effective set is the intersection with the
 * peer's, and it decides the wire length of ep_info/device_connect.
 *
 * qemu-xhci puts the device on a SuperSpeed port and requires the host to
 * advertise ep_info_max_packet_size, 32bits_bulk_length and 64bits_ids
 * (see usb_do_attach). 64bits_ids widens the post-hello header to 16
 * bytes, which the transport layer handles. bulk_streams gates the
 * ep_info max_streams array (which we serialize, zeroed). */
#define USB_VCR_OUR_CAPS \
    ((1u << USB_VCR_CAP_BULK_STREAMS)            | \
     (1u << USB_VCR_CAP_CONNECT_DEVICE_VERSION)  | \
     (1u << USB_VCR_CAP_FILTER)                  | \
     (1u << USB_VCR_CAP_DEVICE_DISCONNECT_ACK)   | \
     (1u << USB_VCR_CAP_EP_INFO_MAX_PACKET_SIZE) | \
     (1u << USB_VCR_CAP_64BITS_IDS)              | \
     (1u << USB_VCR_CAP_32BITS_BULK_LENGTH)      | \
     (1u << USB_VCR_CAP_BULK_RECEIVING))

/* Link speeds. */
#define USB_VCR_SPEED_LOW     0
#define USB_VCR_SPEED_FULL    1
#define USB_VCR_SPEED_HIGH    2
#define USB_VCR_SPEED_SUPER   3
#define USB_VCR_SPEED_UNKNOWN 255

/* Device descriptor field offsets (USB 2.0 spec, table 9-8). */
#define USB_DD_bDeviceClass     4
#define USB_DD_bDeviceSubClass  5
#define USB_DD_bDeviceProtocol  6
#define USB_DD_bMaxPacketSize0  7
#define USB_DD_idVendor         8   /* u16 LE */
#define USB_DD_idProduct        10  /* u16 LE */
#define USB_DD_bcdDevice        12  /* u16 LE */

/* Standard USB descriptor types. */
#define USB_DESC_DEVICE     0x01
#define USB_DESC_CONFIG     0x02
#define USB_DESC_STRING     0x03
#define USB_DESC_INTERFACE  0x04
#define USB_DESC_ENDPOINT   0x05
#define USB_DESC_HID        0x21
#define USB_DESC_HID_REPORT 0x22

/* Standard control requests (bRequest). */
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIGURATION 0x09

/* bmRequestType bits. */
#define USB_DIR_IN   0x80   /* device-to-host */
#define USB_TYPE_STANDARD 0x00
#define USB_TYPE_CLASS    0x20
#define USB_TYPE_MASK     0x60

#pragma pack(push, 1)

/* RT_HELLO payload: version string followed by capability words. */
typedef struct {
    char     version[64];
    uint32_t capabilities[1];
} usb_hello_header_t;

/* RT_INTERFACE_INFO payload. Arrays indexed by interface number. */
typedef struct {
    uint32_t interface_count;
    uint8_t  interface[32];
    uint8_t  interface_class[32];
    uint8_t  interface_subclass[32];
    uint8_t  interface_protocol[32];
} usb_interface_info_header_t;

/* RT_EP_INFO payload. Arrays indexed by usb endpoint index (0..31). */
typedef struct {
    uint8_t  type[32];
    uint8_t  interval[32];
    uint8_t  interface[32];
    uint16_t max_packet_size[32];
    uint32_t max_streams[32];
} usb_ep_info_header_t;

/* RT_DEVICE_CONNECT payload. */
typedef struct {
    uint8_t  speed;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version_bcd;
} usb_device_connect_header_t;

/* RT_CONTROL_PACKET header (mirrors the USB setup packet). */
typedef struct {
    uint8_t  endpoint;
    uint8_t  request;      /* bRequest */
    uint8_t  requesttype;  /* bmRequestType */
    uint8_t  status;
    uint16_t value;        /* wValue */
    uint16_t index;        /* wIndex */
    uint16_t length;       /* wLength */
} usb_control_packet_header_t;

/* RT_INTERRUPT_PACKET header, followed by report data. */
typedef struct {
    uint8_t  endpoint;
    uint8_t  status;
    uint16_t length;
} usb_interrupt_packet_header_t;

#pragma pack(pop)

/* usb endpoint index: 0..15 for OUT eps, 16..31 for IN eps. */
static inline uint8_t usb_ep_index(uint8_t ep_addr)
{
    return (ep_addr & 0x0f) | ((ep_addr & 0x80) >> 3);
}

#endif /* PROTOCOL_H */
