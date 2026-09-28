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
    FILE *fp;
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

/* ---- Writer ------------------------------------------------------------ */

pcap_writer_t *pcap_writer_open(const char *path)
{
    (void)path;
    LOG_WARN("pcap: writer not implemented yet (%s)", path);
    /* TODO: open file, write pcapng SHB + IDB (linktype 220). */
    return NULL;
}

int pcap_writer_add(pcap_writer_t *w, const pcap_packet_t *pkt)
{
    (void)w; (void)pkt;
    /* TODO: encode usbmon header + payload into an Enhanced Packet Block. */
    return -1;
}

void pcap_writer_close(pcap_writer_t *w)
{
    if (!w)
        return;
    if (w->fp)
        fclose(w->fp);
    g_free(w);
}
