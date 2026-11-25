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
                                    usb_interface_info_header_t *ii)
{
    uint32_t n = ii->interface_count;

    if (cur_iface >= 32) {
        LOG_ERR("descriptor: interface number %u exceeds max supported (31), ignored", cur_iface);
        return;
    }

    if (cur_iface + 1u > n)
        ii->interface_count = cur_iface + 1u;

    ii->interface[cur_iface]           = cur_iface;
    ii->interface_class[cur_iface]     = cfg[offset + IFACE_bInterfaceClass];
    ii->interface_subclass[cur_iface]  = cfg[offset + IFACE_bInterfaceSubClass];
    ii->interface_protocol[cur_iface]  = cfg[offset + IFACE_bInterfaceProtocol];
}


void desc_set_ep_info_header(const uint8_t *cfg, uint8_t cur_iface,
                             size_t offset,
                             usb_ep_info_header_t *ei)
{
    uint8_t addr = cfg[offset + EP_bEndpointAddress];
    uint8_t idx  = usb_ep_index(addr);

    if (idx >= 32) {
        LOG_ERR("descriptor: endpoint address 0x%02x at offset %zu yields out-of-range index %u",
                addr, offset, idx);
        return;
    }

    ei->type[idx]     = cfg[offset + EP_bmAttributes] & 0x03;
    ei->interval[idx] = cfg[offset + EP_bInterval];
    ei->interface[idx] = cur_iface;
    ei->max_packet_size[idx] =
        (uint16_t)(cfg[offset + EP_wMaxPacketSize] |
                  (cfg[offset + EP_wMaxPacketSize + 1] << 8));
}


int desc_parse_config(const uint8_t *cfg, size_t len,
                      usb_interface_info_header_t *ii,
                      usb_ep_info_header_t *ei)
{
    memset(ii, 0, sizeof(*ii));
    memset(ei, 0, sizeof(*ei));
    memset(ei->type, 255, sizeof(ei->type)); /* 255 = usb_redir_type_invalid */

    uint8_t cur_iface = 0;   /* interface owning the endpoints we see */
    size_t offset = 0;

    while (offset + 2 <= len) {
        uint8_t blen = cfg[offset + DESC_bLength];
        uint8_t btype = cfg[offset + DESC_bDescriptorType];

        if (blen == 0 || offset + blen > len) {
            if (blen == 0)
                LOG_ERR("descriptor: zero-length descriptor at offset %zu (bType=0x%02x)",
                        offset, btype);
            else
                LOG_ERR("descriptor: descriptor at offset %zu overruns buffer "
                        "(bLength=%u, remaining=%zu)", offset, blen, len - offset);
            return -1;
        }

        switch (btype) {
            case USB_DESC_CONFIG:
                /* Configuration descriptor header - skip */
                break;
            case USB_DESC_INTERFACE:
               if (blen < 9) {
                   LOG_WARN("descriptor: INTERFACE at offset %zu has bLength=%u (expected >= 9), skipping",
                            offset, blen);
                   break;
               }

               cur_iface = cfg[offset + IFACE_bInterfaceNumber];

               desc_set_interface_info_header(cfg, cur_iface, offset, ii);
               break;
            case USB_DESC_ENDPOINT:
               if (blen < 7) {
                   LOG_WARN("descriptor: ENDPOINT at offset %zu has bLength=%u (expected >= 7), skipping",
                            offset, blen);
                   break;
               }

               desc_set_ep_info_header(cfg, cur_iface, offset, ei);
               break;
            default:
               LOG_DEBUG("descriptor: unhandled bDescriptorType=0x%02x at offset %zu (bLength=%u)",
                         btype, offset, blen);
               break;
        }

        offset += blen;
    }

    return 0;
}
