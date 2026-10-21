/* Unit tests for FunctionFS and ConfigFS helpers (src/lib/ffs/ffs.c). */

#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <glib.h>
#include <unistd.h>

#include "ffs/ffs.h"
#include "yaml_backend.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

START_TEST(test_ffs_ctx_lifecycle)
{
    ffs_ctx_t *ctx = ffs_ctx_new("test_gadget", "dummy_udc.0");
    ck_assert_ptr_nonnull(ctx);
    ck_assert_str_eq(ctx->name, "test_gadget");
    ck_assert_str_eq(ctx->udc, "dummy_udc.0");
    ck_assert_str_eq(ctx->configfs_path, "/sys/kernel/config/usb_gadget/test_gadget");
    ck_assert_str_eq(ctx->mount_path, "/dev/ffs-test_gadget");
    ck_assert_int_eq(ctx->ep0_fd, -1);
    ck_assert_ptr_nonnull(ctx->ep_fds);
    ck_assert_int_eq(ctx->ep_fds->len, 0);

    /* Test adding mock ep fds to ensure ffs_ctx_free closes/cleans them up */
    int fds[2];
    ck_assert_int_eq(pipe(fds), 0);
    g_array_append_val(ctx->ep_fds, fds[0]);
    g_array_append_val(ctx->ep_fds, fds[1]);

    ffs_ctx_free(ctx);

    /* Also verify NULL safety */
    ffs_ctx_free(NULL);
}
END_TEST

START_TEST(test_ffs_ctx_defaults)
{
    ffs_ctx_t *ctx = ffs_ctx_new(NULL, NULL);
    ck_assert_ptr_nonnull(ctx);
    ck_assert_str_eq(ctx->name, "usbvcr");
    ck_assert_ptr_null(ctx->udc);
    ck_assert_str_eq(ctx->configfs_path, "/sys/kernel/config/usb_gadget/usbvcr");
    ck_assert_str_eq(ctx->mount_path, "/dev/ffs-usbvcr");
    ffs_ctx_free(ctx);
}
END_TEST

START_TEST(test_ffs_build_descriptors_null)
{
    GByteArray *arr = ffs_build_descriptors(NULL);
    ck_assert_ptr_null(arr);

    usb_device_t dummy_dev;
    memset(&dummy_dev, 0, sizeof(dummy_dev));
    arr = ffs_build_descriptors(&dummy_dev);
    ck_assert_ptr_null(arr);
}
END_TEST

START_TEST(test_ffs_build_descriptors_valid)
{
    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    GByteArray *arr = ffs_build_descriptors(dev);
    ck_assert_ptr_nonnull(arr);

    /* Validate header */
    ck_assert_int_ge(arr->len, sizeof(struct usb_functionfs_descs_head_v2));
    struct usb_functionfs_descs_head_v2 *head = (struct usb_functionfs_descs_head_v2 *)arr->data;
    ck_assert_int_eq(head->magic, FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    ck_assert_int_eq(head->flags, FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    ck_assert_int_eq(head->length, arr->len);

    /* Next should be FS sub_count (uint32_t) */
    size_t off = sizeof(struct usb_functionfs_descs_head_v2);
    uint32_t fs_count = *(uint32_t *)(arr->data + off);
    ck_assert_int_gt(fs_count, 0);

    g_byte_array_free(arr, TRUE);
}
END_TEST

START_TEST(test_ffs_build_strings)
{
    GByteArray *arr = ffs_build_strings(NULL);
    ck_assert_ptr_nonnull(arr);

    ck_assert_int_ge(arr->len, sizeof(struct usb_functionfs_strings_head));
    struct usb_functionfs_strings_head *head = (struct usb_functionfs_strings_head *)arr->data;
    ck_assert_int_eq(head->magic, FUNCTIONFS_STRINGS_MAGIC);
    ck_assert_int_eq(head->length, arr->len);
    ck_assert_int_eq(head->str_count, 1);
    ck_assert_int_eq(head->lang_count, 1);

    size_t off = sizeof(struct usb_functionfs_strings_head);
    uint16_t lang = *(uint16_t *)(arr->data + off);
    ck_assert_int_eq(lang, 0x0409);
    off += sizeof(uint16_t);

    const char *str = (const char *)(arr->data + off);
    ck_assert_str_eq(str, "USB VCR Emulated Device");

    g_byte_array_free(arr, TRUE);
}
END_TEST

static int dummy_desc_short(usb_device_t *dev, uint8_t type, uint8_t index,
                             uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev;
    (void)type;
    (void)index;
    (void)lang;
    static const uint8_t short_desc[] = { 0x04, 0x02 }; /* too short (< 9 bytes) */
    *out = short_desc;
    *len = sizeof(short_desc);
    return 0;
}

START_TEST(test_ffs_build_descriptors_short_config)
{
    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_short;

    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    GByteArray *arr = ffs_build_descriptors(&dev);
    ck_assert_ptr_null(arr);
}
END_TEST

START_TEST(test_ffs_setup_and_teardown_mock)
{
    ffs_ctx_t *ctx = ffs_ctx_new("mock_gadget", "dummy_udc");
    ck_assert_ptr_nonnull(ctx);

    /* Point paths to a writable temporary directory to test directory creation and teardown */
    g_autofree char *tmp = g_dir_make_tmp("usbvcr_ffs_test_XXXXXX", NULL);
    ck_assert_ptr_nonnull(tmp);

    g_free(ctx->configfs_path);
    g_free(ctx->mount_path);
    ctx->configfs_path = g_strdup_printf("%s/configfs", tmp);
    ctx->mount_path = g_strdup_printf("%s/mount", tmp);

    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);

    /* ffs_setup creates ConfigFS dirs, then fails at mount() without root privileges */
    int ret = ffs_setup(ctx, dev);
    ck_assert_int_eq(ret, -1);

    /* Test ffs_teardown on this initialized mock directory structure */
    ffs_teardown(ctx);

    /* Cleanup temporary directory */
    rmdir(ctx->mount_path);
    rmdir(ctx->configfs_path);
    rmdir(tmp);
    ffs_ctx_free(ctx);
}
END_TEST

START_TEST(test_ffs_detect_udc)
{
    /* Should return a string or NULL without crashing */
    char *udc = ffs_detect_udc();
    if (udc != NULL)
    {
        ck_assert_int_gt(strlen(udc), 0);
        g_free(udc);
    }
}
END_TEST

START_TEST(test_ffs_teardown_null)
{
    /* Verify NULL safety */
    ffs_teardown(NULL);
}
END_TEST

static Suite *ffs_suite(void)
{
    Suite *s = suite_create("ffs");
    TCase *tc = tcase_create("core");

    tcase_add_test(tc, test_ffs_ctx_lifecycle);
    tcase_add_test(tc, test_ffs_ctx_defaults);
    tcase_add_test(tc, test_ffs_build_descriptors_null);
    tcase_add_test(tc, test_ffs_build_descriptors_short_config);
    tcase_add_test(tc, test_ffs_build_descriptors_valid);
    tcase_add_test(tc, test_ffs_build_strings);
    tcase_add_test(tc, test_ffs_setup_and_teardown_mock);
    tcase_add_test(tc, test_ffs_detect_udc);
    tcase_add_test(tc, test_ffs_teardown_null);

    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(ffs_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
