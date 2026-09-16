#ifndef USBREDIR_CLIENT_PCAP_H
#define USBREDIR_CLIENT_PCAP_H

#include <stdint.h>

#pragma pack(push, 1)

typedef struct {
    uint16_t header_len;    /* Header length */
    uint64_t irp_id;        /* I/O Request Packet ID */
    uint32_t status;        /* USB Status code */
    uint16_t function;      /* URB Function */
    uint8_t  info;          /* Transfer direction/information */
    uint16_t bus;           /* Hub/Bus interface number */
    uint16_t device;        /* USB Device address */
    uint8_t  endpoint;      /* Endpoint number */
    uint8_t  transfer;      /* Transfer type (0: Control, 1: Isoch, 2: Bulk, 3: Intr) */
    uint32_t data_length;   /* Length of the raw payload trailing this header */
} usb_pcap_t;

#pragma pack(pop)

#endif // USBREDIR_CLIENT_PCAP_H

