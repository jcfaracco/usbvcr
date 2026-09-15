#ifndef USBREDIR_CLIENT_TRANSPORT_H
#define USBREDIR_CLIENT_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "protocol.h"

/* If set, every frame passed to usb_redir_send is also written to `fp`
 * (length-prefixed) for later replay. Pass NULL to disable. */
void transport_set_record(FILE *fp);

/* Resolve host:port and open a TCP connection. Returns fd, or -1 on error. */
int connect_to_server(const char *host, const char *port);

/* Receive exactly `len` bytes. If `buf` is NULL the data is discarded.
 * Returns bytes read; 0 on an idle recv timeout at a frame boundary;
 * -1 on error/EOF. */
int recv_all(int sock, uint8_t *buf, size_t len);

/* Send one usbredir frame: 12-byte header + payload. Returns 0, or -1. */
int usb_redir_send(int sock, uint32_t type, int32_t id,
                   const void *payload, size_t len);

/* Receive one usbredir frame. Fills `hdr`, copies the payload into `buf`
 * (capacity `cap`) and stores its length in `*payload_len`.
 * Returns 0 on a frame; 1 on an idle timeout (no frame, keep waiting);
 * -1 on error/EOF/oversized payload. */
int usb_redir_recv(int sock, usbredir_header *hdr,
                   uint8_t *buf, size_t cap, size_t *payload_len);

#endif
