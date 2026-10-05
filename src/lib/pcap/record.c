/*
 * record.c – Session-level glue between a running USB session and the pcap
 * writer.
 *
 * All pcap/pcapng framing is delegated entirely to pcap_writer_*; this
 * module provides record_conn_send and record_conn_recv to translate
 * wire-level usbredir frames into standard usbmon pcap transfers.
 */

#include <time.h>
#include <string.h>

#include "record.h"
#include "log.h"

static pcap_writer_t *g_writer;

static uint64_t ts_usec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void init_setup_packet(uint8_t *dst, const usb_control_packet_header_t *setup)
{
    dst[0] = setup->requesttype;
    dst[1] = setup->request;
    dst[2] = (uint8_t)(setup->value & 0xff);
    dst[3] = (uint8_t)((setup->value >> 8) & 0xff);
    dst[4] = (uint8_t)(setup->index & 0xff);
    dst[5] = (uint8_t)((setup->index >> 8) & 0xff);
    dst[6] = (uint8_t)(setup->length & 0xff);
    dst[7] = (uint8_t)((setup->length >> 8) & 0xff);
}

int record_open(const char *path)
{
    g_writer = pcap_writer_open(path);
    if (!g_writer) {
        LOG_ERR("record: failed to open capture file: %s", path);
        return -1;
    }
    LOG_INFO("Recording transfers to %s", path);
    return 0;
}

void record_add(const pcap_packet_t *pkt)
{
    if (g_writer)
        pcap_writer_add(g_writer, pkt);
}

void record_device_announce(const uint8_t *device_desc, size_t ddlen,
                            const uint8_t *config_desc, size_t cdlen,
                            const usb_record_ctx_t *ctx)
{
    if (!g_writer || !ctx)
        return;

    /* 1. Synthetic GET_DESCRIPTOR (Device) Submit + Complete */
    if (device_desc && ddlen > 0) {
        usb_control_packet_header_t setup = {
            .endpoint    = 0,
            .requesttype = 0x80, /* IN */
            .request     = USB_REQ_GET_DESCRIPTOR,
            .value       = (USB_DESC_DEVICE << 8),
            .index       = 0,
            .length      = (uint16_t)ddlen,
        };

        pcap_packet_t submit = {
            .ts_usec     = ts_usec(),
            .urb_id      = 1,
            .bus         = ctx->bus,
            .device      = ctx->device,
            .endpoint    = 0x80,
            .dir         = PCAP_DIR_IN,
            .xfer        = PCAP_XFER_CONTROL,
            .event_type  = 'S',
            .setup_valid = 1,
        };
        init_setup_packet(submit.setup, &setup);
        record_add(&submit);

        /* usbmon only carries the setup packet on the Submit; Complete has
         * no setup of its own (see pcap_init_usbmon_header). */
        pcap_packet_t complete = {
            .ts_usec    = ts_usec(),
            .urb_id     = 1,
            .bus        = ctx->bus,
            .device     = ctx->device,
            .endpoint   = 0x80,
            .dir        = PCAP_DIR_IN,
            .xfer       = PCAP_XFER_CONTROL,
            .event_type = 'C',
            .data       = device_desc,
            .data_len   = ddlen,
        };
        record_add(&complete);
    }

    /* 2. Synthetic GET_DESCRIPTOR (Configuration) Submit + Complete */
    if (config_desc && cdlen > 0) {
        usb_control_packet_header_t setup = {
            .endpoint    = 0,
            .requesttype = 0x80, /* IN */
            .request     = USB_REQ_GET_DESCRIPTOR,
            .value       = (USB_DESC_CONFIG << 8),
            .index       = 0,
            .length      = (uint16_t)cdlen,
        };

        pcap_packet_t submit = {
            .ts_usec     = ts_usec(),
            .urb_id      = 2,
            .bus         = ctx->bus,
            .device      = ctx->device,
            .endpoint    = 0x80,
            .dir         = PCAP_DIR_IN,
            .xfer        = PCAP_XFER_CONTROL,
            .event_type  = 'S',
            .setup_valid = 1,
        };
        init_setup_packet(submit.setup, &setup);
        record_add(&submit);

        pcap_packet_t complete = {
            .ts_usec    = ts_usec(),
            .urb_id     = 2,
            .bus        = ctx->bus,
            .device     = ctx->device,
            .endpoint   = 0x80,
            .dir        = PCAP_DIR_IN,
            .xfer       = PCAP_XFER_CONTROL,
            .event_type = 'C',
            .data       = config_desc,
            .data_len   = cdlen,
        };
        record_add(&complete);
    }
}

void record_conn_recv(const usb_header_t *hdr, const void *payload, size_t len,
                      const usb_record_ctx_t *ctx)
{
    if (!g_writer || !ctx)
        return;

    pcap_packet_t pkt = {
        .ts_usec    = ts_usec(),
        .urb_id     = (uint64_t)hdr->id,
        .bus        = ctx->bus,
        .device     = ctx->device,
        .event_type = 'S', /* Inbound from host = Submit */
    };

    switch (hdr->type) {
    case RT_CONTROL_PACKET:
        if (len < sizeof(usb_control_packet_header_t))
            return;
        {
            usb_control_packet_header_t setup;
            memcpy(&setup, payload, sizeof(setup));
            pkt.endpoint    = setup.endpoint;
            pkt.dir         = (setup.requesttype & USB_DIR_IN) ? PCAP_DIR_IN : PCAP_DIR_OUT;
            pkt.xfer        = PCAP_XFER_CONTROL;
            pkt.setup_valid = 1;
            init_setup_packet(pkt.setup, &setup);
            if (len > sizeof(setup)) {
                pkt.data     = (const uint8_t *)payload + sizeof(setup);
                pkt.data_len = len - sizeof(setup);
            }
            record_add(&pkt);
        }
        break;

    case RT_INTERRUPT_PACKET:
        if (len < sizeof(usb_interrupt_packet_header_t))
            return;
        {
            usb_interrupt_packet_header_t ih;
            memcpy(&ih, payload, sizeof(ih));
            pkt.endpoint = ih.endpoint & 0x0f; /* OUT */
            pkt.dir      = PCAP_DIR_OUT;
            pkt.xfer     = PCAP_XFER_INT;
            if (len > sizeof(ih)) {
                pkt.data     = (const uint8_t *)payload + sizeof(ih);
                pkt.data_len = len - sizeof(ih);
            }
            record_add(&pkt);
        }
        break;

    case RT_BULK_PACKET:
        if (len < sizeof(usb_interrupt_packet_header_t))
            return;
        {
            usb_interrupt_packet_header_t ih;
            memcpy(&ih, payload, sizeof(ih));
            pkt.endpoint = ih.endpoint & 0x0f; /* OUT */
            pkt.dir      = PCAP_DIR_OUT;
            pkt.xfer     = PCAP_XFER_BULK;
            if (len > sizeof(ih)) {
                pkt.data     = (const uint8_t *)payload + sizeof(ih);
                pkt.data_len = len - sizeof(ih);
            }
            record_add(&pkt);
        }
        break;

    case RT_START_INTERRUPT_RECEIVING:
        if (len >= 1) {
            uint8_t ep = ((const uint8_t *)payload)[0];
            pkt.endpoint    = ep | 0x80;
            pkt.dir         = PCAP_DIR_IN;
            pkt.xfer        = PCAP_XFER_INT;
            pkt.setup_valid = 0;
            record_add(&pkt);
        }
        break;

    case RT_SET_CONFIGURATION:
        pkt.endpoint    = 0;
        pkt.dir         = PCAP_DIR_OUT;
        pkt.xfer        = PCAP_XFER_CONTROL;
        pkt.setup_valid = 1;
        memset(pkt.setup, 0, sizeof(pkt.setup));
        pkt.setup[0] = 0x00;                      /* bmRequestType */
        pkt.setup[1] = USB_REQ_SET_CONFIGURATION; /* bRequest */
        pkt.setup[2] = (len >= 1) ? ((const uint8_t *)payload)[0] : 0;
        record_add(&pkt);
        break;

    default:
        break;
    }
}

void record_conn_send(uint32_t type, int64_t id, const void *payload, size_t len,
                      const usb_record_ctx_t *ctx)
{
    if (!g_writer || !ctx)
        return;

    pcap_packet_t pkt = {
        .ts_usec    = ts_usec(),
        .urb_id     = (uint64_t)id,
        .bus        = ctx->bus,
        .device     = ctx->device,
        .event_type = 'C', /* Outbound to host = Callback / Complete */
    };

    switch (type) {
    case RT_CONTROL_PACKET:
        if (len < sizeof(usb_control_packet_header_t))
            return;
        {
            usb_control_packet_header_t setup;
            memcpy(&setup, payload, sizeof(setup));
            pkt.endpoint    = setup.endpoint;
            pkt.dir         = (setup.requesttype & USB_DIR_IN) ? PCAP_DIR_IN : PCAP_DIR_OUT;
            pkt.xfer        = PCAP_XFER_CONTROL;
            /* This is the Complete/Callback side; usbmon only carries the
             * setup packet on the Submit (see pcap_init_usbmon_header). */
            if (len > sizeof(setup)) {
                pkt.data     = (const uint8_t *)payload + sizeof(setup);
                pkt.data_len = len - sizeof(setup);
            }
            record_add(&pkt);
        }
        break;

    case RT_INTERRUPT_PACKET:
        if (len < sizeof(usb_interrupt_packet_header_t))
            return;
        {
            usb_interrupt_packet_header_t ih;
            memcpy(&ih, payload, sizeof(ih));
            pkt.endpoint = ih.endpoint | 0x80; /* IN */
            pkt.dir      = PCAP_DIR_IN;
            pkt.xfer     = PCAP_XFER_INT;
            if (len > sizeof(ih)) {
                pkt.data     = (const uint8_t *)payload + sizeof(ih);
                pkt.data_len = len - sizeof(ih);
            }
            record_add(&pkt);
        }
        break;

    case RT_BULK_PACKET:
        if (len < sizeof(usb_interrupt_packet_header_t))
            return;
        {
            usb_interrupt_packet_header_t ih;
            memcpy(&ih, payload, sizeof(ih));
            pkt.endpoint = ih.endpoint | 0x80; /* IN */
            pkt.dir      = PCAP_DIR_IN;
            pkt.xfer     = PCAP_XFER_BULK;
            if (len > sizeof(ih)) {
                pkt.data     = (const uint8_t *)payload + sizeof(ih);
                pkt.data_len = len - sizeof(ih);
            }
            record_add(&pkt);
        }
        break;

    case RT_CONFIGURATION_STATUS:
        pkt.endpoint    = 0;
        pkt.dir         = PCAP_DIR_OUT;
        pkt.xfer        = PCAP_XFER_CONTROL;
        record_add(&pkt);
        break;

    default:
        break;
    }
}

void record_close(void)
{
    if (g_writer) {
        pcap_writer_close(g_writer);
        g_writer = NULL;
    }
}
