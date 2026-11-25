/* Unit tests for local_engine (src/engine/local_engine.c). */

#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <glib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "yaml_backend.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

/* Expose static functions for unit testing under __local_engine_test__ */
#define static
#include "../../src/engine/local_engine.c"
#undef static

static int dummy_get_desc_fail(usb_device_t *dev, uint8_t type, uint8_t index,
                                uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev;
    (void)type;
    (void)index;
    (void)lang;
    (void)out;
    (void)len;
    return -1;
}

START_TEST(test_open_endpoints_fail)
{
    ffs_ctx_t *ctx = ffs_ctx_new("test", NULL);

    usb_device_ops_t dummy_ops;
    memset(&dummy_ops, 0, sizeof(dummy_ops));
    dummy_ops.get_descriptor = dummy_get_desc_fail;

    usb_device_t dummy_dev;
    memset(&dummy_dev, 0, sizeof(dummy_dev));
    dummy_dev.ops = &dummy_ops;

    ck_assert_ptr_null(open_endpoints(ctx, &dummy_dev));

    ffs_ctx_free(ctx);
}
END_TEST

START_TEST(test_open_endpoints_and_close)
{
    /* Create a temporary mock mount directory with ep1 to cover lines 76-77 */
    g_autofree char *tmp = g_dir_make_tmp("usbvcr_mock_mount_XXXXXX", NULL);
    ck_assert_ptr_nonnull(tmp);

    g_autofree char *ep1_path = g_strdup_printf("%s/ep1", tmp);
    int mock_ep1_fd = open(ep1_path, O_RDWR | O_CREAT, 0600);
    ck_assert_int_ge(mock_ep1_fd, 0);
    close(mock_ep1_fd);

    ffs_ctx_t *ctx = ffs_ctx_new("test_ep", NULL);
    g_free(ctx->mount_path);
    ctx->mount_path = g_strdup(tmp);

    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    /* open_endpoints finds ep1 in the mock mount directory and opens it successfully */
    GArray *eps = open_endpoints(ctx, dev);
    ck_assert_ptr_nonnull(eps);
    ck_assert_int_gt(eps->len, 0);

    close_endpoints(eps);
    close_endpoints(NULL);

    unlink(ep1_path);
    rmdir(tmp);
    ffs_ctx_free(ctx);
    yaml_backend_destroy(dev);
}
END_TEST

START_TEST(test_handle_control_event_in)
{
    ffs_ctx_t *ctx = ffs_ctx_new("test_ctrl", NULL);
    int pfd[2];
    ck_assert_int_eq(pipe(pfd), 0);
    ctx->ep0_fd = pfd[1]; /* write end */

    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    /* Request device descriptor via control request */
    struct usb_ctrlrequest req;
    memset(&req, 0, sizeof(req));
    req.bRequestType = 0x80; /* IN, standard, device */
    req.bRequest = 0x06;     /* GET_DESCRIPTOR */
    req.wValue = 0x0100;     /* DEVICE descriptor */
    req.wIndex = 0;
    req.wLength = 18;

    handle_control_event(ctx, dev, &req);

    /* Read back response written to ep0 */
    uint8_t buf[18];
    ssize_t r = read(pfd[0], buf, sizeof(buf));
    ck_assert_int_eq(r, 18);
    ck_assert_int_eq(buf[0], 18);
    ck_assert_int_eq(buf[1], 0x01); /* USB_DESC_DEVICE */

    close(pfd[0]);
    ffs_ctx_free(ctx);
    yaml_backend_destroy(dev);
}
END_TEST

START_TEST(test_handle_control_event_out)
{
    ffs_ctx_t *ctx = ffs_ctx_new("test_ctrl_out", NULL);
    int pfd[2];
    ck_assert_int_eq(pipe(pfd), 0);
    ctx->ep0_fd = pfd[1];

    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    /* Standard SET_CONFIGURATION (OUT request, returns 0 bytes status ACK) */
    struct usb_ctrlrequest req;
    memset(&req, 0, sizeof(req));
    req.bRequestType = 0x00; /* OUT */
    req.bRequest = 0x09;     /* SET_CONFIGURATION */
    req.wValue = 1;
    req.wIndex = 0;
    req.wLength = 0;

    handle_control_event(ctx, dev, &req);

    close(pfd[0]);
    ffs_ctx_free(ctx);
    yaml_backend_destroy(dev);
}
END_TEST

START_TEST(test_handle_control_event_stall)
{
    ffs_ctx_t *ctx = ffs_ctx_new("test_ctrl_stall", NULL);
    int sv[2];
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    ctx->ep0_fd = sv[0];

    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    /* Invalid request that dev->ops->control fails on -> triggers Stall IN */
    struct usb_ctrlrequest req;
    memset(&req, 0, sizeof(req));
    req.bRequestType = 0x80;
    req.bRequest = 0xFF; /* Unknown request */
    req.wValue = 0;
    req.wIndex = 0;
    req.wLength = 4;

    handle_control_event(ctx, dev, &req);

    /* Also test OUT failure stall */
    req.bRequestType = 0x00;
    handle_control_event(ctx, dev, &req);

    close(sv[1]);
    ffs_ctx_free(ctx);
    yaml_backend_destroy(dev);
}
END_TEST

START_TEST(test_handle_ep0_events)
{
    ffs_ctx_t *ctx = ffs_ctx_new("test_ep0", NULL);
    int pfd[2];
    ck_assert_int_eq(pipe(pfd), 0);
    ctx->ep0_fd = pfd[0]; /* read end */

    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    /* Test short read */
    uint8_t dummy = 0;
    ck_assert_int_eq(write(pfd[1], &dummy, 1), 1);
    handle_ep0(ctx, dev);

    /* Test all event types: SETUP, ENABLE, DISABLE, BIND, UNBIND, and default */
    struct usb_functionfs_event ev;
    enum usb_functionfs_event_type types[] = {
        FUNCTIONFS_SETUP,
        FUNCTIONFS_ENABLE,
        FUNCTIONFS_DISABLE,
        FUNCTIONFS_BIND,
        FUNCTIONFS_UNBIND,
        FUNCTIONFS_SUSPEND,
    };

    for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++)
    {
        memset(&ev, 0, sizeof(ev));
        ev.type = (uint8_t)types[i];
        ck_assert_int_eq(write(pfd[1], &ev, sizeof(ev)), (ssize_t)sizeof(ev));
        handle_ep0(ctx, dev);
    }

    close(pfd[1]);
    ffs_ctx_free(ctx);
    yaml_backend_destroy(dev);
}
END_TEST

START_TEST(test_service_endpoints)
{
    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    /* NULL check */
    service_endpoints(dev, NULL);

    GArray *eps = g_array_new(FALSE, FALSE, sizeof(local_ep_t));

    /* IN endpoint with connected pipe: prime some device input data */
    dev->ops->control(dev, &(usb_control_packet_header_t){
        .requesttype = 0x00,
        .request = 0x09, /* SET_CONFIGURATION */
        .value = 1,
    }, NULL, &(uint16_t){0});

    int in_pfd[2];
    ck_assert_int_eq(pipe(in_pfd), 0);
    local_ep_t ep_in = {
        .ep_addr = 0x81,
        .fd = in_pfd[1],
        .is_in = TRUE,
    };
    g_array_append_val(eps, ep_in);

    /* OUT endpoint with connected pipe */
    int out_pfd[2];
    ck_assert_int_eq(pipe(out_pfd), 0);
    local_ep_t ep_out = {
        .ep_addr = 0x02,
        .fd = out_pfd[0],
        .is_in = FALSE,
    };
    g_array_append_val(eps, ep_out);

    /* Invalid fd endpoint */
    local_ep_t ep_invalid = {
        .ep_addr = 0x83,
        .fd = -1,
        .is_in = TRUE,
    };
    g_array_append_val(eps, ep_invalid);

    /* Write some dummy data for OUT endpoint to read */
    const char out_data[] = "ping";
    ck_assert_int_eq(write(out_pfd[1], out_data, sizeof(out_data)), (ssize_t)sizeof(out_data));

    /* Service endpoints */
    service_endpoints(dev, eps);

    close(in_pfd[0]);
    close(in_pfd[1]);
    close(out_pfd[0]);
    close(out_pfd[1]);
    g_array_free(eps, TRUE);
    yaml_backend_destroy(dev);
}
END_TEST

START_TEST(test_on_signal)
{
    g_stop = 0;
    on_signal(SIGINT);
    ck_assert_int_eq(g_stop, 1);
    g_stop = 0;
}
END_TEST

START_TEST(test_local_engine_run_setup_fail)
{
    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    /* Running with an invalid name or without root FunctionFS mounts will fail at ffs_setup */
    int ret = local_engine_run(dev, "nonexistent_dev_path_xyz", NULL);
    ck_assert_int_eq(ret, -1);
    yaml_backend_destroy(dev);
}
END_TEST

static Suite *local_engine_suite(void)
{
    Suite *s = suite_create("local_engine");
    TCase *tc = tcase_create("core");

    tcase_add_test(tc, test_open_endpoints_fail);
    tcase_add_test(tc, test_open_endpoints_and_close);
    tcase_add_test(tc, test_handle_control_event_in);
    tcase_add_test(tc, test_handle_control_event_out);
    tcase_add_test(tc, test_handle_control_event_stall);
    tcase_add_test(tc, test_handle_ep0_events);
    tcase_add_test(tc, test_service_endpoints);
    tcase_add_test(tc, test_on_signal);
    tcase_add_test(tc, test_local_engine_run_setup_fail);

    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(local_engine_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
