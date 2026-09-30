/*
 * pcap_backend.c – Generic ops backend backed by a usbmon capture. STUB.
 *
 * Maps usb_device_ops -> decoded pcap transfers:
 *   get_descriptor -> replay the GET_DESCRIPTOR reply captured for (type,index)
 *   control        -> replay the response captured for the matching SETUP
 *   ep_in          -> hand back the next captured IN payload for that endpoint
 *   ep_out         -> consume/verify against the captured OUT payload
 *   tick           -> advance the replay cursor (timing/streaming)
 *
 * The heavy lifting (indexing transfers by request/endpoint, cursor state) is
 * TODO; today every op stalls/NAKs so the engine stays well-behaved.
 */

#include "pcap_backend.h"
#include "pcap/pcap.h"
#include "log.h"

#include <stdlib.h>
#include <glib.h>

typedef struct {
    pcap_trace_t *trace;
    size_t        cursor;   /* next transfer to replay */
} pcap_backend_state_t;

static int pb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                             uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)type; (void)index; (void)lang; (void)out; (void)len;
    /* TODO: find the captured GET_DESCRIPTOR reply for (type,index). */
    return -1;
}

static int pb_control(usb_device_t *dev,
                      const usb_control_packet_header_t *setup,
                      uint8_t *data, uint16_t *len)
{
    (void)dev; (void)setup; (void)data; (void)len;
    /* TODO: match SETUP against captured control transfers, replay the reply. */
    return -1;
}

static int pb_ep_in(usb_device_t *dev, uint8_t ep, uint8_t *buf, size_t cap)
{
    (void)dev; (void)ep; (void)buf; (void)cap;
    /* TODO: return the next captured IN payload for `ep`. */
    return 0;   /* NAK */
}

static int pb_ep_out(usb_device_t *dev, uint8_t ep, const uint8_t *buf, size_t len)
{
    (void)dev; (void)ep; (void)buf; (void)len;
    /* TODO: consume/verify against the captured OUT payload. */
    return 0;
}

static void pb_tick(usb_device_t *dev)
{
    (void)dev;
    /* TODO: advance replay cursor per capture timing. */
}

static const usb_device_ops_t pcap_ops = {
    .name           = "pcap",
    .get_descriptor = pb_get_descriptor,
    .control        = pb_control,
    .ep_in          = pb_ep_in,
    .ep_out         = pb_ep_out,
    .tick           = pb_tick,
};

usb_device_t *pcap_backend_create(const char *path)
{
    pcap_trace_t *trace = pcap_trace_load(path);
    if (!trace) {
        LOG_ERR("pcap_backend: failed to load capture: %s", path);
        return NULL;
    }

    pcap_backend_state_t *st = g_new0(pcap_backend_state_t, 1);
    usb_device_t *dev = g_new0(usb_device_t, 1);
    st->trace  = trace;
    st->cursor = 0;
    dev->ops   = &pcap_ops;
    dev->state = st;
    return dev;
}
