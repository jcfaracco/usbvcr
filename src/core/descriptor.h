#ifndef USBREDIR_CLIENT_DESCRIPTOR_H
#define USBREDIR_CLIENT_DESCRIPTOR_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/*
 * Walk a standard USB configuration descriptor (config + interface + class
 * + endpoint descriptors, concatenated) and populate the usb
 * interface_info / ep_info payloads the engine must announce. The config
 * descriptor is thus the single source of truth — no hand-maintained
 * interface/ep tables.
 *
 * Returns 0 on success, -1 on a malformed descriptor.
 */
int desc_parse_config(const uint8_t *cfg, size_t len,
                      usb_interface_info_header_t *ii,
                      usb_ep_info_header_t *ei);

#endif
