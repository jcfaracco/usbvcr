#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "connection.h"

/* ------------------------------------------------------------------ *
 * Socket helpers                                                      *
 * ------------------------------------------------------------------ */

static int connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints, *res;
    int sock, ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo(host, port, &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }

    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        perror("socket");
        freeaddrinfo(res);
        return -1;
    }

    /* Receive timeout so we don't block forever */
    struct timeval timeout = {5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        close(sock);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return sock;
}

/* Receive exactly `len` bytes (blocking) */
static int recv_all(int sock, uint8_t *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        uint8_t tmp[512];
        size_t remaining  = len - total;
        size_t chunk_size = buf ? remaining : (remaining > sizeof(tmp) ? sizeof(tmp) : remaining);
        uint8_t *target   = buf ? buf + total : tmp;

        ssize_t n = recv(sock, target, chunk_size, 0);
        if (n == 0) {
            fprintf(stderr, "Connection closed by peer.\n");
            return -1;
        } else if (n < 0) {
            perror("recv");
            return -1;
        }
        total += (size_t)n;
    }
    return (int)total;
}
