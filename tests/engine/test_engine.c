/*
 * test_engine.c – Unit tests for generic engine subsystem.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <check.h>
#include <glib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "engine.h"
#include "qemu_engine.h"
#ifdef USBVCR_HAVE_LOCAL_ENGINE
#include "local_engine.h"
#endif
#include "protocol.h"
#include "transport.h"
#include "bytes.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

START_TEST(test_engine_create_and_destroy_default)
{
    usb_engine_t *engine = usb_engine_create(NULL, NULL);
    ck_assert_ptr_nonnull(engine);
    ck_assert_ptr_nonnull(engine->ops);
    ck_assert_str_eq(engine->ops->name, "qemu");
    usb_engine_destroy(engine);
}
END_TEST

START_TEST(test_engine_create_aliases)
{
    usb_engine_config_t cfg = {
        .host = "127.0.0.1",
        .port = "9999",
        .debug = TRUE,
    };

    usb_engine_t *e1 = usb_engine_create("qemu", &cfg);
    ck_assert_ptr_nonnull(e1);
    ck_assert_str_eq(e1->ops->name, "qemu");
    usb_engine_destroy(e1);

    usb_engine_t *e2 = usb_engine_create("usbredir", &cfg);
    ck_assert_ptr_nonnull(e2);
    ck_assert_str_eq(e2->ops->name, "qemu");
    usb_engine_destroy(e2);
}
END_TEST

START_TEST(test_engine_create_unknown)
{
    usb_engine_t *engine = usb_engine_create("unknown_target_engine", NULL);
    ck_assert_ptr_null(engine);
}
END_TEST

START_TEST(test_engine_null_handling)
{
    ck_assert_int_eq(usb_engine_run(NULL, NULL), -1);

    usb_engine_stop(NULL);    /* Should not crash */
    usb_engine_destroy(NULL); /* Should not crash */

    usb_engine_t *engine = usb_engine_create("qemu", NULL);
    ck_assert_ptr_nonnull(engine);
    ck_assert_int_eq(usb_engine_run(engine, NULL), -1);
    usb_engine_stop(engine);
    usb_engine_destroy(engine);
}
END_TEST

START_TEST(test_engine_init_fail)
{
    usb_engine_config_t cfg = {
        .port = "invalid_port_999999",
    };
    usb_engine_t *engine = usb_engine_create("qemu", &cfg);
    ck_assert_ptr_null(engine);
}
END_TEST

START_TEST(test_qemu_destroy_open_sock)
{
    usb_engine_t *engine = usb_engine_create("qemu", NULL);
    ck_assert_ptr_nonnull(engine);

    typedef struct {
        char *host;
        char *port;
        int sock;
    } dummy_qemu_priv_t;

    dummy_qemu_priv_t *priv = (dummy_qemu_priv_t *)engine->priv;
    priv->sock = dup(0);
    ck_assert_int_ge(priv->sock, 0);

    usb_engine_destroy(engine);
}
END_TEST

START_TEST(test_qemu_engine_conn_fail)
{
    /* Use an unreachable/closed port on loopback */
    usb_engine_config_t cfg = {
        .host = "127.0.0.1",
        .port = "1",
    };
    usb_engine_t *engine = usb_engine_create("qemu", &cfg);
    ck_assert_ptr_nonnull(engine);

    usb_device_t dummy_dev;
    memset(&dummy_dev, 0, sizeof(dummy_dev));

    int rc = usb_engine_run(engine, &dummy_dev);
    ck_assert_int_eq(rc, -1);

    usb_engine_destroy(engine);
}
END_TEST

START_TEST(test_qemu_engine_compat_helper)
{
    /* Verify backward-compatibility helper delegates to usb_core_run */
    int rc = qemu_engine_run(-1, NULL);
    ck_assert_int_eq(rc, -1);
}
END_TEST

static int
dummy_get_desc(usb_device_t *dev, uint8_t type, uint8_t index, uint16_t lang, const uint8_t **out,
               uint16_t *len)
{
    (void)dev;
    (void)type;
    (void)index;
    (void)lang;
    static const uint8_t dd[]
        = { 18, 0x01, 0x00, 0x02, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 1, 2, 0, 1 };
    static const uint8_t cd[] = { 9, 0x02, 9, 0, 1, 1, 0, 0x80, 50 };
    if (type == USB_DESC_DEVICE)
    {
        *out = dd;
        *len = sizeof(dd);
        return 0;
    }
    if (type == USB_DESC_CONFIG)
    {
        *out = cd;
        *len = sizeof(cd);
        return 0;
    }
    return -1;
}

#ifdef USBVCR_HAVE_LOCAL_ENGINE
START_TEST(test_local_engine_ops)
{
    usb_engine_config_t cfg = {
        .instance_name = "test_gadget",
        .udc = "dummy_udc",
    };
    usb_engine_t *engine = usb_engine_create("local", &cfg);
    ck_assert_ptr_nonnull(engine);
    ck_assert_str_eq(engine->ops->name, "local");

    /* Running with NULL dev should fail */
    ck_assert_int_eq(usb_engine_run(engine, NULL), -1);

    /* Running with dummy dev will fail setup gracefully */
    usb_device_ops_t dummy_ops;
    memset(&dummy_ops, 0, sizeof(dummy_ops));
    dummy_ops.get_descriptor = dummy_get_desc;

    usb_device_t dummy_dev;
    memset(&dummy_dev, 0, sizeof(dummy_dev));
    dummy_dev.ops = &dummy_ops;
    ck_assert_int_eq(usb_engine_run(engine, &dummy_dev), -1);

    usb_engine_stop(engine);
    usb_engine_destroy(engine);
}
END_TEST
#endif

/* ------------------------------------------------------------------ *
 * End-to-end QEMU engine test with a real loopback TCP server         *
 * ------------------------------------------------------------------ */

typedef struct {
    int server_sock;
    int port;
    pthread_t thread;
} mock_server_t;

static void *
mock_server_thread(void *arg)
{
    mock_server_t *srv = arg;
    int client_sock = accept(srv->server_sock, NULL, NULL);
    if (client_sock < 0)
        return NULL;

    /* Send server HELLO first: usb_core do_handshake() waits for peer hello before responding */
    usb_hello_header_t resp_hello;
    memset(&resp_hello, 0, sizeof(resp_hello));
    strncpy(resp_hello.version, "mock_server 1.0", sizeof(resp_hello.version) - 1);
    resp_hello.capabilities[0] = 0;

    usb_conn_send(client_sock, RT_HELLO, 0, &resp_hello, sizeof(resp_hello), NULL);

    /* Receive client HELLO and announcements */
    usb_header_t hdr;
    uint8_t payload[512];
    size_t payload_len = 0;
    (void)usb_conn_recv(client_sock, &hdr, payload, sizeof(payload), &payload_len, NULL);

    usleep(20000);
    close(client_sock);
    return NULL;
}

START_TEST(test_qemu_engine_e2e_loopback)
{
    mock_server_t srv;
    memset(&srv, 0, sizeof(srv));
    srv.server_sock = socket(AF_INET, SOCK_STREAM, 0);
    ck_assert_int_ge(srv.server_sock, 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* Ephemeral port */

    ck_assert_int_eq(bind(srv.server_sock, (struct sockaddr *)&addr, sizeof(addr)), 0);
    ck_assert_int_eq(listen(srv.server_sock, 1), 0);

    socklen_t addrlen = sizeof(addr);
    ck_assert_int_eq(getsockname(srv.server_sock, (struct sockaddr *)&addr, &addrlen), 0);
    srv.port = ntohs(addr.sin_port);

    pthread_create(&srv.thread, NULL, mock_server_thread, &srv);

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", srv.port);

    usb_engine_config_t cfg = {
        .host = "127.0.0.1",
        .port = port_str,
        .debug = FALSE,
    };

    usb_engine_t *engine = usb_engine_create("qemu", &cfg);
    ck_assert_ptr_nonnull(engine);

    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_get_desc;

    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    int rc = usb_engine_run(engine, &dev);
    ck_assert_int_eq(rc, 0);

    usb_engine_destroy(engine);
    pthread_join(srv.thread, NULL);
    close(srv.server_sock);
}
END_TEST

static Suite *
engine_suite(void)
{
    Suite *s = suite_create("engine");
    TCase *tc = tcase_create("core");

    tcase_add_test(tc, test_engine_create_and_destroy_default);
    tcase_add_test(tc, test_engine_create_aliases);
    tcase_add_test(tc, test_engine_create_unknown);
    tcase_add_test(tc, test_engine_null_handling);
    tcase_add_test(tc, test_engine_init_fail);
    tcase_add_test(tc, test_qemu_destroy_open_sock);
    tcase_add_test(tc, test_qemu_engine_conn_fail);
    tcase_add_test(tc, test_qemu_engine_compat_helper);
#ifdef USBVCR_HAVE_LOCAL_ENGINE
    tcase_add_test(tc, test_local_engine_ops);
#endif
    tcase_add_test(tc, test_qemu_engine_e2e_loopback);

    suite_add_tcase(s, tc);
    return s;
}

int
main(void)
{
    SRunner *sr = srunner_create(engine_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
