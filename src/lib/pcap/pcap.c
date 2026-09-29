/*
 * pcap.c – usbmon pcap/pcapng read + write. STUB.
 *
 * Reader TODO:
 *   - Detect container: classic pcap (magic 0xa1b2c3d4 / 0xd4c3b2a1, plus the
 *     nanosecond variants 0xa1b23c4d) vs pcapng (block type 0x0a0d0d0a).
 *   - Honour byte order from the magic / pcapng byte-order magic.
 *   - Require link type 189 (USB_LINUX) or 220 (USB_LINUX_MMAPPED); reject
 *     anything else so a non-usbmon capture fails loud.
 *   - Per record: parse the usbmon header (48 vs 64 bytes), pull bus/device/
 *     endpoint, xfer type+dir, the SETUP block for control transfers, and the
 *     payload length; copy payload into trace-owned storage.
 *   - pcapng: walk Section Header / Interface Description / Enhanced Packet
 *     blocks; skip everything else.
 *
 * Writer TODO:
 *   - Emit a pcapng Section Header + one Interface Description block with
 *     link type 220 (USB_LINUX_MMAPPED).
 *   - Per pcap_writer_add: synthesize the usbmon header from pcap_packet_t and
 *     wrap payload in an Enhanced Packet Block.
 */

#include "pcap.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>

struct pcap_trace {
    GArray    *packets;   /* pcap_packet_t */
    GPtrArray *payloads;  /* GByteArray*, backing pcap_packet_t.data */
};

struct pcap_writer {
    pcap_format_t format;
    FILE *fp;
    char *path;
};

/* ---- Reader ------------------------------------------------------------ */

pcap_trace_t *pcap_trace_load(const char *path)
{
    (void)path;
    LOG_WARN("pcap: reader not implemented yet (%s)", path);
    /* TODO: open file, sniff container/byte order, decode usbmon records. */
    return NULL;
}

size_t pcap_trace_count(const pcap_trace_t *t)
{
    return (t && t->packets) ? t->packets->len : 0;
}

const pcap_packet_t *pcap_trace_get(const pcap_trace_t *t, size_t i)
{
    if (!t || !t->packets || i >= t->packets->len)
        return NULL;
    return &g_array_index(t->packets, pcap_packet_t, i);
}

void pcap_trace_free(pcap_trace_t *t)
{
    if (!t)
        return;
    if (t->packets)
        g_array_free(t->packets, TRUE);
    if (t->payloads)
        g_ptr_array_free(t->payloads, TRUE);
    g_free(t);
}

/* ---- Helpers ----------------------------------------------------------- */

/* Check if the file belongs to legacy format or PCAPNG */
pcap_format_t pcap_format_get(const char *str) {
    if (!str)
        return FORMAT_LEGACY_PCAP;
    
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(".pcapng");
    
    // A string cannot end with a suffix longer than itself
    if (suffix_len > str_len)
        return FORMAT_LEGACY_PCAP;
    
    // Compare the end of the string with the suffix `.pcapng`
    if (!strcmp(str + (str_len - suffix_len), ".pcapng"))
        return FORMAT_PCAPNG;

    return FORMAT_LEGACY_PCAP;
}

/* Helper to safely write 4-byte padding bytes required by PCAPNG specifications */
static void pcap_write_ng_padding(FILE *fp,
                                  size_t unpadded_len) {
    size_t remainder = unpadded_len % 4;
    if (remainder != 0) {
        uint32_t zero = 0;
        fwrite(&zero, 1, 4 - remainder, fp);
    }
}

/* Helper to map your custom pcap_packet_t layout into an explicit mon_bin_hdr packet context */
static void pcap_init_usbmon_header(usbmon_header_t *usb_hdr,
                                    const pcap_packet_t *p,
                                    uint32_t sec, uint32_t usec) {
    memset(usb_hdr, 0, sizeof(usbmon_header_t));
    
    uint8_t final_ep = p->endpoint & 0x0F; 
    if (p->dir == PCAP_DIR_IN) {
        final_ep |= 0x80;
        usb_hdr->type = 'C'; /* Callback */
    } else {
        usb_hdr->type = 'S'; /* Submit */
    }

    usb_hdr->id        = (uint64_t)(uintptr_t)p;
    usb_hdr->xfer_type = (unsigned char)p->xfer;
    usb_hdr->epnum     = final_ep;
    usb_hdr->devnum    = p->device;
    usb_hdr->busnum    = p->bus;
    usb_hdr->ts_sec    = sec;
    usb_hdr->ts_usec   = usec;
    usb_hdr->status    = 0;
    usb_hdr->length    = (uint32_t)p->data_len;
    usb_hdr->len_cap   = (uint32_t)p->data_len;

    if (p->xfer == PCAP_XFER_CONTROL && p->setup_valid) {
        usb_hdr->flag_setup = '-';
        memcpy(usb_hdr->s.setup, p->setup, 8);
    } else {
        usb_hdr->flag_setup = 0;
    }
    usb_hdr->flag_data = (p->data_len > 0) ? '=' : '0';
}

/* ---- Writer ------------------------------------------------------------ */

void pcap_writer_open_legacy_header(FILE *fp)
{
    /* Write Classic Legacy Global Header */
    pcap_header_t global_hdr = {
        .magic_number = 0xa1b2c3d4,
        .version_major = 2,
        .version_minor = 4,
        .thiszone = 0,
        .sigfigs = 0,
        .snaplen = 65535,
        .network = 189 /* DLT_USB_LINUX */
    };
    fwrite(&global_hdr, sizeof(pcap_header_t), 1, fp);
}

void pcap_writer_open_ng_header(FILE *fp)
{
    /* Write PCAPNG Section Header Block (SHB) */
    pcapng_shb_t shb = {
        .block_type = 0x0A0D0D0A,
        /* Header + trailing length block */
        .block_len = sizeof(pcapng_shb_t) + sizeof(uint32_t), 
        .byte_order_magic = 0x1A2B3C4D,
        .major_version = 1,
        .minor_version = 0,
        .section_len = -1
    };
    fwrite(&shb, sizeof(pcapng_shb_t), 1, fp);
    fwrite(&shb.block_len, sizeof(uint32_t), 1, fp);

    /* Write PCAPNG Interface Description Block (IDB) for USB */
    pcapng_idb_t idb = {
        .block_type = 0x00000001,
        .block_len = sizeof(pcapng_idb_t) + sizeof(uint32_t),
        .link_type = 189, /* DLT_USB_LINUX */
        .reserved = 0,
        .snaplen = 65535
    };
    fwrite(&idb, sizeof(pcapng_idb_t), 1, fp);
    fwrite(&idb.block_len, sizeof(uint32_t), 1, fp);
}

pcap_writer_t *pcap_writer_open(const char *path)
{
    pcap_writer_t *w = g_new0(pcap_writer_t, 1);

    w->path = g_strdup(path);
    w->fp = fopen(path, "wb");
    w->format = pcap_format_get(path);

    if (!w->fp) {
        LOG_ERR("Failed to open file: %s", strerror(errno));
        g_free(w->path);
        g_free(w);
        return NULL;
    }

    if (w->format == FORMAT_PCAPNG) {
        pcap_writer_open_ng_header(w->fp);
    } else {
        pcap_writer_open_legacy_header(w->fp);
    }
    fflush(w->fp);  /* survive a Ctrl+C before the first packet */

    return w;
}

/* Classic Record Output Wrapper */
void pcap_writer_add_pcap(pcap_writer_t *w,
                          const pcap_packet_t *pkt,
                          usbmon_header_t usb_hdr,
                          uint32_t payload_and_meta_len,
                          uint32_t sec,
                          uint32_t usec)
{
    pcap_rec_header_t record_hdr = {
        .ts_sec = sec,
        .ts_usec = usec,
        .incl_len = payload_and_meta_len,
        .orig_len = payload_and_meta_len
    };
    fwrite(&record_hdr, sizeof(pcap_rec_header_t), 1, w->fp);
            
    // Raw sequential data block drop
    fwrite(&usb_hdr, sizeof(usbmon_header_t), 1, w->fp);
    if (pkt->data_len > 0 && pkt->data != NULL) {
        fwrite(pkt->data, 1, pkt->data_len, w->fp);
    }
}

void pcap_writer_add_pcapng(pcap_writer_t *w,
                            const pcap_packet_t *pkt,
                            usbmon_header_t usb_hdr,
                            uint32_t payload_and_meta_len)
{
    /* Enhanced Packet Block Processing Matrix */
    size_t unpadded_payload_len = payload_and_meta_len;
    size_t padding_needed = (4 - (unpadded_payload_len % 4)) % 4;
            
    uint32_t total_epb_block_len = sizeof(pcapng_epb_t) +
                                   unpadded_payload_len +
                                   padding_needed + sizeof(uint32_t);

    pcapng_epb_t epb = {
        .block_type = 0x00000006,
        .block_len = total_epb_block_len,
        .interface_id = 0, // Maps directly to IDB 0 above
        .ts_high = (uint32_t)(pkt->ts_usec >> 32),
        .ts_low = (uint32_t)(pkt->ts_usec & 0xFFFFFFFFULL),
        .cap_packet_len = payload_and_meta_len,
        .orig_packet_len = payload_and_meta_len
    };

    // Write EPB Structural Header
    fwrite(&epb, sizeof(pcapng_epb_t), 1, w->fp);

    // Write internal usbmon context block + data payload
    fwrite(&usb_hdr, sizeof(usbmon_header_t), 1, w->fp);

    if (pkt->data_len > 0 && pkt->data != NULL)
        fwrite(pkt->data, 1, pkt->data_len, w->fp);

    // Write alignment padding to reach a stable 32-bit offset boundary
    pcap_write_ng_padding(w->fp, unpadded_payload_len);

    // Write structural boundary trailing tracking length descriptor
    fwrite(&total_epb_block_len, sizeof(uint32_t), 1, w->fp);
}

void pcap_writer_add(pcap_writer_t *w, const pcap_packet_t *pkt)
{
    usbmon_header_t usb_hdr;
    uint32_t sec = (uint32_t)(pkt->ts_usec / 1000000ULL);
    uint32_t usec = (uint32_t)(pkt->ts_usec % 1000000ULL);

    pcap_init_usbmon_header(&usb_hdr, pkt, sec, usec);

    uint32_t payload_and_meta_len = sizeof(usbmon_header_t) + pkt->data_len;

    if (w->format == FORMAT_LEGACY_PCAP) {
        pcap_writer_add_pcap(w, pkt, usb_hdr, payload_and_meta_len, sec, usec);
    } else if (w->format == FORMAT_PCAPNG) {
        pcap_writer_add_pcapng(w, pkt, usb_hdr, payload_and_meta_len);
    }
    fflush(w->fp);  /* survive a crash mid-run */
}

void pcap_writer_close(pcap_writer_t *w)
{
    if (!w)
        return;
    if (w->fp)
        fclose(w->fp);
    if (w->path)
        g_free(w->path);
    g_free(w);
}
