/* Unit tests for FunctionFS and ConfigFS helpers (src/lib/ffs/ffs.c). */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <check.h>
#include <glib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include "ffs/ffs.h"
#include "yaml_backend.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

/* Real libc functions for linker wrapping */
int __real_mount(const char *source, const char *target, const char *filesystemtype, unsigned long mountflags, const void *data);
int __real_umount(const char *target);
DIR *__real_opendir(const char *name);

static int s_mock_mount_enabled = 0;
static int s_mock_mount_ret = 0;
static int s_mock_mount_errno = 0;

static int s_mock_umount_enabled = 0;
static int s_mock_umount_ret = 0;
static int s_mock_umount_errno = 0;

static const char *s_mock_udc_dir = NULL;

int __wrap_mount(const char *source, const char *target, const char *filesystemtype, unsigned long mountflags, const void *data)
{
    if (s_mock_mount_enabled)
    {
        if (s_mock_mount_ret < 0)
        {
            errno = s_mock_mount_errno;
            return -1;
        }
        return 0;
    }
    return __real_mount(source, target, filesystemtype, mountflags, data);
}

int __wrap_umount(const char *target)
{
    if (s_mock_umount_enabled)
    {
        if (s_mock_umount_ret < 0)
        {
            errno = s_mock_umount_errno;
            return -1;
        }
        return 0;
    }
    return __real_umount(target);
}

DIR *__wrap_opendir(const char *name)
{
    if (s_mock_udc_dir != NULL && strcmp(name, "/sys/class/udc") == 0)
    {
        return __real_opendir(s_mock_udc_dir);
    }
    return __real_opendir(name);
}

static void rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
    {
        unlink(path);
        rmdir(path);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char *sub = g_strdup_printf("%s/%s", path, ent->d_name);
        struct stat st;
        if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode))
            rm_rf(sub);
        else
            unlink(sub);
        g_free(sub);
    }
    closedir(d);
    rmdir(path);
}

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
    int neg_fd = -1;
    g_array_append_val(ctx->ep_fds, neg_fd);

    ffs_ctx_free(ctx);

    /* Test ctx with NULL ep_fds */
    ffs_ctx_t *ctx_no_ep = ffs_ctx_new("no_ep", NULL);
    g_array_free(ctx_no_ep->ep_fds, TRUE);
    ctx_no_ep->ep_fds = NULL;
    ffs_ctx_free(ctx_no_ep);

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

    usb_device_ops_t dummy_ops;
    memset(&dummy_ops, 0, sizeof(dummy_ops));
    dummy_dev.ops = &dummy_ops;
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
    yaml_backend_destroy(dev);
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

    /* Test with dev having ops but NULL get_descriptor */
    usb_device_ops_t dummy_ops;
    memset(&dummy_ops, 0, sizeof(dummy_ops));
    usb_device_t dummy_dev;
    memset(&dummy_dev, 0, sizeof(dummy_dev));
    dummy_dev.ops = &dummy_ops;
    arr = ffs_build_strings(&dummy_dev);
    ck_assert_ptr_nonnull(arr);
    g_byte_array_free(arr, TRUE);
}
END_TEST

static int dummy_desc_with_strings(usb_device_t *dev, uint8_t type, uint8_t index,
                                   uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)lang;
    static const uint8_t dd[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
        0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x01, 0x02,
        0x00, 0x01
    };
    static const uint8_t prod_desc[] = {
        20, 3,
        'M', 0, 'y', 0, ' ', 0, 'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0
    };

    if (type == 1) {
        *out = dd;
        *len = sizeof(dd);
        return 0;
    }
    if (type == 3 && index == 2) {
        *out = prod_desc;
        *len = sizeof(prod_desc);
        return 0;
    }
    return -1;
}

START_TEST(test_ffs_build_strings_custom_device)
{
    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_with_strings;

    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    GByteArray *arr = ffs_build_strings(&dev);
    ck_assert_ptr_nonnull(arr);

    size_t off = sizeof(struct usb_functionfs_strings_head) + sizeof(uint16_t);
    const char *str = (const char *)(arr->data + off);
    ck_assert_str_eq(str, "My Device");

    g_byte_array_free(arr, TRUE);
}
END_TEST

static int dummy_desc_invalid_utf16(usb_device_t *dev, uint8_t type, uint8_t index,
                                    uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)lang;
    static const uint8_t dd[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
        0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x01, 0x02,
        0x00, 0x01
    };
    /* Invalid UTF-16: surrogate pair code unit 0xd800 without low surrogate */
    static const uint8_t bad_utf16_desc[] = {
        4, 3, 0x00, 0xd8
    };

    if (type == 1) {
        *out = dd;
        *len = sizeof(dd);
        return 0;
    }
    if (type == 3 && index == 2) {
        *out = bad_utf16_desc;
        *len = sizeof(bad_utf16_desc);
        return 0;
    }
    return -1;
}

START_TEST(test_ffs_build_strings_invalid_utf16)
{
    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_invalid_utf16;

    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    /* extract_string_from_desc should hit lines 159-161 (g_error_free) and return NULL,
     * causing ffs_build_strings to use the default fallback string. */
    GByteArray *arr = ffs_build_strings(&dev);
    ck_assert_ptr_nonnull(arr);

    size_t off = sizeof(struct usb_functionfs_strings_head) + sizeof(uint16_t);
    const char *str = (const char *)(arr->data + off);
    ck_assert_str_eq(str, "USB VCR Emulated Device");

    g_byte_array_free(arr, TRUE);
}
END_TEST

static int dummy_desc_raw_and_empty_strings(usb_device_t *dev, uint8_t type, uint8_t index,
                                            uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)lang;
    static const uint8_t dd[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
        0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x01, 0x02,
        0x00, 0x01
    };
    /* Raw ASCII string (desc[1] != USB_DESC_STRING) */
    static const uint8_t raw_str_desc[] = "Raw String";
    /* String descriptor with length <= 2 */
    static const uint8_t short_str_desc[] = { 2, 3 };

    if (type == 1) {
        *out = dd;
        *len = sizeof(dd);
        return 0;
    }
    if (type == 3 && index == 2) {
        *out = raw_str_desc;
        *len = sizeof(raw_str_desc) - 1;
        return 0;
    }
    if (type == 3 && index == 3) {
        *out = short_str_desc;
        *len = sizeof(short_str_desc);
        return 0;
    }
    return -1;
}

static int dummy_desc_null_string(usb_device_t *dev, uint8_t type, uint8_t index,
                                  uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)lang;
    static const uint8_t dd[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
        0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x00, 0x04, /* iProduct=4 */
        0x00, 0x01
    };
    if (type == 1) {
        *out = dd;
        *len = sizeof(dd);
        return 0;
    }
    if (type == 3 && index == 4) {
        *out = NULL;
        *len = 1;
        return 0;
    }
    return -1;
}

START_TEST(test_ffs_build_strings_desc_null_or_short)
{
    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_null_string;

    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    /* extract_string_from_desc hits line 143 (desc == NULL || len < 2) */
    GByteArray *arr = ffs_build_strings(&dev);
    ck_assert_ptr_nonnull(arr);
    g_byte_array_free(arr, TRUE);
}
END_TEST

static int dummy_desc_truncated_string(usb_device_t *dev, uint8_t type, uint8_t index,
                                       uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)lang;
    static const uint8_t dd[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
        0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x00, 0x02,
        0x00, 0x01
    };
    static const uint8_t trunc_desc[] = {
        50, 3,
        'O', 0, 'K', 0, 0, 0
    };
    if (type == 1) {
        *out = dd;
        *len = sizeof(dd);
        return 0;
    }
    if (type == 3 && index == 2) {
        *out = trunc_desc;
        *len = 6; /* len < desc[0] */
        return 0;
    }
    return -1;
}

static int dummy_desc_dd_short(usb_device_t *dev, uint8_t type, uint8_t index,
                               uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)index; (void)lang;
    static const uint8_t dd[10] = { 0 };
    if (type == 1) {
        *out = dd;
        *len = 10;
        return 0;
    }
    return -1;
}

static int dummy_desc_dd_null(usb_device_t *dev, uint8_t type, uint8_t index,
                              uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)index; (void)lang;
    if (type == 1) {
        *out = NULL;
        *len = 18;
        return 0;
    }
    return -1;
}

static int dummy_desc_string_query_fail(usb_device_t *dev, uint8_t type, uint8_t index,
                                        uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)index; (void)lang;
    static const uint8_t dd[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
        0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x00, 0x02,
        0x00, 0x01
    };
    if (type == 1) {
        *out = dd;
        *len = sizeof(dd);
        return 0;
    }
    return -1;
}

START_TEST(test_ffs_build_strings_branches)
{
    usb_device_ops_t ops;
    usb_device_t dev;
    GByteArray *arr;

    /* 1. Truncated descriptor length (desc[0] > len) */
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_truncated_string;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;
    arr = ffs_build_strings(&dev);
    ck_assert_ptr_nonnull(arr);
    g_byte_array_free(arr, TRUE);

    /* 2. ddlen < 18 */
    ops.get_descriptor = dummy_desc_dd_short;
    arr = ffs_build_strings(&dev);
    ck_assert_ptr_nonnull(arr);
    g_byte_array_free(arr, TRUE);

    /* 3. dd == NULL */
    ops.get_descriptor = dummy_desc_dd_null;
    arr = ffs_build_strings(&dev);
    ck_assert_ptr_nonnull(arr);
    g_byte_array_free(arr, TRUE);

    /* 4. String descriptor lookup returns -1 */
    ops.get_descriptor = dummy_desc_string_query_fail;
    arr = ffs_build_strings(&dev);
    ck_assert_ptr_nonnull(arr);
    g_byte_array_free(arr, TRUE);
}
END_TEST

START_TEST(test_ffs_build_strings_raw_ascii)
{
    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_raw_and_empty_strings;

    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    /* extract_string_from_desc hits line 168 (desc[1] != USB_DESC_STRING -> raw ASCII) */
    GByteArray *arr = ffs_build_strings(&dev);
    ck_assert_ptr_nonnull(arr);

    size_t off = sizeof(struct usb_functionfs_strings_head) + sizeof(uint16_t);
    const char *str = (const char *)(arr->data + off);
    ck_assert_str_eq(str, "Raw String");

    g_byte_array_free(arr, TRUE);
}
END_TEST

static int dummy_desc_i_prod_zero(usb_device_t *dev, uint8_t type, uint8_t index,
                                  uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)index; (void)lang;
    static const uint8_t dd[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
        0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x00, 0x00, /* iManufacturer=0, iProduct=0 */
        0x00, 0x01
    };
    if (type == 1) {
        *out = dd;
        *len = sizeof(dd);
        return 0;
    }
    return -1;
}

START_TEST(test_ffs_build_strings_iprod_zero)
{
    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_i_prod_zero;

    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    GByteArray *arr = ffs_build_strings(&dev);
    ck_assert_ptr_nonnull(arr);
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

static int dummy_desc_sub_len_branches(usb_device_t *dev, uint8_t type, uint8_t index,
                                       uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)index; (void)lang;
    /* Subdescriptor with length 0 at offset 9 */
    static const uint8_t cd_zero_sub[] = {
        9, 2, 11, 0, 1, 1, 0, 0x80, 50,
        0, 0
    };

    if (type == 2) {
        *out = cd_zero_sub;
        *len = sizeof(cd_zero_sub);
        return 0;
    }
    return -1;
}

static int dummy_desc_sub_overflow_branch(usb_device_t *dev, uint8_t type, uint8_t index,
                                         uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)index; (void)lang;
    /* Subdescriptor exceeding cdlen */
    static const uint8_t cd_overflow_sub[] = {
        9, 2, 11, 0, 1, 1, 0, 0x80, 50,
        20, 0
    };

    if (type == 2) {
        *out = cd_overflow_sub;
        *len = sizeof(cd_overflow_sub);
        return 0;
    }
    return -1;
}

START_TEST(test_ffs_build_descriptors_sub_branches)
{
    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_sub_len_branches;

    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    GByteArray *arr = ffs_build_descriptors(&dev);
    ck_assert_ptr_nonnull(arr);
    g_byte_array_free(arr, TRUE);

    ops.get_descriptor = dummy_desc_sub_overflow_branch;
    arr = ffs_build_descriptors(&dev);
    ck_assert_ptr_nonnull(arr);
    g_byte_array_free(arr, TRUE);
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

    rm_rf(tmp);
    ffs_ctx_free(ctx);
    yaml_backend_destroy(dev);
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

START_TEST(test_ffs_detect_udc_mock)
{
    /* Test ffs_detect_udc with mock directory containing a dotfile and a valid UDC */
    g_autofree char *tmp = g_dir_make_tmp("usbvcr_mock_udc_XXXXXX", NULL);
    ck_assert_ptr_nonnull(tmp);

    g_autofree char *dotfile = g_strdup_printf("%s/.hidden", tmp);
    int fd = open(dotfile, O_CREAT | O_WRONLY, 0600);
    if (fd >= 0)
        close(fd);

    g_autofree char *udc_entry = g_strdup_printf("%s/dummy_udc.0", tmp);
    g_mkdir_with_parents(udc_entry, 0755);

    s_mock_udc_dir = tmp;
    char *detected = ffs_detect_udc();
    s_mock_udc_dir = NULL;

    ck_assert_ptr_nonnull(detected);
    ck_assert_str_eq(detected, "dummy_udc.0");
    g_free(detected);

    rm_rf(tmp);
}
END_TEST

START_TEST(test_ffs_setup_device_desc_fail)
{
    ffs_ctx_t *ctx = ffs_ctx_new("mock_gadget", "dummy_udc");
    ck_assert_ptr_nonnull(ctx);

    g_autofree char *tmp = g_dir_make_tmp("usbvcr_ffs_test_XXXXXX", NULL);
    g_free(ctx->configfs_path);
    g_free(ctx->mount_path);
    ctx->configfs_path = g_strdup_printf("%s/configfs", tmp);
    ctx->mount_path = g_strdup_printf("%s/mount", tmp);

    /* Dev with ops returning error for device descriptor */
    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_short; /* returns len < 18 */
    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    /* Should hit line 275: "unable to retrieve device descriptor" and return -1 */
    int ret = ffs_setup(ctx, &dev);
    ck_assert_int_eq(ret, -1);

    rm_rf(tmp);
    ffs_ctx_free(ctx);
}
END_TEST

static int dummy_desc_full_device(usb_device_t *dev, uint8_t type, uint8_t index,
                                  uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)lang;
    static const uint8_t dd[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
        0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x01, 0x02, /* iManufacturer=1, iProduct=2 */
        0x00, 0x01
    };
    static const uint8_t cd[32] = {
        9, 2, 32, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 1, 3, 1, 1, 0,
        7, 5, 0x81, 3, 8, 0, 10,
        7, 5, 0x02, 2, 64, 0, 0
    };
    static const uint8_t mfg_desc[] = {
        16, 3,
        'M', 0, 'a', 0, 'n', 0, 'u', 0, 'f', 0, 'a', 0, 'c', 0
    };
    static const uint8_t prod_desc[] = {
        16, 3,
        'P', 0, 'r', 0, 'o', 0, 'd', 0, 'u', 0, 'c', 0, 't', 0
    };

    if (type == 1) {
        *out = dd;
        *len = sizeof(dd);
        return 0;
    }
    if (type == 2) {
        *out = cd;
        *len = sizeof(cd);
        return 0;
    }
    if (type == 3 && index == 1) {
        *out = mfg_desc;
        *len = sizeof(mfg_desc);
        return 0;
    }
    if (type == 3 && index == 2) {
        *out = prod_desc;
        *len = sizeof(prod_desc);
        return 0;
    }
    return -1;
}

START_TEST(test_ffs_setup_full_success_and_teardown)
{
    ffs_ctx_t *ctx = ffs_ctx_new("test_gadget_success", NULL);
    ck_assert_ptr_nonnull(ctx);

    g_autofree char *tmp = g_dir_make_tmp("usbvcr_ffs_full_XXXXXX", NULL);
    g_autofree char *mock_udc = g_dir_make_tmp("usbvcr_udc_XXXXXX", NULL);

    g_autofree char *udc_entry = g_strdup_printf("%s/dummy_udc.0", mock_udc);
    g_mkdir_with_parents(udc_entry, 0755);

    g_free(ctx->configfs_path);
    g_free(ctx->mount_path);
    ctx->configfs_path = g_strdup_printf("%s/configfs", tmp);
    ctx->mount_path = g_strdup_printf("%s/mount", tmp);

    /* Pre-create mount_path and ep0 */
    g_mkdir_with_parents(ctx->mount_path, 0755);
    g_autofree char *ep0_path = g_strdup_printf("%s/ep0", ctx->mount_path);
    int ep0_fd = open(ep0_path, O_RDWR | O_CREAT, 0600);
    ck_assert_int_ge(ep0_fd, 0);
    close(ep0_fd);

    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_full_device;
    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    s_mock_mount_enabled = 1;
    s_mock_mount_ret = 0;
    s_mock_udc_dir = mock_udc;

    int ret = ffs_setup(ctx, &dev);
    ck_assert_int_eq(ret, 0);
    ck_assert_int_ge(ctx->ep0_fd, 0);
    ck_assert_str_eq(ctx->udc, "dummy_udc.0");

    /* Verify manufacturer and product files were created in configfs strings (lines 316, 326-328) */
    g_autofree char *mfg_file = g_strdup_printf("%s/strings/0x409/manufacturer", ctx->configfs_path);
    ck_assert_int_eq(access(mfg_file, F_OK), 0);

    /* Verify UDC file created and contains dummy_udc.0 (lines 399-401) */
    g_autofree char *udc_file = g_strdup_printf("%s/UDC", ctx->configfs_path);
    ck_assert_int_eq(access(udc_file, F_OK), 0);

    /* Verify ffs_init_configfs_dirs handles symlink existing (line 341 errno == EEXIST) */
    ret = ffs_setup(ctx, &dev);
    ck_assert_int_eq(ret, 0);

    /* Teardown should close ep0_fd (lines 425-426), clear UDC, and remove dirs */
    s_mock_umount_enabled = 1;
    s_mock_umount_ret = 0;
    ffs_teardown(ctx);
    ck_assert_int_eq(ctx->ep0_fd, -1);
    s_mock_umount_enabled = 0;

    /* Teardown with NULL mount_path and NULL configfs_path */
    ffs_ctx_t *ctx_null_paths = ffs_ctx_new("null_paths", NULL);
    g_free(ctx_null_paths->mount_path);
    g_free(ctx_null_paths->configfs_path);
    ctx_null_paths->mount_path = NULL;
    ctx_null_paths->configfs_path = NULL;
    ffs_teardown(ctx_null_paths);
    ffs_ctx_free(ctx_null_paths);

    s_mock_mount_enabled = 0;
    s_mock_udc_dir = NULL;

    rm_rf(mock_udc);
    rm_rf(tmp);
    ffs_ctx_free(ctx);
}
END_TEST

START_TEST(test_ffs_setup_no_udc_detected)
{
    ffs_ctx_t *ctx = ffs_ctx_new("mock_no_udc", NULL);
    ck_assert_ptr_nonnull(ctx);

    g_autofree char *tmp = g_dir_make_tmp("usbvcr_ffs_no_udc_XXXXXX", NULL);
    g_autofree char *empty_udc_dir = g_dir_make_tmp("usbvcr_empty_udc_XXXXXX", NULL);

    g_free(ctx->configfs_path);
    g_free(ctx->mount_path);
    ctx->configfs_path = g_strdup_printf("%s/configfs", tmp);
    ctx->mount_path = g_strdup_printf("%s/mount", tmp);

    g_mkdir_with_parents(ctx->mount_path, 0755);
    g_autofree char *ep0_path = g_strdup_printf("%s/ep0", ctx->mount_path);
    int ep0_fd = open(ep0_path, O_RDWR | O_CREAT, 0600);
    close(ep0_fd);

    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_full_device;
    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    s_mock_mount_enabled = 1;
    s_mock_mount_ret = 0;
    s_mock_udc_dir = empty_udc_dir; /* empty, so ffs_detect_udc returns NULL */

    int ret = ffs_setup(ctx, &dev);
    ck_assert_int_eq(ret, 0);
    ck_assert_ptr_null(ctx->udc); /* line 397 (ctx->udc != NULL) evaluates to FALSE */

    s_mock_mount_enabled = 0;
    s_mock_udc_dir = NULL;
    ffs_teardown(ctx);
    rm_rf(empty_udc_dir);
    rm_rf(tmp);
    ffs_ctx_free(ctx);
}
END_TEST

START_TEST(test_ffs_setup_ep0_open_fail)
{
    ffs_ctx_t *ctx = ffs_ctx_new("mock_open_fail", "dummy_udc");
    ck_assert_ptr_nonnull(ctx);

    g_autofree char *tmp = g_dir_make_tmp("usbvcr_ffs_open_fail_XXXXXX", NULL);
    g_free(ctx->configfs_path);
    g_free(ctx->mount_path);
    ctx->configfs_path = g_strdup_printf("%s/configfs", tmp);
    ctx->mount_path = g_strdup_printf("%s/mount", tmp);

    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_full_device;
    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    /* Mount succeeds, but ep0 does NOT exist in mount_path -> lines 369-374 */
    s_mock_mount_enabled = 1;
    s_mock_mount_ret = 0;

    int ret = ffs_setup(ctx, &dev);
    ck_assert_int_eq(ret, -1);
    ck_assert_int_eq(ctx->ep0_fd, -1);

    s_mock_mount_enabled = 0;
    rm_rf(tmp);
    ffs_ctx_free(ctx);
}
END_TEST

static int dummy_desc_bad_config(usb_device_t *dev, uint8_t type, uint8_t index,
                                 uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)dev; (void)index; (void)lang;
    static const uint8_t dd[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
        0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x01
    };
    if (type == 1) {
        *out = dd;
        *len = sizeof(dd);
        return 0;
    }
    /* Fail on config descriptor */
    return -1;
}

START_TEST(test_ffs_setup_build_descs_fail)
{
    ffs_ctx_t *ctx = ffs_ctx_new("mock_desc_fail", "dummy_udc");
    ck_assert_ptr_nonnull(ctx);

    g_autofree char *tmp = g_dir_make_tmp("usbvcr_ffs_desc_fail_XXXXXX", NULL);
    g_free(ctx->configfs_path);
    g_free(ctx->mount_path);
    ctx->configfs_path = g_strdup_printf("%s/configfs", tmp);
    ctx->mount_path = g_strdup_printf("%s/mount", tmp);

    /* Pre-create ep0 */
    g_mkdir_with_parents(ctx->mount_path, 0755);
    g_autofree char *ep0_path = g_strdup_printf("%s/ep0", ctx->mount_path);
    int ep0_fd = open(ep0_path, O_RDWR | O_CREAT, 0600);
    ck_assert_int_ge(ep0_fd, 0);
    close(ep0_fd);

    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_bad_config;
    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    s_mock_mount_enabled = 1;
    s_mock_mount_ret = 0;

    /* Descs return NULL -> lines 377-381 */
    int ret = ffs_setup(ctx, &dev);
    ck_assert_int_eq(ret, -1);

    s_mock_mount_enabled = 0;
    rm_rf(tmp);
    ffs_ctx_free(ctx);
}
END_TEST

START_TEST(test_ffs_setup_ep0_write_fail)
{
    ffs_ctx_t *ctx = ffs_ctx_new("mock_write_fail", "dummy_udc");
    ck_assert_ptr_nonnull(ctx);

    g_autofree char *tmp = g_dir_make_tmp("usbvcr_ffs_write_fail_XXXXXX", NULL);
    g_free(ctx->configfs_path);
    g_free(ctx->mount_path);
    ctx->configfs_path = g_strdup_printf("%s/configfs", tmp);
    ctx->mount_path = g_strdup_printf("%s/mount", tmp);

    /* Pre-create ep0 as a symlink to /dev/full (which fails on write with ENOSPC) */
    g_mkdir_with_parents(ctx->mount_path, 0755);
    g_autofree char *ep0_path = g_strdup_printf("%s/ep0", ctx->mount_path);
    symlink("/dev/full", ep0_path);

    usb_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_descriptor = dummy_desc_full_device;
    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.ops = &ops;

    s_mock_mount_enabled = 1;
    s_mock_mount_ret = 0;

    /* Writing to ep0 fails -> lines 384-388 */
    int ret = ffs_setup(ctx, &dev);
    ck_assert_int_eq(ret, -1);

    s_mock_mount_enabled = 0;
    rm_rf(tmp);
    ffs_ctx_free(ctx);
}
END_TEST

START_TEST(test_ffs_teardown_warnings)
{
    ffs_ctx_t *ctx = ffs_ctx_new("mock_teardown_warn", "dummy_udc");
    ck_assert_ptr_nonnull(ctx);

    g_autofree char *tmp = g_dir_make_tmp("usbvcr_ffs_warn_XXXXXX", NULL);
    g_free(ctx->configfs_path);
    g_free(ctx->mount_path);
    ctx->configfs_path = g_strdup_printf("%s/configfs", tmp);
    ctx->mount_path = g_strdup_printf("%s/mount", tmp);

    g_mkdir_with_parents(ctx->mount_path, 0755);
    g_mkdir_with_parents(ctx->configfs_path, 0755);

    /* 1. umount fails with EBUSY -> line 433 */
    s_mock_umount_enabled = 1;
    s_mock_umount_ret = -1;
    s_mock_umount_errno = EBUSY;

    /* 2. rmdir(mount_path) fails (not empty) -> line 437 */
    g_autofree char *m_file = g_strdup_printf("%s/keep", ctx->mount_path);
    int fd = open(m_file, O_CREAT | O_WRONLY, 0600);
    if (fd >= 0)
        close(fd);

    /* 3. unlink(link_dst) fails because it is a directory -> line 446 */
    g_autofree char *link_dst = g_strdup_printf("%s/configs/c.1/ffs.%s", ctx->configfs_path, ctx->name);
    g_mkdir_with_parents(link_dst, 0755);

    /* 4. rmdir(fn_dir) fails (not empty) -> line 452 */
    g_autofree char *fn_dir = g_strdup_printf("%s/functions/ffs.%s", ctx->configfs_path, ctx->name);
    g_mkdir_with_parents(fn_dir, 0755);
    g_autofree char *fn_file = g_strdup_printf("%s/keep", fn_dir);
    fd = open(fn_file, O_CREAT | O_WRONLY, 0600);
    if (fd >= 0)
        close(fd);

    /* 5. rmdir(cfg_dir) fails (not empty) -> line 458 */
    g_autofree char *cfg_dir = g_strdup_printf("%s/configs/c.1", ctx->configfs_path);
    g_autofree char *cfg_file = g_strdup_printf("%s/keep", cfg_dir);
    fd = open(cfg_file, O_CREAT | O_WRONLY, 0600);
    if (fd >= 0)
        close(fd);

    /* 6. rmdir(str_dir) fails (not empty) -> line 464 */
    g_autofree char *str_dir = g_strdup_printf("%s/strings/0x409", ctx->configfs_path);
    g_mkdir_with_parents(str_dir, 0755);
    g_autofree char *str_file = g_strdup_printf("%s/keep", str_dir);
    fd = open(str_file, O_CREAT | O_WRONLY, 0600);
    if (fd >= 0)
        close(fd);

    ffs_teardown(ctx);

    s_mock_umount_enabled = 0;
    rm_rf(tmp);
    ffs_ctx_free(ctx);
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
    tcase_add_test(tc, test_ffs_build_descriptors_sub_branches);
    tcase_add_test(tc, test_ffs_build_descriptors_valid);
    tcase_add_test(tc, test_ffs_build_strings);
    tcase_add_test(tc, test_ffs_build_strings_custom_device);
    tcase_add_test(tc, test_ffs_build_strings_invalid_utf16);
    tcase_add_test(tc, test_ffs_build_strings_raw_ascii);
    tcase_add_test(tc, test_ffs_build_strings_desc_null_or_short);
    tcase_add_test(tc, test_ffs_build_strings_branches);
    tcase_add_test(tc, test_ffs_build_strings_iprod_zero);
    tcase_add_test(tc, test_ffs_detect_udc);
    tcase_add_test(tc, test_ffs_detect_udc_mock);
    tcase_add_test(tc, test_ffs_setup_and_teardown_mock);
    tcase_add_test(tc, test_ffs_setup_device_desc_fail);
    tcase_add_test(tc, test_ffs_setup_full_success_and_teardown);
    tcase_add_test(tc, test_ffs_setup_no_udc_detected);
    tcase_add_test(tc, test_ffs_setup_ep0_open_fail);
    tcase_add_test(tc, test_ffs_setup_build_descs_fail);
    tcase_add_test(tc, test_ffs_setup_ep0_write_fail);
    tcase_add_test(tc, test_ffs_teardown_warnings);
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
