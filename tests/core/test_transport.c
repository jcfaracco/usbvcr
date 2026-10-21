/*
 * Unit tests for src/core/transport.c.
 *
 * Missing coverage (58%):
 *   connect_to_server()  – getaddrinfo failure, connect() refused       (lines 33-67)
 *   recv_all()           – peer EOF (n==0), mid-frame EAGAIN, hard err  (lines 91,95,97-98)
 *   usb_conn_send()      – payload too large, id64 path, send() failure (lines 133-134,141,149-150)
 *   usb_conn_recv()      – id64 parse path, payload > cap               (lines 173,177-178)
 *
 * All socket tests use socketpair(AF_UNIX, SOCK_STREAM) so they run
 * without root privileges or a real network.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <check.h>

#include "transport.h"
#include "protocol.h"

/* ── helpers ── */

/* Create a matched socketpair, store fds[0] (peer A) and fds[1] (peer B). */
static void make_pair(int fds[2])
{
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
}

/* Set SO_RCVTIMEO on `fd` to `ms` milliseconds. */
static void set_rcvtimeo(int fd, int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* Always reset the global id-width to 32-bit between tests. */
static void reset_id64(void)
{
    transport_set_id64(0);
}

/* ================================================================== *
 * connect_to_server                                                   *
 * ================================================================== */

/* getaddrinfo fails for a syntactically valid but non-existent TLD name. */
START_TEST(test_connect_bad_host)
{
    reset_id64();
    int fd = connect_to_server("this.host.does.not.exist.invalid", "9999");
    ck_assert_int_eq(fd, -1);
}
END_TEST

/* connect() is refused because nothing listens on that port.
 * We bind a TCP socket, record its port, then close it before connecting. */
START_TEST(test_connect_refused)
{
    reset_id64();

    /* Bind to port 0 on loopback (OS assigns an ephemeral port). */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    ck_assert_int_ge(srv, 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    ck_assert_int_eq(bind(srv, (struct sockaddr *)&addr, sizeof(addr)), 0);

    socklen_t alen = sizeof(addr);
    ck_assert_int_eq(getsockname(srv, (struct sockaddr *)&addr, &alen), 0);
    int port = ntohs(addr.sin_port);

    /* Close before any listener starts – connect() will get ECONNREFUSED. */
    close(srv);

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    int fd = connect_to_server("127.0.0.1", port_str);
    ck_assert_int_eq(fd, -1);
}
END_TEST

/* Happy path: connect to a real listener, verify we get a valid fd. */
START_TEST(test_connect_success)
{
    reset_id64();

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    ck_assert_int_ge(srv, 0);

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    ck_assert_int_eq(bind(srv, (struct sockaddr *)&addr, sizeof(addr)), 0);
    ck_assert_int_eq(listen(srv, 1), 0);

    socklen_t alen = sizeof(addr);
    ck_assert_int_eq(getsockname(srv, (struct sockaddr *)&addr, &alen), 0);
    int port = ntohs(addr.sin_port);

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int fd = connect_to_server("127.0.0.1", port_str);
    ck_assert_int_ge(fd, 0);

    /* Accept to prevent RST on close. */
    int cli = accept(srv, NULL, NULL);
    close(cli);
    close(fd);
    close(srv);
}
END_TEST

/* ================================================================== *
 * recv_all                                                            *
 * ================================================================== */

/* n == 0: peer closes the connection → recv_all returns -1. */
START_TEST(test_recv_all_peer_eof)
{
    reset_id64();
    int fds[2];
    make_pair(fds);

    /* Close the write end immediately. */
    close(fds[1]);

    uint8_t buf[4];
    int r = recv_all(fds[0], buf, sizeof(buf));
    ck_assert_int_eq(r, -1);

    close(fds[0]);
}
END_TEST

/* EAGAIN at frame boundary (total == 0): recv_all returns 0. */
START_TEST(test_recv_all_idle_timeout)
{
    reset_id64();
    int fds[2];
    make_pair(fds);

    /* Very short timeout so the test doesn't block for long. */
    set_rcvtimeo(fds[0], 50);

    uint8_t buf[4];
    int r = recv_all(fds[0], buf, sizeof(buf));
    /* With nothing written, first recv fires EAGAIN at total==0 → return 0. */
    ck_assert_int_eq(r, 0);

    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* Hard error: recv() on a completely closed fd → errno != EAGAIN → return -1. */
START_TEST(test_recv_all_hard_error)
{
    reset_id64();
    int fds[2];
    make_pair(fds);
    close(fds[0]);
    close(fds[1]);

    /* fds[0] is now invalid; recv() will fail with EBADF. */
    uint8_t buf[4];
    int r = recv_all(fds[0], buf, sizeof(buf));
    ck_assert_int_eq(r, -1);
}
END_TEST

/* ================================================================== *
 * usb_conn_send                                                       *
 * ================================================================== */

/* Payload too large: hs + len > USB_VCR_BUF_SIZE. */
START_TEST(test_send_payload_too_large)
{
    reset_id64();
    int fds[2];
    make_pair(fds);

    /* Any non-NULL payload pointer is fine; the guard fires before memcpy. */
    uint8_t dummy = 0;
    int r = usb_conn_send(fds[0], 0, 0, &dummy, USB_VCR_BUF_SIZE, NULL);
    ck_assert_int_eq(r, -1);

    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* id64 send path: 16-byte header is written; receiver parses it correctly. */
START_TEST(test_send_recv_id64)
{
    int fds[2];
    make_pair(fds);

    transport_set_id64(1);   /* switch to 64-bit id header */

    int64_t sent_id     = 0x0102030405060708LL;
    uint32_t sent_type  = 0xABCD1234u;
    uint8_t payload[4]  = { 0x11, 0x22, 0x33, 0x44 };

    int r = usb_conn_send(fds[0], sent_type, sent_id, payload, sizeof(payload), NULL);
    ck_assert_int_eq(r, 0);

    /* Read the raw bytes and verify the 16-byte header layout. */
    uint8_t frame[20];
    ssize_t n = recv(fds[1], frame, sizeof(frame), MSG_WAITALL);
    ck_assert_int_eq(n, 20);   /* 16-byte header + 4 payload */

    /* type @ offset 0 (LE u32) */
    uint32_t got_type;
    memcpy(&got_type, frame + 0, 4);
    ck_assert_uint_eq(got_type, sent_type);

    /* length @ offset 4 (LE u32) */
    uint32_t got_len;
    memcpy(&got_len, frame + 4, 4);
    ck_assert_uint_eq(got_len, 4);

    /* id @ offset 8 (LE i64) */
    int64_t got_id;
    memcpy(&got_id, frame + 8, 8);
    ck_assert_int_eq(got_id, sent_id);

    /* payload after header */
    ck_assert_mem_eq(frame + 16, payload, sizeof(payload));

    reset_id64();
    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* send() fails: closed fd → usb_conn_send returns -1. */
START_TEST(test_send_socket_error)
{
    signal(SIGPIPE, SIG_IGN);
    reset_id64();
    int fds[2];
    make_pair(fds);

    /* Shut down the write side. */
    shutdown(fds[0], SHUT_WR);

    uint8_t payload[4] = { 0 };
    int r = usb_conn_send(fds[0], 0, 0, payload, sizeof(payload), NULL);
    ck_assert_int_eq(r, -1);

    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* ================================================================== *
 * usb_conn_recv                                                       *
 * ================================================================== */

/* id64 recv path: engine parses the 64-bit id correctly. */
START_TEST(test_recv_id64)
{
    int fds[2];
    make_pair(fds);

    transport_set_id64(1);

    /* Build a raw 16-byte header + 2 payload bytes by hand. */
    uint8_t frame[18];
    memset(frame, 0, sizeof(frame));

    uint32_t type = 42;
    uint32_t plen = 2;
    int64_t  id   = -9000000000LL;

    memcpy(frame + 0, &type, 4);
    memcpy(frame + 4, &plen, 4);
    memcpy(frame + 8, &id,   8);
    frame[16] = 0xDE;
    frame[17] = 0xAD;

    ssize_t sent = send(fds[1], frame, sizeof(frame), 0);
    ck_assert_int_eq((int)sent, (int)sizeof(frame));

    usb_header_t hdr;
    uint8_t buf[32];
    size_t payload_len = 0;
    int r = usb_conn_recv(fds[0], &hdr, buf, sizeof(buf), &payload_len, NULL);
    ck_assert_int_eq(r, 0);
    ck_assert_uint_eq(hdr.type, 42);
    ck_assert_int_eq(hdr.id, id);
    ck_assert_uint_eq(payload_len, 2);
    ck_assert_uint_eq(buf[0], 0xDE);
    ck_assert_uint_eq(buf[1], 0xAD);

    reset_id64();
    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* Payload length in header exceeds buffer cap → usb_conn_recv returns -1. */
START_TEST(test_recv_payload_exceeds_cap)
{
    reset_id64();
    int fds[2];
    make_pair(fds);

    /* Build a 12-byte (id32) header claiming a 512-byte payload. */
    uint8_t frame[12];
    uint32_t type = 1;
    uint32_t plen = 512;    /* bigger than our cap below */
    int32_t  id   = 7;
    memcpy(frame + 0, &type, 4);
    memcpy(frame + 4, &plen, 4);
    memcpy(frame + 8, &id,   4);

    ssize_t sent = send(fds[1], frame, sizeof(frame), 0);
    ck_assert_int_eq((int)sent, 12);

    usb_header_t hdr;
    uint8_t tiny_buf[8];   /* cap = 8 < 512 → must fail */
    size_t payload_len = 0;
    int r = usb_conn_recv(fds[0], &hdr, tiny_buf, sizeof(tiny_buf), &payload_len, NULL);
    ck_assert_int_eq(r, -1);

    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* Round-trip: id32 send → recv, verify all fields. */
START_TEST(test_send_recv_id32_roundtrip)
{
    reset_id64();
    int fds[2];
    make_pair(fds);

    uint8_t payload[6] = { 1, 2, 3, 4, 5, 6 };
    int r = usb_conn_send(fds[0], 77, 99, payload, sizeof(payload), NULL);
    ck_assert_int_eq(r, 0);

    usb_header_t hdr;
    uint8_t buf[64];
    size_t plen = 0;
    r = usb_conn_recv(fds[1], &hdr, buf, sizeof(buf), &plen, NULL);
    ck_assert_int_eq(r, 0);
    ck_assert_uint_eq(hdr.type, 77);
    ck_assert_int_eq(hdr.id, 99);
    ck_assert_uint_eq(plen, 6);
    ck_assert_mem_eq(buf, payload, 6);

    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* usb_conn_recv: peer sends only the header then closes → payload recv fails. */
START_TEST(test_recv_peer_closes_mid_payload)
{
    reset_id64();
    int fds[2];
    make_pair(fds);

    /* Header claiming 4-byte payload. */
    uint8_t frame[12];
    uint32_t type = 1, plen = 4;
    int32_t  id   = 1;
    memcpy(frame + 0, &type, 4);
    memcpy(frame + 4, &plen, 4);
    memcpy(frame + 8, &id,   4);
    send(fds[1], frame, sizeof(frame), 0);

    /* Close before sending the payload → recv_all on payload returns -1. */
    close(fds[1]);

    usb_header_t hdr;
    uint8_t buf[32];
    size_t payload_len = 0;
    int r = usb_conn_recv(fds[0], &hdr, buf, sizeof(buf), &payload_len, NULL);
    ck_assert_int_eq(r, -1);

    close(fds[0]);
}
END_TEST

/* transport_set_id64: verify the flag round-trips. */
START_TEST(test_transport_set_id64_toggle)
{
    transport_set_id64(0);
    int fds[2];
    make_pair(fds);

    /* With id32, a header-only send produces 12 bytes. */
    int r = usb_conn_send(fds[0], 1, 0, NULL, 0, NULL);
    ck_assert_int_eq(r, 0);
    uint8_t buf32[16];
    ssize_t n = recv(fds[1], buf32, sizeof(buf32), MSG_DONTWAIT);
    ck_assert_int_eq((int)n, 12);

    /* With id64, the same call produces 16 bytes. */
    transport_set_id64(1);
    r = usb_conn_send(fds[0], 1, 0, NULL, 0, NULL);
    ck_assert_int_eq(r, 0);
    n = recv(fds[1], buf32, sizeof(buf32), MSG_DONTWAIT);
    ck_assert_int_eq((int)n, 16);

    reset_id64();
    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* ================================================================== */

static Suite *transport_suite(void)
{
    Suite *s = suite_create("transport");

    TCase *tc_conn = tcase_create("connect_to_server");
    /* Longer timeout for the DNS/network cases. */
    tcase_set_timeout(tc_conn, 10);
    tcase_add_test(tc_conn, test_connect_bad_host);
    tcase_add_test(tc_conn, test_connect_refused);
    tcase_add_test(tc_conn, test_connect_success);
    suite_add_tcase(s, tc_conn);

    TCase *tc_recv = tcase_create("recv_all");
    tcase_add_test(tc_recv, test_recv_all_peer_eof);
    tcase_add_test(tc_recv, test_recv_all_idle_timeout);
    tcase_add_test(tc_recv, test_recv_all_hard_error);
    suite_add_tcase(s, tc_recv);

    TCase *tc_send = tcase_create("usb_conn_send");
    tcase_add_test(tc_send, test_send_payload_too_large);
    tcase_add_test(tc_send, test_send_recv_id64);
    tcase_add_test(tc_send, test_send_socket_error);
    tcase_add_test(tc_send, test_transport_set_id64_toggle);
    suite_add_tcase(s, tc_send);

    TCase *tc_recvf = tcase_create("usb_conn_recv");
    tcase_add_test(tc_recvf, test_recv_id64);
    tcase_add_test(tc_recvf, test_recv_payload_exceeds_cap);
    tcase_add_test(tc_recvf, test_send_recv_id32_roundtrip);
    tcase_add_test(tc_recvf, test_recv_peer_closes_mid_payload);
    suite_add_tcase(s, tc_recvf);

    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(transport_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
