#ifndef RECORD_H
#define RECORD_H

#include "pcap.h"
#include "protocol.h"
#include "transport.h"

/*
 * record.h – Thin session-level glue between a running USB session and the
 * pcap writer.  Callers only deal with open/add/close; all pcap/pcapng
 * framing is handled internally by pcap_writer_t.
 *
 * Replay is intentionally absent: use pcap_trace_load() / pcap_trace_get()
 * from pcap.h directly.
 */

/* Open `path` for recording live USB transfers (pcap or pcapng, inferred from
 * the file extension).  Returns 0, or -1 on error. */
int  record_open(const char *path);

/* Append one decoded USB transfer to the open recording. */
void record_add(const pcap_packet_t *pkt);

/* Record device announcement (device descriptor + config descriptor) */
void record_device_announce(const uint8_t *device_desc, size_t ddlen,
                            const uint8_t *config_desc, size_t cdlen,
                            const usb_record_ctx_t *ctx);

/* Record wire frames from transport send / recv */
void record_conn_send(uint32_t type, int64_t id, const void *payload, size_t len,
                      const usb_record_ctx_t *ctx);
void record_conn_recv(const usb_header_t *hdr, const void *payload, size_t len,
                      const usb_record_ctx_t *ctx);

/* Flush and close the recording. Safe to call even if record_open was never
 * called or failed. */
void record_close(void);

#endif /* RECORD_H */
