#include <string.h>

#include "descriptor.h"
#include "log.h"

/* Standard descriptor prologue: every USB descriptor starts with these. */
#define DESC_bLength         0
#define DESC_bDescriptorType 1

/* Interface descriptor field offsets. */
#define IFACE_bInterfaceNumber   2
#define IFACE_bInterfaceClass    5
#define IFACE_bInterfaceSubClass 6
#define IFACE_bInterfaceProtocol 7

/* Endpoint descriptor field offsets. */
#define EP_bEndpointAddress 2
#define EP_bmAttributes     3
#define EP_wMaxPacketSize   4  /* little-endian u16 */
#define EP_bInterval        6


void desc_set_interface_info_header(const uint8_t *cfg, uint8_t cur_iface,
                                    size_t offset,
                                    usbredir_interface_info_header *ii)
{
    uint32_t n = ii->interface_count;

    if (cur_iface >= 32)
        return;

    if (cur_iface + 1u > n)
        ii->interface_count = cur_iface + 1u;

    ii->interface[cur_iface]           = cur_iface;
    ii->interface_class[cur_iface]     = cfg[offset + IFACE_bInterfaceClass];
    ii->interface_subclass[cur_iface]  = cfg[offset + IFACE_bInterfaceSubClass];
    ii->interface_protocol[cur_iface]  = cfg[offset + IFACE_bInterfaceProtocol];
}


void desc_set_ep_info_header(const uint8_t *cfg, uint8_t cur_iface,
                             size_t offset,
                             usbredir_ep_info_header *ei)
{
    uint8_t addr = cfg[offset + EP_bEndpointAddress];
    uint8_t idx  = usbredir_ep_index(addr);

    ei->type[idx]     = cfg[offset + EP_bmAttributes] & 0x03;
    ei->interval[idx] = cfg[offset + EP_bInterval];
    ei->interface[idx] = cur_iface;
    ei->max_packet_size[idx] =
        (uint16_t)(cfg[offset + EP_wMaxPacketSize] |
                  (cfg[offset + EP_wMaxPacketSize + 1] << 8));
}


int desc_parse_config(const uint8_t *cfg, size_t len,
                      usbredir_interface_info_header *ii,
                      usbredir_ep_info_header *ei)
{
    memset(ii, 0, sizeof(*ii));
    memset(ei, 0, sizeof(*ei));

    uint8_t cur_iface = 0;   /* interface owning the endpoints we see */
    size_t offset = 0;

    while (offset + 2 <= len) {
        uint8_t blen = cfg[offset + DESC_bLength];
        uint8_t btype = cfg[offset + DESC_bDescriptorType];

        if (blen == 0 || offset + blen > len) {
            LOG_ERR("descriptor: malformed at offset %zu", offset);
            return -1;
        }

        switch (btype) {
            case USB_DESC_INTERFACE:
               if (blen < 9)
                   break;

               cur_iface = cfg[offset + IFACE_bInterfaceNumber];

               desc_set_interface_info_header(cfg, cur_iface, offset, ii);
               break;
            case USB_DESC_ENDPOINT:
               if (blen < 7)
                   break;

               desc_set_ep_info_header(cfg, cur_iface, offset, ei);
               break;
        }

        offset += blen;
    }

    return 0;
}
