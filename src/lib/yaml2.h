#ifndef USBREDIR_CLIENT_YAML_H
#define USBREDIR_CLIENT_YAML_H

#include "pcap2.h"
#include "../core/protocol.h"

typedef struct {
    uint64_t len;
    uint8_t *load;
} packet_raw_t;
        
typedef struct {
    usb_control_packet_header_t *usb_pcap_setup;
    usb_pcap_t *usb_pcap;
    packet_raw_t *raw;
} packet_t;

packet_t *read_yaml(const char *path);

#endif
