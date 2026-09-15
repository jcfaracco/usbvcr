#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include "transport.h"
#include "protocol.h"
#include "log.h"

/* ------------------------------------------------------------------ *
 * Socket helpers                                                      *
 * ------------------------------------------------------------------ */

int connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints, *res;
    int sock, ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo(host, port, &hints, &res);
    if (ret != 0) {
        LOG_ERR("getaddrinfo: %s", gai_strerror(ret));
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
int recv_all(int sock, uint8_t *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        uint8_t tmp[512];
        size_t remaining  = len - total;
        size_t chunk_size = buf ? remaining : (remaining > sizeof(tmp) ? sizeof(tmp) : remaining);
        uint8_t *target   = buf ? buf + total : tmp;

        ssize_t n = recv(sock, target, chunk_size, 0);
        if (n == 0) {
            LOG_ERR("Connection closed by peer.");
            return -1;
        } else if (n < 0) {
            perror("recv");
            return -1;
        }
        total += (size_t)n;
    }
    return (int)total;
}

/* ------------------------------------------------------------------ *
 * usbredir frame layer                                                *
 * ------------------------------------------------------------------ */

int usb_redir_send(int sock, uint32_t type, int32_t id,
                   const void *payload, size_t len)
{
    usbredir_header hdr = { type, (uint32_t)len, id };
    uint8_t frame[USB_REDIR_BUF_SIZE];

    if (sizeof(hdr) + len > sizeof(frame)) {
        LOG_ERR("send: payload too large (%zu bytes)", len);
        return -1;
    }

    memcpy(frame, &hdr, sizeof(hdr));
    if (payload && len)
        memcpy(frame + sizeof(hdr), payload, len);

    size_t frame_len = sizeof(hdr) + len;

    if (send(sock, frame, frame_len, 0) < 0) {
        perror("send");
        return -1;
    }
    return 0;
}

int usb_redir_recv(int sock, usbredir_header *hdr,
                   uint8_t *buf, size_t cap, size_t *payload_len)
{
    uint8_t hbuf[sizeof(*hdr)];
    if (recv_all(sock, hbuf, sizeof(hbuf)) <= 0)
        return -1;
    memcpy(hdr, hbuf, sizeof(*hdr));

    if (hdr->length > cap) {
        LOG_ERR("recv: payload %u exceeds buffer %zu", hdr->length, cap);
        return -1;
    }

    if (hdr->length && recv_all(sock, buf, hdr->length) <= 0)
        return -1;

    *payload_len = hdr->length;
    return 0;
}
