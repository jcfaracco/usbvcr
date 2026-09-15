#ifndef __USB_PCAP_H__
#define __USB_PCAP_H__

#include <stdint.h>

// Target Device IDs to filter for (Modify these)
#define TARGET_VENDOR_ID  0x1b1c
#define TARGET_PRODUCT_ID 0x0c1a

// Linux usbmon mmapped header layout (DLT_USB_LINUX_MMAPPED)
typedef struct usbmon_packet_header {
    uint64_t id;
    uint8_t  type; // 'S'ubmission, 'C'allback, 'E'rror
    uint8_t  transfer_type; // 2 = Control, 3 = Bulk, etc.
    uint8_t  endpoint_number;
    uint8_t  device_address;
    uint16_t bus_id;
    char     setup_flag;
    char     data_flag;
    uint64_t sec;
    uint32_t usec;
    uint32_t status;
    uint32_t length;
    uint32_t data_length;
    // ... union of setup data or iso descriptors follows (usually 32 bytes)
    uint8_t  sub_header[32]; 
} usbmon_packet_header_t;

#endif
