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

int desc_parse_config(const uint8_t *cfg, size_t len,
                      usbredir_interface_info_header *ii,
                      usbredir_ep_info_header *ei)
{
    memset(ii, 0, sizeof(*ii));
    memset(ei, 0, sizeof(*ei));

    uint8_t cur_iface = 0;   /* interface owning the endpoints we see */
    size_t off = 0;

    while (off + 2 <= len) {
        uint8_t blen = cfg[off + DESC_bLength];
        uint8_t btype = cfg[off + DESC_bDescriptorType];

        if (blen == 0 || off + blen > len) {
            LOG_ERR("descriptor: malformed at offset %zu", off);
            return -1;
        }

        if (btype == USB_DESC_INTERFACE && blen >= 9) {
            cur_iface = cfg[off + IFACE_bInterfaceNumber];
            if (cur_iface < 32) {
                uint32_t n = ii->interface_count;
                if (cur_iface + 1u > n)
                    ii->interface_count = cur_iface + 1u;
                ii->interface[cur_iface]           = cur_iface;
                ii->interface_class[cur_iface]     = cfg[off + IFACE_bInterfaceClass];
                ii->interface_subclass[cur_iface]  = cfg[off + IFACE_bInterfaceSubClass];
                ii->interface_protocol[cur_iface]  = cfg[off + IFACE_bInterfaceProtocol];
            }
        } else if (btype == USB_DESC_ENDPOINT && blen >= 7) {
            uint8_t addr = cfg[off + EP_bEndpointAddress];
            uint8_t idx  = usbredir_ep_index(addr);
            ei->type[idx]     = cfg[off + EP_bmAttributes] & 0x03;
            ei->interval[idx] = cfg[off + EP_bInterval];
            ei->interface[idx] = cur_iface;
            ei->max_packet_size[idx] =
                (uint16_t)(cfg[off + EP_wMaxPacketSize] |
                          (cfg[off + EP_wMaxPacketSize + 1] << 8));
        }

        off += blen;
    }

    return 0;
}
