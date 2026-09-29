/*
 * record.c – Session-level glue between a running USB session and the pcap
 * writer.
 *
 * The only state held here is a single static pcap_writer_t pointer.
 * All pcap/pcapng framing is delegated entirely to pcap_writer_*; this
 * module has no knowledge of the transport layer or raw socket frames.
 */

#include "record.h"
#include "log.h"

static pcap_writer_t *g_writer;

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

void record_close(void)
{
    if (g_writer) {
        pcap_writer_close(g_writer);
        g_writer = NULL;
    }
}
