#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

#include "transport.h"
#include "protocol.h"
#include "bytes.h"
#include "log.h"

/* On-wire frame header field offsets. id is 4 or 8 bytes (see g_id64), giving
 * a 12- or 16-byte header. */
enum {
    HDR_OFF_TYPE   = 0,
    HDR_OFF_LENGTH = 4,
    HDR_OFF_ID     = 8,
};
#define HDR_SIZE_ID32 (HDR_OFF_ID + 4)   /* 32-bit id: 12-byte header */
#define HDR_SIZE_ID64 (HDR_OFF_ID + 8)   /* 64-bit id: 16-byte header */

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
        LOG_ERR("Error type: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    /* Receive timeout so we don't block forever */
    struct timeval timeout = {5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        LOG_ERR("Error type: %s", strerror(errno));
        close(sock);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return sock;
}

/* Receive exactly `len` bytes. Blocks until the frame completes.
 *
 * Return: number of bytes read on success; 0 if the recv timeout
 * (SO_RCVTIMEO) fires at a frame boundary with nothing read yet (idle,
 * caller should keep waiting); -1 on EOF or a hard error. A timeout that
 * hits mid-frame is not fatal — we keep waiting for the rest. */
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
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (total == 0)
                    return 0;   /* idle at frame boundary */
                continue;       /* partial frame: wait for the rest */
            }
            LOG_ERR("Error type: %s", strerror(errno));
            return -1;
        }
        total += (size_t)n;
    }
    return (int)total;
}

/* ------------------------------------------------------------------ *
 * usb frame layer                                                *
 * ------------------------------------------------------------------ */

static FILE *g_record_fp;

void transport_set_record(FILE *fp)
{
    g_record_fp = fp;
}

/* On-wire header id width. 0 = 32-bit id (12-byte header), 1 = 64-bit id
 * (16-byte header). Toggled by transport_set_id64 only after the hello
 * exchange, and only if cap_64bits_ids was negotiated. The hello itself is
 * always framed with the 12-byte header (sent pre-negotiation). */
static int g_id64;

void transport_set_id64(int enable)
{
    g_id64 = enable ? 1 : 0;
}

static size_t hdr_wire_size(void)
{
    return g_id64 ? HDR_SIZE_ID64 : HDR_SIZE_ID32;
}

int usb_conn_send(int sock, uint32_t type, int64_t id,
                  const void *payload, size_t len)
{
    uint8_t frame[USB_VCR_BUF_SIZE];
    size_t hs = hdr_wire_size();

    if (hs + len > sizeof(frame)) {
        LOG_ERR("send: payload too large (%zu bytes)", len);
        return -1;
    }

    /* Serialize the header field by field: type(4) length(4) id(4 or 8). */
    put_u32le(frame + HDR_OFF_TYPE,   type);
    put_u32le(frame + HDR_OFF_LENGTH, (uint32_t)len);
    if (g_id64)
        put_i64le(frame + HDR_OFF_ID, id);
    else
        put_i32le(frame + HDR_OFF_ID, (int32_t)id);
    if (payload && len)
        memcpy(frame + hs, payload, len);

    size_t frame_len = hs + len;

    if (g_record_fp) {
        uint32_t rec = (uint32_t)frame_len;
        fwrite(&rec, sizeof(rec), 1, g_record_fp);
        fwrite(frame, 1, frame_len, g_record_fp);
        fflush(g_record_fp);  /* survive a crash mid-run */
    }

    if (send(sock, frame, frame_len, 0) < 0) {
        LOG_ERR("Error type: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int usb_conn_recv(int sock, usb_header_t *hdr,
                  uint8_t *buf, size_t cap, size_t *payload_len)
{
    uint8_t hbuf[HDR_SIZE_ID64];
    size_t hs = hdr_wire_size();

    int r = recv_all(sock, hbuf, hs);
    if (r == 0)
        return 1;   /* idle timeout, no frame */
    if (r < 0)
        return -1;

    hdr->type   = get_u32le(hbuf + HDR_OFF_TYPE);
    hdr->length = get_u32le(hbuf + HDR_OFF_LENGTH);
    /* Narrow id sign-extends into the 64-bit field. */
    hdr->id = g_id64 ? get_i64le(hbuf + HDR_OFF_ID)
                     : (int64_t)get_i32le(hbuf + HDR_OFF_ID);

    if (hdr->length > cap) {
        LOG_ERR("recv: payload %u exceeds buffer %zu", hdr->length, cap);
        return -1;
    }

    if (hdr->length && recv_all(sock, buf, hdr->length) <= 0)
        return -1;

    *payload_len = hdr->length;
    return 0;
}
