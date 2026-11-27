/* Unit tests for local_engine (src/engine/local_engine.c). */

#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <glib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "yaml_backend.h"
#include "ffs/ffs.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

/* Forward declarations for mocking ffs_setup/ffs_teardown in local_engine_run */
static int mock_ffs_setup(ffs_ctx_t *ctx, usb_device_t *dev);
static void mock_ffs_teardown(ffs_ctx_t *ctx);

/* Expose static functions for unit testing under __local_engine_test__ */
#define ffs_setup mock_ffs_setup
#define ffs_teardown mock_ffs_teardown
#define static
#include "../../src/engine/local_engine.c"
#undef static
#undef ffs_setup
#undef ffs_teardown

static int s_mock_setup_succeed = 0;
static int s_mock_ep0_pipe[2] = { -1, -1 };
static char *s_mock_mount_dir = NULL;

static int
mock_ffs_setup(ffs_ctx_t *ctx, usb_device_t *dev)
{
    if (!s_mock_setup_succeed)
        return ffs_setup(ctx, dev);

    s_mock_mount_dir = g_dir_make_tmp("mock_local_engine_XXXXXX", NULL);
    g_free(ctx->mount_path);
    ctx->mount_path = g_strdup(s_mock_mount_dir);

    char *ep1 = g_strdup_printf("%s/ep1", s_mock_mount_dir);
    int fd = open(ep1, O_RDWR | O_CREAT, 0600);
    if (fd >= 0)
        close(fd);
    g_free(ep1);

    if (pipe(s_mock_ep0_pipe) == 0)
    {
        ctx->ep0_fd = s_mock_ep0_pipe[0];
        /* Write an event so poll sees activity */
        struct usb_functionfs_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = FUNCTIONFS_ENABLE;
        (void)write(s_mock_ep0_pipe[1], &ev, sizeof(ev));
    }

    return 0;
}

static void
mock_ffs_teardown(ffs_ctx_t *ctx)
{
    if (!s_mock_setup_succeed)
    {
        ffs_teardown(ctx);
        return;
    }

    if (s_mock_ep0_pipe[1] >= 0)
    {
        close(s_mock_ep0_pipe[1]);
        s_mock_ep0_pipe[1] = -1;
    }
    if (s_mock_mount_dir != NULL)
    {
        char *ep1 = g_strdup_printf("%s/ep1", s_mock_mount_dir);
        unlink(ep1);
        g_free(ep1);
        rmdir(s_mock_mount_dir);
        g_free(s_mock_mount_dir);
        s_mock_mount_dir = NULL;
    }
}

static int
dummy_get_desc_fail(usb_device_t *dev, uint8_t type, uint8_t index, uint16_t lang,
                    const uint8_t **out, uint16_t *len)
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
    /* Create a temporary mock mount directory with ep1 */
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

START_TEST(test_open_endpoints_open_fail)
{
    /* Mount directory exists, but ep1 does NOT exist -> triggers open failure warning */
    g_autofree char *tmp = g_dir_make_tmp("usbvcr_mock_empty_XXXXXX", NULL);
    ck_assert_ptr_nonnull(tmp);

    ffs_ctx_t *ctx = ffs_ctx_new("test_ep_fail", NULL);
    g_free(ctx->mount_path);
    ctx->mount_path = g_strdup(tmp);

    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    GArray *eps = open_endpoints(ctx, dev);
    ck_assert_ptr_nonnull(eps);
    ck_assert_int_eq(eps->len, 0);

    close_endpoints(eps);
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

START_TEST(test_handle_control_event_io_failures)
{
    ffs_ctx_t *ctx = ffs_ctx_new("test_ctrl_io_fail", NULL);
    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    /* Use invalid fd (-1) to exercise write/read failure branches */
    ctx->ep0_fd = -1;

    /* 1. Successful IN control request: write fails */
    struct usb_ctrlrequest req_in;
    memset(&req_in, 0, sizeof(req_in));
    req_in.bRequestType = 0x80;
    req_in.bRequest = 0x06;
    req_in.wValue = 0x0100;
    req_in.wLength = 18;
    handle_control_event(ctx, dev, &req_in);

    /* 2. Successful OUT control request: status ACK write fails */
    struct usb_ctrlrequest req_out;
    memset(&req_out, 0, sizeof(req_out));
    req_out.bRequestType = 0x00;
    req_out.bRequest = 0x09;
    req_out.wValue = 1;
    handle_control_event(ctx, dev, &req_out);

    /* 3. Stall IN request: read fails */
    struct usb_ctrlrequest req_stall_in;
    memset(&req_stall_in, 0, sizeof(req_stall_in));
    req_stall_in.bRequestType = 0x80;
    req_stall_in.bRequest = 0xFF;
    req_stall_in.wLength = 4;
    handle_control_event(ctx, dev, &req_stall_in);

    /* 4. Stall OUT request: write fails */
    struct usb_ctrlrequest req_stall_out;
    memset(&req_stall_out, 0, sizeof(req_stall_out));
    req_stall_out.bRequestType = 0x00;
    req_stall_out.bRequest = 0xFF;
    handle_control_event(ctx, dev, &req_stall_out);

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
        FUNCTIONFS_SETUP, FUNCTIONFS_ENABLE, FUNCTIONFS_DISABLE,
        FUNCTIONFS_BIND,  FUNCTIONFS_UNBIND, FUNCTIONFS_SUSPEND,
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
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
    dev->ops->control(dev,
                      &(usb_control_packet_header_t){
                          .requesttype = 0x00,
                          .request = 0x09, /* SET_CONFIGURATION */
                          .value = 1,
                      },
                      NULL, &(uint16_t){ 0 });

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

START_TEST(test_service_endpoints_in_write_fail)
{
    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    /* Prime SET_CONFIGURATION so ep_in produces data */
    dev->ops->control(dev,
                      &(usb_control_packet_header_t){
                          .requesttype = 0x00,
                          .request = 0x09,
                          .value = 1,
                      },
                      NULL, &(uint16_t){ 0 });

    int pfd[2];
    ck_assert_int_eq(pipe(pfd), 0);

    GArray *eps = g_array_new(FALSE, FALSE, sizeof(local_ep_t));
    /* Read-only fd causes write() failure on IN endpoint -> covers line 228 */
    local_ep_t ep_in = {
        .ep_addr = 0x81,
        .fd = pfd[0],
        .is_in = TRUE,
    };
    g_array_append_val(eps, ep_in);

    service_endpoints(dev, eps);

    close(pfd[0]);
    close(pfd[1]);
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

    /* Running without mock succeeds fail at ffs_setup */
    s_mock_setup_succeed = 0;
    int ret = local_engine_run(dev, "nonexistent_dev_path_xyz", NULL);
    ck_assert_int_eq(ret, -1);
    yaml_backend_destroy(dev);
}
END_TEST

static void
mock_tick_stop(usb_device_t *dev)
{
    (void)dev;
    g_stop = 1;
}

START_TEST(test_local_engine_run_success_loop)
{
    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    /* Use a copy of ops so we do not mutate .rodata */
    const usb_device_ops_t *orig_ops = dev->ops;
    usb_device_ops_t custom_ops = *orig_ops;
    custom_ops.tick = mock_tick_stop;
    dev->ops = &custom_ops;

    s_mock_setup_succeed = 1;
    g_stop = 0;

    int ret = local_engine_run(dev, "mock_gadget_success", "dummy_udc");
    ck_assert_int_eq(ret, 0);

    s_mock_setup_succeed = 0;
    dev->ops = orig_ops;
    yaml_backend_destroy(dev);
}
END_TEST

START_TEST(test_local_engine_ops_full)
{
    const usb_engine_ops_t *ops = local_engine_get_ops();
    ck_assert_ptr_nonnull(ops);
    ck_assert_str_eq(ops->name, "local");
    ck_assert_ptr_nonnull(ops->description);

    usb_engine_t eng;
    memset(&eng, 0, sizeof(eng));
    eng.ops = ops;

    usb_engine_config_t cfg = {
        .instance_name = "test_gadget_ops",
        .udc = "test_udc",
    };

    ck_assert_int_eq(ops->init(&eng, &cfg), 0);
    ck_assert_ptr_nonnull(eng.priv);

    /* Test stop hook */
    g_stop = 0;
    ops->stop(&eng);
    ck_assert_int_eq(g_stop, 1);
    g_stop = 0;

    /* Test destroy */
    ops->destroy(&eng);
    ck_assert_ptr_null(eng.priv);

    /* Safe handling of NULL */
    ops->destroy(NULL);
    ops->stop(NULL);

    /* Init with NULL config */
    memset(&eng, 0, sizeof(eng));
    eng.ops = ops;
    ck_assert_int_eq(ops->init(&eng, NULL), 0);
    ops->destroy(&eng);
}
END_TEST

static Suite *
local_engine_suite(void)
{
    Suite *s = suite_create("local_engine");
    TCase *tc = tcase_create("core");

    tcase_add_test(tc, test_open_endpoints_fail);
    tcase_add_test(tc, test_open_endpoints_and_close);
    tcase_add_test(tc, test_open_endpoints_open_fail);
    tcase_add_test(tc, test_handle_control_event_in);
    tcase_add_test(tc, test_handle_control_event_out);
    tcase_add_test(tc, test_handle_control_event_stall);
    tcase_add_test(tc, test_handle_control_event_io_failures);
    tcase_add_test(tc, test_handle_ep0_events);
    tcase_add_test(tc, test_service_endpoints);
    tcase_add_test(tc, test_service_endpoints_in_write_fail);
    tcase_add_test(tc, test_on_signal);
    tcase_add_test(tc, test_local_engine_run_setup_fail);
    tcase_add_test(tc, test_local_engine_run_success_loop);
    tcase_add_test(tc, test_local_engine_ops_full);

    suite_add_tcase(s, tc);
    return s;
}

int
main(void)
{
    SRunner *sr = srunner_create(local_engine_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
