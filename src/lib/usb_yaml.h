#ifndef USBREDIR_CLIENT_YAML_H
#define USBREDIR_CLIENT_YAML_H

#include <glib.h>

#include "usbpcap_hdr.h"
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

GList *yaml_parse_from_file(const char *path);

#endif
