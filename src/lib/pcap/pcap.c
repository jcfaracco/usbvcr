/*
 * pcap.c – usbmon pcap/pcapng read + write.
 *
 * The reader mirrors the writer's own framing exactly (see pcap_writer_add):
 * container format is inferred from the file extension (pcap_format_get),
 * each record/block wraps one usbmon_header_t followed by its payload, and
 * for pcapng the Enhanced Packet Block's cap_packet_len covers header+
 * payload together. Decoded transfers are copied into trace-owned storage
 * (GByteArray per packet) so pcap_packet_t.data stays valid until
 * pcap_trace_free().
 */

#include "pcap.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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

/* Defined in the Helpers section below; used by both reader and writer. */
pcap_format_t pcap_format_get(const char *str);

/* ---- Reader ------------------------------------------------------------ */

static pcap_trace_t *trace_new(void)
{
    pcap_trace_t *t = g_new0(pcap_trace_t, 1);
    t->packets = g_array_new(FALSE, TRUE, sizeof(pcap_packet_t));
    t->payloads = g_ptr_array_new_with_free_func((GDestroyNotify)g_byte_array_unref);
    return t;
}

/* Decode a usbmon header into a pcap_packet_t. data/data_len are filled in
 * by the caller once the payload has been copied into trace-owned storage. */
static void pcap_decode_usbmon_header(const usbmon_header_t *h, pcap_packet_t *pkt)
{
    memset(pkt, 0, sizeof(*pkt));

    pkt->ts_usec     = (uint64_t)(int64_t)h->ts_sec * 1000000ULL + (uint32_t)h->ts_usec;
    pkt->urb_id      = h->id;
    pkt->bus         = (uint8_t)h->busnum;
    pkt->device      = h->devnum;
    pkt->endpoint    = h->epnum;
    pkt->dir         = (h->epnum & 0x80) ? PCAP_DIR_IN : PCAP_DIR_OUT;
    pkt->xfer        = (pcap_xfer_t)h->xfer_type;
    pkt->event_type  = (char)h->type;

    if (h->flag_setup == '-') {
        pkt->setup_valid = 1;
        memcpy(pkt->setup, h->s.setup, sizeof(pkt->setup));
    }
}

/* Append a decoded packet to the trace, copying `payload_len` bytes from
 * `payload` into trace-owned storage so pkt->data stays valid after the
 * read buffer is freed. */
static void trace_append(pcap_trace_t *t, pcap_packet_t pkt,
                         const uint8_t *payload, size_t payload_len)
{
    if (payload_len > 0) {
        GByteArray *owned = g_byte_array_sized_new((guint)payload_len);
        g_byte_array_append(owned, payload, (guint)payload_len);
        g_ptr_array_add(t->payloads, owned);
        pkt.data     = owned->data;
        pkt.data_len = payload_len;
    } else {
        pkt.data     = NULL;
        pkt.data_len = 0;
    }
    g_array_append_val(t->packets, pkt);
}

/* Read one usbmon_header_t + its payload (total `incl_len` bytes) and append
 * the decoded transfer to `t`. Returns 0 on success, -1 on short read. */
static int trace_read_record(pcap_trace_t *t, FILE *fp, uint32_t incl_len)
{
    g_autofree uint8_t *payload = NULL;
    usbmon_header_t uh;
    pcap_packet_t pkt;
    size_t payload_len;

    if (incl_len < sizeof(usbmon_header_t)) {
        LOG_ERR("pcap: record smaller than usbmon header (%u bytes)", incl_len);
        return -1;
    }

    if (fread(&uh, sizeof(uh), 1, fp) != 1)
        return -1;

    payload_len = incl_len - sizeof(uh);
    if (payload_len > 0) {
        payload = g_malloc(payload_len);
        if (fread(payload, 1, payload_len, fp) != payload_len)
            return -1;
    }

    pcap_decode_usbmon_header(&uh, &pkt);
    trace_append(t, pkt, payload, payload_len);
    return 0;
}

/* Classic legacy pcap: global header, then a stream of
 * [pcap_rec_header_t][usbmon_header_t][payload] records. */
static pcap_trace_t *pcap_trace_load_legacy(FILE *fp)
{
    pcap_header_t gh;
    if (fread(&gh, sizeof(gh), 1, fp) != 1) {
        LOG_ERR("pcap: truncated global header");
        return NULL;
    }
    if (gh.magic_number != PCAP_MAGIC_NUMBER_MS && gh.magic_number != PCAP_MAGIC_NUMBER_NS) {
        LOG_ERR("pcap: unrecognized magic number 0x%08x", gh.magic_number);
        return NULL;
    }

    pcap_trace_t *t = trace_new();

    pcap_rec_header_t rh;
    while (fread(&rh, sizeof(rh), 1, fp) == 1) {
        if (trace_read_record(t, fp, rh.incl_len) != 0)
            break;
    }
    return t;
}

/* pcapng: Section Header + Interface Description, then a stream of Enhanced
 * Packet Blocks, each [epb header][usbmon_header_t][payload][pad][trailer]. */
static pcap_trace_t *pcap_trace_load_ng(FILE *fp)
{
    pcapng_shb_t shb;
    uint32_t trailer;
    if (fread(&shb, sizeof(shb), 1, fp) != 1 || shb.block_type != 0x0A0D0D0A) {
        LOG_ERR("pcap: missing/invalid pcapng section header block");
        return NULL;
    }
    if (fread(&trailer, sizeof(trailer), 1, fp) != 1) {
        LOG_ERR("pcap: truncated section header block");
        return NULL;
    }

    pcapng_idb_t idb;
    if (fread(&idb, sizeof(idb), 1, fp) != 1 || idb.block_type != 0x00000001) {
        LOG_ERR("pcap: missing/invalid pcapng interface description block");
        return NULL;
    }
    if (fread(&trailer, sizeof(trailer), 1, fp) != 1) {
        LOG_ERR("pcap: truncated interface description block");
        return NULL;
    }
    if (idb.link_type != 189 && idb.link_type != 220) {
        LOG_ERR("pcap: unsupported link type %u (expected usbmon)", idb.link_type);
        return NULL;
    }

    pcap_trace_t *t = trace_new();

    pcapng_epb_t epb;
    while (fread(&epb, sizeof(epb), 1, fp) == 1) {
        if (epb.block_type != 0x00000006) {
            LOG_ERR("pcap: unexpected pcapng block type 0x%08x", epb.block_type);
            break;
        }

        if (trace_read_record(t, fp, epb.cap_packet_len) != 0)
            break;

        size_t padding = (4 - (epb.cap_packet_len % 4)) % 4;
        if (padding && fseek(fp, (long)padding, SEEK_CUR) != 0)
            break;

        if (fread(&trailer, sizeof(trailer), 1, fp) != 1 || trailer != epb.block_len) {
            LOG_ERR("pcap: malformed enhanced packet block trailer");
            break;
        }
    }
    return t;
}

pcap_trace_t *pcap_trace_load(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_ERR("pcap: failed to open %s: %s", path, strerror(errno));
        return NULL;
    }

    pcap_trace_t *t = (pcap_format_get(path) == FORMAT_PCAPNG)
                     ? pcap_trace_load_ng(fp)
                     : pcap_trace_load_legacy(fp);

    fclose(fp);
    return t;
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
    }

    if (p->event_type == 'S' || p->event_type == 'C' || p->event_type == 'E') {
        usb_hdr->type = (unsigned char)p->event_type;
    } else {
        usb_hdr->type = (p->dir == PCAP_DIR_IN) ? 'C' : 'S';
    }

    usb_hdr->id        = p->urb_id ? p->urb_id : (uint64_t)(uintptr_t)p;
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
        if (usb_hdr->type == 'S') {
            uint16_t wLength = (uint16_t)p->setup[6] | ((uint16_t)p->setup[7] << 8);
            if (wLength > usb_hdr->length)
                usb_hdr->length = wLength;
        }
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
        /* usb_hdr written per-record is the 64-byte usbmon_header_t (id..ndesc),
         * i.e. the MMAPPED layout - declaring 189 (legacy 48-byte) here makes
         * Wireshark misparse every record by 16 bytes. */
        .network = 220 /* DLT_USB_LINUX_MMAPPED */
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
        /* See pcap_writer_open_legacy_header(): must match the 64-byte
         * usbmon_header_t we actually write per Enhanced Packet Block. */
        .link_type = 220, /* DLT_USB_LINUX_MMAPPED */
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
