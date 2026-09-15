#ifndef USBREDIR_CLIENT_CONNECTION_H
#define USBREDIR_CLIENT_CONNECTION_H

#include <stddef.h>
#include <stdint.h>

/* Resolve host:port and open a TCP connection. Returns fd, or -1 on error. */
int connect_to_server(const char *host, const char *port);

/* Receive exactly `len` bytes (blocking). If `buf` is NULL the data is
 * discarded. Returns bytes read, or -1 on error/EOF. */
int recv_all(int sock, uint8_t *buf, size_t len);

#endif
