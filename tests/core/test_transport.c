/* Unit tests for src/core/transport.c: the socket helpers and the usb wire
 * frame layer (usb_conn_send / usb_conn_recv / recv_all / connect_to_server).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <check.h>

#include "transport.h"
#include "protocol.h"
#include "bytes.h"

/* ------------------------------------------------------------------ *
 * connect_to_server                                                    *
 * ------------------------------------------------------------------ */

/* An unresolvable host makes getaddrinfo() fail. */
START_TEST(test_connect_bad_host)
{
    ck_assert_int_lt(connect_to_server("this.host.does.not.exist.invalid", "80"), 0);
}
END_TEST

/* A reachable host with nobody listening makes connect() fail (ECONNREFUSED). */
START_TEST(test_connect_refused)
{
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    ck_assert_int_ge(probe, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET };
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ck_assert_int_eq(bind(probe, (struct sockaddr *)&addr, sizeof(addr)), 0);
    socklen_t alen = sizeof(addr);
    ck_assert_int_eq(getsockname(probe, (struct sockaddr *)&addr, &alen), 0);
    int port = ntohs(addr.sin_port);
    close(probe);   /* nobody listens on this port now */

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    ck_assert_int_lt(connect_to_server("127.0.0.1", portstr), 0);
}
END_TEST

typedef struct {
    int listen_fd;
    int accepted_fd;
} loopback_server;

static void *loopback_accept_thread(void *arg)
{
    loopback_server *srv = arg;
    srv->accepted_fd = accept(srv->listen_fd, NULL, NULL);
    return NULL;
}

/* A real loopback listener: connect_to_server() resolves and connects. */
START_TEST(test_connect_success)
{
    loopback_server srv = { .accepted_fd = -1 };
    srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    ck_assert_int_ge(srv.listen_fd, 0);

    struct sockaddr_in addr = { .sin_family = AF_INET };
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ck_assert_int_eq(bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
    ck_assert_int_eq(listen(srv.listen_fd, 1), 0);

    socklen_t alen = sizeof(addr);
    ck_assert_int_eq(getsockname(srv.listen_fd, (struct sockaddr *)&addr, &alen), 0);
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", ntohs(addr.sin_port));

    pthread_t th;
    ck_assert_int_eq(pthread_create(&th, NULL, loopback_accept_thread, &srv), 0);

    int fd = connect_to_server("127.0.0.1", portstr);
    ck_assert_int_ge(fd, 0);

    pthread_join(th, NULL);
    ck_assert_int_ge(srv.accepted_fd, 0);

    close(fd);
    close(srv.accepted_fd);
    close(srv.listen_fd);
}
END_TEST

/* ------------------------------------------------------------------ *
 * recv_all                                                             *
 * ------------------------------------------------------------------ */

/* A hard recv() error (bad fd) is reported as -1, not retried. */
START_TEST(test_recv_all_bad_fd)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ck_assert_int_ge(fd, 0);
    close(fd);     /* now invalid */

    uint8_t buf[4];
    ck_assert_int_eq(recv_all(fd, buf, sizeof(buf)), -1);
}
END_TEST

static void *delayed_sender(void *arg)
{
    int fd = *(int *)arg;
    const uint8_t part1[2] = { 0xAA, 0xBB };
    const uint8_t part2[2] = { 0xCC, 0xDD };

    ck_assert_int_eq((int)send(fd, part1, sizeof(part1), 0), (int)sizeof(part1));
    usleep(250 * 1000);   /* longer than the peer's recv timeout below */
    ck_assert_int_eq((int)send(fd, part2, sizeof(part2), 0), (int)sizeof(part2));
    return NULL;
}

/* A recv timeout that fires mid-frame (some bytes already read) is not
 * fatal: recv_all keeps waiting for the rest of the frame. */
START_TEST(test_recv_all_partial_then_timeout)
{
    int fds[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    struct timeval tv = { 0, 50 * 1000 };   /* 50 ms, shorter than the delay above */
    setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    pthread_t th;
    ck_assert_int_eq(pthread_create(&th, NULL, delayed_sender, &fds[1]), 0);

    uint8_t buf[4];
    ck_assert_int_eq(recv_all(fds[0], buf, sizeof(buf)), 4);
    ck_assert_uint_eq(buf[0], 0xAA);
    ck_assert_uint_eq(buf[3], 0xDD);

    pthread_join(th, NULL);
    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* ------------------------------------------------------------------ *
 * usb_conn_send / usb_conn_recv                                       *
 * ------------------------------------------------------------------ */

/* A payload that doesn't fit the frame buffer alongside the header is
 * rejected before any I/O happens. */
START_TEST(test_usb_conn_send_payload_too_large)
{
    int fds[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    ck_assert_int_eq(usb_conn_send(fds[0], RT_CONTROL_PACKET, 1,
                                   NULL, USB_VCR_BUF_SIZE, NULL), -1);

    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* send() failing (peer gone) is surfaced as -1. SIGPIPE is ignored so the
 * test process doesn't get killed by the write to a broken pipe. */
START_TEST(test_usb_conn_send_error)
{
    signal(SIGPIPE, SIG_IGN);

    int fds[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    close(fds[1]);   /* peer gone */

    uint8_t payload[4] = { 1, 2, 3, 4 };
    ck_assert_int_eq(usb_conn_send(fds[0], RT_CONTROL_PACKET, 1, payload, sizeof(payload), NULL), -1);

    close(fds[0]);
}
END_TEST

/* With 64-bit ids enabled, the header widens to 16 bytes and the id round
 * trips through the 64-bit field on both send and recv. */
START_TEST(test_usb_conn_send_recv_id64_roundtrip)
{
    int fds[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    transport_set_id64(1);

    int64_t big_id = 0x1122334455667788LL;
    uint8_t payload[3] = { 'x', 'y', 'z' };
    ck_assert_int_eq(usb_conn_send(fds[0], RT_BULK_PACKET, big_id, payload, sizeof(payload), NULL), 0);

    usb_header_t hdr;
    uint8_t buf[64];
    size_t len;
    ck_assert_int_eq(usb_conn_recv(fds[1], &hdr, buf, sizeof(buf), &len, NULL), 0);
    ck_assert_uint_eq(hdr.type, RT_BULK_PACKET);
    ck_assert_int_eq(hdr.id, big_id);
    ck_assert_uint_eq(len, sizeof(payload));
    ck_assert_mem_eq(buf, payload, sizeof(payload));

    transport_set_id64(0);
    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* A header claiming a payload larger than the caller's buffer is rejected
 * without attempting to read the (nonexistent) payload. */
START_TEST(test_usb_conn_recv_length_exceeds_cap)
{
    int fds[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    uint8_t raw[12];
    put_u32le(raw + 0, RT_BULK_PACKET);
    put_u32le(raw + 4, 999999u);   /* bogus oversized length */
    put_i32le(raw + 8, 1);
    ck_assert_int_eq((int)send(fds[0], raw, sizeof(raw), 0), (int)sizeof(raw));

    usb_header_t hdr;
    uint8_t buf[8];
    size_t len;
    ck_assert_int_eq(usb_conn_recv(fds[1], &hdr, buf, sizeof(buf), &len, NULL), -1);

    close(fds[0]);
    close(fds[1]);
}
END_TEST

/* ------------------------------------------------------------------ */

static Suite *transport_suite(void)
{
    Suite *s = suite_create("transport");

    TCase *tc_connect = tcase_create("connect_to_server");
    tcase_add_test(tc_connect, test_connect_bad_host);
    tcase_add_test(tc_connect, test_connect_refused);
    tcase_add_test(tc_connect, test_connect_success);
    suite_add_tcase(s, tc_connect);

    TCase *tc_recv_all = tcase_create("recv_all");
    tcase_add_test(tc_recv_all, test_recv_all_bad_fd);
    tcase_add_test(tc_recv_all, test_recv_all_partial_then_timeout);
    suite_add_tcase(s, tc_recv_all);

    TCase *tc_frame = tcase_create("usb_conn_frame");
    tcase_add_test(tc_frame, test_usb_conn_send_payload_too_large);
    tcase_add_test(tc_frame, test_usb_conn_send_error);
    tcase_add_test(tc_frame, test_usb_conn_send_recv_id64_roundtrip);
    tcase_add_test(tc_frame, test_usb_conn_recv_length_exceeds_cap);
    suite_add_tcase(s, tc_frame);

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
