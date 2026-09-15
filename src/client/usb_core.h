#ifndef USBREDIR_CLIENT_USB_CORE_H
#define USBREDIR_CLIENT_USB_CORE_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ *
 * USB‑redir protocol definitions (from the usbredir spec)            *
 * ------------------------------------------------------------------ */

/* Every usbredir packet starts with this 12‑byte header (little‑endian) */
#pragma pack(push, 1)
typedef struct {
    uint32_t type;
    uint32_t length;
    int32_t  id;
} usbredir_header;
#pragma pack(pop)

/* Packet type constants */
enum {
    RT_HELLO              = 0,
    RT_DEVICE_CONNECT     = 1,
    RT_DEVICE_DISCONNECT  = 2,
    RT_RESET              = 3,
    RT_INTERFACE_INFO     = 4,
    RT_EP_INFO            = 5,
};


/* ------------------
 * TODO: Remove it
 */

#define USB_REDIR_BUF_SIZE 1024

// HID Descriptor for Interface 0 (9 bytes)
static const uint8_t USB_REDIR_HELLO_PACKET[68] = {
    // "usbredirserver 0.8.0" padded to 64 bytes
    'u','s','b','r','e','d','i','r','s','e','r','v','e','r',' ','0','.','8','.','0',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,
    // capabilities: 0xdf000000
    0xdf, 0x00, 0x00, 0x00
};

#define HIGH_BYTE(x) (((x) >> 8) & 0xff)

// ---- Speed constants ----
#define USB_REDIR_SPEED_LOW     0
#define USB_REDIR_SPEED_FULL    1
#define USB_REDIR_SPEED_HIGH    2
#define USB_REDIR_SPEED_SUPER   3
#define USB_REDIR_SPEED_UNKNOWN 255

// ---- Structs ----
#pragma pack(push, 1)
typedef struct {
    uint32_t interface_count;
    uint8_t  interface[32];
    uint8_t  interface_class[32];
    uint8_t  interface_subclass[32];
    uint8_t  interface_protocol[32];
} usbredir_interface_info_header;

typedef struct {
    uint8_t  type[32];
    uint8_t  interval[32];
    uint8_t  interface[32];
    uint16_t max_packet_size[32];
    uint32_t max_streams[32];
} usbredir_ep_info_header;

typedef struct {
    uint8_t  speed;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version_bcd;
} usbredir_device_connect_header;

typedef struct {
    uint8_t  endpoint;
    uint8_t  request;
    uint8_t  requesttype;
    uint8_t  status;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} usbredir_control_packet_header;

typedef struct {
    uint8_t configuration;
} usb_redir_set_configuration_header;

#pragma pack(pop)

size_t build_interface_info_pkt(uint8_t *out);

size_t build_ep_info_pkt(uint8_t *out);

size_t build_connect_pkt(uint8_t *out);

/* ------------------ */

/* Receive one usbredir packet (header + payload) from `sock` and print it.
 * Returns the packet type, or -1 on error. */
int receive_and_print_packet(int sock);

/* Build our hello packet into `out`. Returns total size in bytes. */
size_t build_hello_packet(uint8_t *out);

#endif
