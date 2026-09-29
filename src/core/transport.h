#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* Select the on-wire packet-header width. When enabled the header carries a
 * 64-bit id (16 bytes) instead of a 32-bit id (12 bytes). Must be turned on
 * only after the hello exchange, if cap_64bits_ids was negotiated. */
void transport_set_id64(int enable);

/* Resolve host:port and open a TCP connection. Returns fd, or -1 on error. */
int connect_to_server(const char *host, const char *port);

/* Receive exactly `len` bytes. If `buf` is NULL the data is discarded.
 * Returns bytes read; 0 on an idle recv timeout at a frame boundary;
 * -1 on error/EOF. */
int recv_all(int sock, uint8_t *buf, size_t len);

/* Send one usb frame: header (12 or 16 bytes) + payload. Returns 0,
 * or -1. */
int usb_conn_send(int sock, uint32_t type, int64_t id,
                   const void *payload, size_t len);

/* Receive one usb frame. Fills `hdr`, copies the payload into `buf`
 * (capacity `cap`) and stores its length in `*payload_len`.
 * Returns 0 on a frame; 1 on an idle timeout (no frame, keep waiting);
 * -1 on error/EOF/oversized payload. */
int usb_conn_recv(int sock, usb_header_t *hdr,
                   uint8_t *buf, size_t cap, size_t *payload_len);

#endif /* TRANSPORT_H */
