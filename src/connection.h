#ifndef _CONNECTION_H_
#define _CONNECTION_H_

static int connect_to_server(const char *host, const char *port);

/* Receive exactly `len` bytes (blocking) */
static int recv_all(int sock, uint8_t *buf, size_t len);

#endif
