#ifndef USBREDIR_CLIENT_YAML_H
#define USBREDIR_CLIENT_YAML_H

#include "pcap.h"
#include "../core/protocol.h"

typedef struct {
    usb_control_packet_header_t *usb_pcap_setup;
    usb_pcap_t *usb_pcap;
    uint8_t *raw;
} packet_t;

packet_t *read_yaml(const char *path);

#endif
