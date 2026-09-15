#include <string.h>

#include "usb_core.h"
#include "transport.h"
#include "descriptor.h"
#include "protocol.h"
#include "log.h"

#define HELLO_VERSION "usb_redir_hello 0.1"

/* ------------------------------------------------------------------ *
 * Handshake / announcement                                            *
 * ------------------------------------------------------------------ */

static int send_hello(int sock)
{
    usbredir_hello_header h;
    memset(&h, 0, sizeof(h));
    strncpy(h.version, HELLO_VERSION, sizeof(h.version) - 1);

    h.capabilities[0] = USB_REDIR_OUR_CAPS;

    return usb_redir_send(sock, RT_HELLO, 0, &h, sizeof(h));
}

/* Derive interface_info + ep_info from the device's config descriptor and
 * announce the device to the peer. */
static int announce_device(int sock, usb_device_t *dev, uint32_t caps)
{
    const uint8_t *dd, *cd;
    uint16_t ddlen, cdlen;

    if (dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, &dd, &ddlen) < 0 ||
        dev->ops->get_descriptor(dev, USB_DESC_CONFIG, 0, 0, &cd, &cdlen) < 0) {
        LOG_ERR("backend missing device/config descriptor");
        return -1;
    }

    usbredir_interface_info_header ii;
    usbredir_ep_info_header ei;
    if (desc_parse_config(cd, cdlen, &ii, &ei) < 0)
        return -1;

    /* ep0 (control) is implicit: it has no endpoint descriptor in the
     * config, so fill its ep_info from the device descriptor's
     * bMaxPacketSize0 (byte 7). Without a nonzero max packet size the peer
     * cannot run control transfers and enumeration stalls. */
    uint8_t ep0_out = usbredir_ep_index(0x00);
    uint8_t ep0_in  = usbredir_ep_index(0x80);
    ei.type[ep0_out] = ei.type[ep0_in] = 0;   /* control */
    ei.max_packet_size[ep0_out] = ei.max_packet_size[ep0_in] = dd[7];

    /* Serialize ep_info to the length the *negotiated* caps imply. The peer
     * reads the trailing arrays only for caps both sides advertise:
     *   max_packet_size  <- cap_ep_info_max_packet_size
     *   max_streams      <- cap_bulk_streams   (NOT a separate cap)
     * Sending the wrong length desyncs its parser. */
    size_t eilen = offsetof(usbredir_ep_info_header, max_packet_size);
    if (caps & (1u << USB_REDIR_CAP_EP_INFO_MAX_PACKET_SIZE))
        eilen = offsetof(usbredir_ep_info_header, max_streams);
    if (caps & (1u << USB_REDIR_CAP_BULK_STREAMS))
        eilen = sizeof(usbredir_ep_info_header);   /* includes max_streams */

    LOG_DEBUG("announce: negotiated caps=0x%08x ep_info=%zu bytes", caps, eilen);

    if (usb_redir_send(sock, RT_INTERFACE_INFO, 0, &ii, sizeof(ii)) < 0 ||
        usb_redir_send(sock, RT_EP_INFO, 0, &ei, eilen) < 0)
        return -1;

    /* Device descriptor: class @4/5/6, idVendor @8, idProduct @10, bcd @12. */
    usbredir_device_connect_header c;
    memset(&c, 0, sizeof(c));
    /* bMaxPacketSize0==8 is a full-speed value; announcing HIGH would be
     * inconsistent. TODO: expose speed via the backend. */
    c.speed              = USB_REDIR_SPEED_FULL;
    c.device_class       = dd[4];
    c.device_subclass    = dd[5];
    c.device_protocol    = dd[6];
    c.vendor_id          = (uint16_t)(dd[8]  | (dd[9]  << 8));
    c.product_id         = (uint16_t)(dd[10] | (dd[11] << 8));
    c.device_version_bcd = (uint16_t)(dd[12] | (dd[13] << 8));

    LOG_INFO("Announcing device %04x:%04x, %u interface(s)",
             c.vendor_id, c.product_id, ii.interface_count);

    /* device_version_bcd is only present when connect_device_version is
     * negotiated; otherwise the peer expects the shorter header. */
    size_t clen = offsetof(usbredir_device_connect_header, device_version_bcd);
    if (caps & (1u << USB_REDIR_CAP_CONNECT_DEVICE_VERSION))
        clen = sizeof(usbredir_device_connect_header);

    return usb_redir_send(sock, RT_DEVICE_CONNECT, 0, &c, clen);
}

/* ------------------------------------------------------------------ *
 * Control transfers                                                   *
 * ------------------------------------------------------------------ */

static int reply_control(int sock, int32_t id,
                         const usbredir_control_packet_header *setup,
                         uint8_t status, const uint8_t *data, uint16_t n)
{
    uint8_t buf[USB_REDIR_BUF_SIZE];
    usbredir_control_packet_header h = *setup;
    h.status = status;
    h.length = n;

    memcpy(buf, &h, sizeof(h));
    if (data && n)
        memcpy(buf + sizeof(h), data, n);

    return usb_redir_send(sock, RT_CONTROL_PACKET, id, buf, sizeof(h) + n);
}

static void handle_control(int sock, usb_device_t *dev, int32_t id,
                           const uint8_t *payload, size_t len)
{
    if (len < sizeof(usbredir_control_packet_header)) {
        LOG_WARN("control packet too short (%zu bytes)", len);
        return;
    }

    usbredir_control_packet_header setup;
    memcpy(&setup, payload, sizeof(setup));

    /* Standard GET_DESCRIPTOR (device-to-host). */
    if (setup.requesttype == USB_DIR_IN &&
        setup.request == USB_REQ_GET_DESCRIPTOR) {
        uint8_t  dtype = (uint8_t)(setup.value >> 8);
        uint8_t  dindex = (uint8_t)(setup.value & 0xff);
        const uint8_t *desc;
        uint16_t dlen;

        if (dev->ops->get_descriptor(dev, dtype, dindex, setup.index,
                                     &desc, &dlen) < 0) {
            LOG_DEBUG("stall: no descriptor type=0x%02x index=%u", dtype, dindex);
            reply_control(sock, id, &setup, 1, NULL, 0);  /* stall */
            return;
        }

        uint16_t n = dlen < setup.length ? dlen : setup.length;
        LOG_DEBUG("GET_DESCRIPTOR type=0x%02x index=%u -> %u bytes",
                  dtype, dindex, n);
        reply_control(sock, id, &setup, 0, desc, n);
        return;
    }

    /* Class/vendor request: hand to the backend. */
    if (dev->ops->control) {
        uint8_t data[USB_REDIR_BUF_SIZE];
        uint16_t n = 0;
        if (dev->ops->control(dev, &setup, data, &n) == 0) {
            reply_control(sock, id, &setup, 0, data, n);
            return;
        }
    }

    LOG_DEBUG("stall: unhandled control req=0x%02x type=0x%02x",
              setup.request, setup.requesttype);
    reply_control(sock, id, &setup, 1, NULL, 0);  /* stall */
}

/* ------------------------------------------------------------------ *
 * Interrupt IN production                                             *
 * ------------------------------------------------------------------ */

static void push_interrupt_reports(int sock, usb_device_t *dev, uint8_t ep)
{
    if (!dev->ops->ep_in)
        return;

    /* Drain whatever the backend wants to send right now. */
    for (;;) {
        uint8_t data[USB_REDIR_BUF_SIZE];
        int n = dev->ops->ep_in(dev, ep, data, sizeof(data));
        if (n <= 0)
            break;

        uint8_t buf[USB_REDIR_BUF_SIZE];
        usbredir_interrupt_packet_header h = { ep, 0, (uint16_t)n };
        memcpy(buf, &h, sizeof(h));
        memcpy(buf + sizeof(h), data, (size_t)n);

        if (usb_redir_send(sock, RT_INTERRUPT_PACKET, 0, buf, sizeof(h) + n) < 0)
            break;
        LOG_OUT("interrupt report on ep 0x%02x (%d bytes)", ep, n);
    }
}

/* ------------------------------------------------------------------ *
 * Main dispatch loop                                                  *
 * ------------------------------------------------------------------ */

int usb_core_run(int sock, usb_device_t *dev)
{
    usbredir_header hdr;
    uint8_t payload[USB_REDIR_BUF_SIZE];
    size_t len;

    /* 1. Handshake: peer hello, then ours. Ignore idle timeouts. */
    int r;
    while ((r = usb_redir_recv(sock, &hdr, payload, sizeof(payload), &len)) == 1)
        ;
    if (r < 0)
        return -1;
    if (hdr.type != RT_HELLO) {
        LOG_WARN("expected hello, got type %u", hdr.type);
        return -1;
    }
    LOG_INFO("Peer hello: \"%.63s\"", (char *)payload);

    uint32_t peer_caps = 0;
    if (len >= sizeof(usbredir_hello_header)) {
        memcpy(&peer_caps, payload + 64, sizeof(peer_caps));
        LOG_DEBUG("peer capabilities[0]=0x%08x", peer_caps);
    }
    uint32_t caps = USB_REDIR_OUR_CAPS & peer_caps;

    if (send_hello(sock) < 0)
        return -1;

    /* 2. Announce the emulated device using the negotiated capabilities. */
    if (announce_device(sock, dev, caps) < 0)
        return -1;

    /* 3. Serve requests until the peer goes away. The socket recv timeout
     * turns an idle wait into a periodic tick, so streaming backends make
     * progress even when no request is pending. */
    for (;;) {
        int rc = usb_redir_recv(sock, &hdr, payload, sizeof(payload), &len);
        if (rc < 0)
            break;              /* EOF / hard error: peer gone */
        if (rc == 1) {          /* idle: no request pending */
            if (dev->ops->tick)
                dev->ops->tick(dev);
            continue;
        }

        switch (hdr.type) {
        case RT_CONTROL_PACKET:
            handle_control(sock, dev, hdr.id, payload, len);
            break;

        case RT_START_INTERRUPT_RECEIVING:
            if (len >= 1) {
                uint8_t ep = payload[0];
                LOG_DEBUG("start interrupt receiving on ep 0x%02x", ep);
                push_interrupt_reports(sock, dev, ep);
            }
            break;

        case RT_STOP_INTERRUPT_RECEIVING:
            LOG_DEBUG("stop interrupt receiving");
            break;

        case RT_INTERRUPT_PACKET:  /* OUT (e.g. keyboard LEDs) */
            if (dev->ops->ep_out && len > sizeof(usbredir_interrupt_packet_header)) {
                usbredir_interrupt_packet_header h;
                memcpy(&h, payload, sizeof(h));
                dev->ops->ep_out(dev, h.endpoint,
                                 payload + sizeof(h), len - sizeof(h));
            }
            break;

        default:
            LOG_DEBUG("unhandled packet type=%u len=%zu", hdr.type, len);
            break;
        }

        if (dev->ops->tick)
            dev->ops->tick(dev);
    }

    return 0;
}
