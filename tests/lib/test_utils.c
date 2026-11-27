/* Unit tests for src/lib/utils.c. */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <check.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "utils.h"

/* ── cmp_versions ── */

START_TEST(test_cmp_versions_equal)
{
    ck_assert_int_eq(cmp_versions("1.0", "1.0"), 0);
    ck_assert_int_eq(cmp_versions("2.3.1", "2.3.1"), 0);
    ck_assert_int_eq(cmp_versions("0", "0"), 0);
}
END_TEST

START_TEST(test_cmp_versions_greater)
{
    ck_assert_int_eq(cmp_versions("2.0", "1.9"), 1);
    ck_assert_int_eq(cmp_versions("1.10", "1.9"), 1);
    ck_assert_int_eq(cmp_versions("1.0.1", "1.0"), 1);
}
END_TEST

START_TEST(test_cmp_versions_less)
{
    ck_assert_int_eq(cmp_versions("1.0", "1.1"), -1);
    ck_assert_int_eq(cmp_versions("0.9", "1.0"), -1);
    ck_assert_int_eq(cmp_versions("1.2.3", "1.2.4"), -1);
}
END_TEST

START_TEST(test_cmp_versions_empty)
{
    /* Empty string treats missing components as 0. */
    ck_assert_int_eq(cmp_versions("", "0"), 0);
    ck_assert_int_eq(cmp_versions("1", ""), 1);
    ck_assert_int_eq(cmp_versions("", "1"), -1);
}
END_TEST

/* ── str_index_in_set ── */

START_TEST(test_str_index_found_first)
{
    const char *set[] = { "alpha", "beta", "gamma" };
    ck_assert_int_eq(str_index_in_set("alpha", set, 3), 0);
}
END_TEST

START_TEST(test_str_index_found_last)
{
    const char *set[] = { "alpha", "beta", "gamma" };
    ck_assert_int_eq(str_index_in_set("gamma", set, 3), 2);
}
END_TEST

START_TEST(test_str_index_not_found)
{
    const char *set[] = { "alpha", "beta" };
    ck_assert_int_eq(str_index_in_set("delta", set, 2), -1);
}
END_TEST

START_TEST(test_str_index_null_string)
{
    const char *set[] = { "alpha" };
    ck_assert_int_eq(str_index_in_set(NULL, set, 1), -1);
}
END_TEST

START_TEST(test_str_index_null_entry_skipped)
{
    /* A NULL entry inside the set should be skipped safely. */
    const char *set[] = { NULL, "beta" };
    ck_assert_int_eq(str_index_in_set("beta", set, 2), 1);
}
END_TEST

START_TEST(test_str_index_empty_set)
{
    ck_assert_int_eq(str_index_in_set("x", NULL, 0), -1);
}
END_TEST

/* ── trim_string ── */

START_TEST(test_trim_basic)
{
    char dst[64];
    trim_string(dst, "  hello  ", sizeof(dst));
    ck_assert_str_eq(dst, "hello");
}
END_TEST

START_TEST(test_trim_no_whitespace)
{
    char dst[64];
    trim_string(dst, "hello", sizeof(dst));
    ck_assert_str_eq(dst, "hello");
}
END_TEST

START_TEST(test_trim_leading_only)
{
    char dst[64];
    trim_string(dst, "\t  word", sizeof(dst));
    ck_assert_str_eq(dst, "word");
}
END_TEST

START_TEST(test_trim_trailing_only)
{
    char dst[64];
    trim_string(dst, "word  \n", sizeof(dst));
    ck_assert_str_eq(dst, "word");
}
END_TEST

START_TEST(test_trim_strips_comment)
{
    char dst[64];
    trim_string(dst, "value # comment", sizeof(dst));
    ck_assert_str_eq(dst, "value");
}
END_TEST

START_TEST(test_trim_comment_with_spaces)
{
    char dst[64];
    trim_string(dst, "  key  # inline comment  ", sizeof(dst));
    ck_assert_str_eq(dst, "key");
}
END_TEST

START_TEST(test_trim_empty_input)
{
    char dst[64];
    trim_string(dst, "", sizeof(dst));
    ck_assert_str_eq(dst, "");
}
END_TEST

START_TEST(test_trim_only_whitespace)
{
    char dst[64];
    trim_string(dst, "   ", sizeof(dst));
    ck_assert_str_eq(dst, "");
}
END_TEST

START_TEST(test_trim_only_comment)
{
    char dst[64];
    trim_string(dst, "# comment", sizeof(dst));
    ck_assert_str_eq(dst, "");
}
END_TEST

/* ── data dir discovery ── */

START_TEST(test_data_dir_lookup)
{
    g_autofree char *datadir = usbvcr_get_data_dir();
    ck_assert_ptr_nonnull(datadir);
    ck_assert(g_file_test(datadir, G_FILE_TEST_IS_DIR));
}
END_TEST

START_TEST(test_data_dir_env)
{
    /* Save whatever environment meson passed */
    const char *orig_env = g_getenv("USBVCR_DATA_DIR");
    g_autofree char *saved_env = orig_env ? g_strdup(orig_env) : NULL;

    g_setenv("USBVCR_DATA_DIR", "/tmp", 1);
    g_autofree char *datadir = usbvcr_get_data_dir();
    ck_assert_ptr_nonnull(datadir);
    ck_assert_str_eq(datadir, "/tmp");

    /* Non-existent directory should be ignored and fall back */
    g_setenv("USBVCR_DATA_DIR", "/nonexistent_dir_12345", 1);
    g_autofree char *datadir2 = usbvcr_get_data_dir();
    ck_assert_ptr_nonnull(datadir2);
    ck_assert(g_file_test(datadir2, G_FILE_TEST_IS_DIR));

    if (saved_env)
        g_setenv("USBVCR_DATA_DIR", saved_env, 1);
    else
        g_unsetenv("USBVCR_DATA_DIR");
}
END_TEST

START_TEST(test_data_dir_chdir_resolution)
{
    const char *orig_env = g_getenv("USBVCR_DATA_DIR");
    g_autofree char *saved_env = orig_env ? g_strdup(orig_env) : NULL;
    g_unsetenv("USBVCR_DATA_DIR");

    g_autofree char *orig_cwd = g_get_current_dir();

    /* 1. From source root (cwd/.. from build/), "data" directory exists directly */
    ck_assert_int_eq(g_chdir(".."), 0);
    g_autofree char *dir_root = usbvcr_get_data_dir();
    ck_assert_ptr_nonnull(dir_root);
    ck_assert(g_file_test(dir_root, G_FILE_TEST_IS_DIR));
    ck_assert_int_eq(g_chdir(orig_cwd), 0);

    /* 2. From a subdirectory like tests/, data is at ../data */
    ck_assert_int_eq(g_chdir(".."), 0);
    g_autofree char *sub = g_build_filename(g_get_current_dir(), "tests", NULL);
    ck_assert_int_eq(g_chdir(sub), 0);
    g_autofree char *dir_from_sub = usbvcr_get_data_dir();
    ck_assert_ptr_nonnull(dir_from_sub);
    ck_assert(g_file_test(dir_from_sub, G_FILE_TEST_IS_DIR));
    ck_assert_int_eq(g_chdir(orig_cwd), 0);

    /* 3. From /tmp, neither data nor ../data exists, so it resolves from /proc/self/exe */
    ck_assert_int_eq(g_chdir("/tmp"), 0);
    g_autofree char *dir_from_tmp = usbvcr_get_data_dir();
    ck_assert_ptr_nonnull(dir_from_tmp);
    ck_assert(g_file_test(dir_from_tmp, G_FILE_TEST_IS_DIR));

    /* Restore original working directory and environment */
    ck_assert_int_eq(g_chdir(orig_cwd), 0);
    if (saved_env)
        g_setenv("USBVCR_DATA_DIR", saved_env, 1);
}
END_TEST

START_TEST(test_find_data_file)
{
    g_autofree char *kbd = usbvcr_find_data_file("devices", "keyboard");
    ck_assert_ptr_nonnull(kbd);
    ck_assert(g_str_has_suffix(kbd, "keyboard.yaml"));
    ck_assert(g_file_test(kbd, G_FILE_TEST_EXISTS));

    g_autofree char *mouse = usbvcr_find_data_file("devices", "mouse.yaml");
    ck_assert_ptr_nonnull(mouse);
    ck_assert(g_str_has_suffix(mouse, "mouse.yaml"));

    g_autofree char *uvc = usbvcr_find_data_file("devices", "uvc_camera");
    ck_assert_ptr_nonnull(uvc);

    g_autofree char *missing = usbvcr_find_data_file("devices", "nonexistent_device");
    ck_assert_ptr_null(missing);
}
END_TEST

START_TEST(test_find_data_file_null_and_corner_cases)
{
    /* Null filename */
    ck_assert_ptr_null(usbvcr_find_data_file("devices", NULL));
    ck_assert_ptr_null(usbvcr_find_data_file(NULL, NULL));

    /* Null subdir with non-existent file */
    ck_assert_ptr_null(usbvcr_find_data_file(NULL, "nonexistent.yaml"));
    ck_assert_ptr_null(usbvcr_find_data_file(NULL, "nonexistent"));

    /* Null subdir with real file: point USBVCR_DATA_DIR to data/devices */
    g_autofree char *orig_data = usbvcr_get_data_dir();
    g_autofree char *dev_dir = g_build_filename(orig_data, "devices", NULL);
    g_setenv("USBVCR_DATA_DIR", dev_dir, 1);

    g_autofree char *found = usbvcr_find_data_file(NULL, "keyboard");
    ck_assert_ptr_nonnull(found);
    ck_assert(g_str_has_suffix(found, "keyboard.yaml"));

    g_autofree char *found_exact = usbvcr_find_data_file(NULL, "keyboard.yaml");
    ck_assert_ptr_nonnull(found_exact);

    g_unsetenv("USBVCR_DATA_DIR");
}
END_TEST

/* ── usb_parse_speed ── */

START_TEST(test_usb_parse_speed)
{
    ck_assert_int_eq(usb_parse_speed("low"), USB_VCR_SPEED_LOW);
    ck_assert_int_eq(usb_parse_speed("Low"), USB_VCR_SPEED_LOW);
    ck_assert_int_eq(usb_parse_speed("0"), USB_VCR_SPEED_LOW);

    ck_assert_int_eq(usb_parse_speed("full"), USB_VCR_SPEED_FULL);
    ck_assert_int_eq(usb_parse_speed("FULL"), USB_VCR_SPEED_FULL);
    ck_assert_int_eq(usb_parse_speed("1"), USB_VCR_SPEED_FULL);

    ck_assert_int_eq(usb_parse_speed("high"), USB_VCR_SPEED_HIGH);
    ck_assert_int_eq(usb_parse_speed("High"), USB_VCR_SPEED_HIGH);
    ck_assert_int_eq(usb_parse_speed("2"), USB_VCR_SPEED_HIGH);

    ck_assert_int_eq(usb_parse_speed("super"), USB_VCR_SPEED_SUPER);
    ck_assert_int_eq(usb_parse_speed("SuperSpeed"), USB_VCR_SPEED_SUPER);
    ck_assert_int_eq(usb_parse_speed("3"), USB_VCR_SPEED_SUPER);

    ck_assert_int_eq(usb_parse_speed(NULL), -1);
    ck_assert_int_eq(usb_parse_speed("invalid"), -1);
    ck_assert_int_eq(usb_parse_speed(""), -1);
}
END_TEST

static Suite *utils_suite(void)
{
    Suite *s = suite_create("utils");

    TCase *tc_cmp = tcase_create("cmp_versions");
    tcase_add_test(tc_cmp, test_cmp_versions_equal);
    tcase_add_test(tc_cmp, test_cmp_versions_greater);
    tcase_add_test(tc_cmp, test_cmp_versions_less);
    tcase_add_test(tc_cmp, test_cmp_versions_empty);
    suite_add_tcase(s, tc_cmp);

    TCase *tc_idx = tcase_create("str_index_in_set");
    tcase_add_test(tc_idx, test_str_index_found_first);
    tcase_add_test(tc_idx, test_str_index_found_last);
    tcase_add_test(tc_idx, test_str_index_not_found);
    tcase_add_test(tc_idx, test_str_index_null_string);
    tcase_add_test(tc_idx, test_str_index_null_entry_skipped);
    tcase_add_test(tc_idx, test_str_index_empty_set);
    suite_add_tcase(s, tc_idx);

    TCase *tc_trim = tcase_create("trim_string");
    tcase_add_test(tc_trim, test_trim_basic);
    tcase_add_test(tc_trim, test_trim_no_whitespace);
    tcase_add_test(tc_trim, test_trim_leading_only);
    tcase_add_test(tc_trim, test_trim_trailing_only);
    tcase_add_test(tc_trim, test_trim_strips_comment);
    tcase_add_test(tc_trim, test_trim_comment_with_spaces);
    tcase_add_test(tc_trim, test_trim_empty_input);
    tcase_add_test(tc_trim, test_trim_only_whitespace);
    tcase_add_test(tc_trim, test_trim_only_comment);
    suite_add_tcase(s, tc_trim);

    TCase *tc_data = tcase_create("data_discovery");
    tcase_add_test(tc_data, test_data_dir_lookup);
    tcase_add_test(tc_data, test_data_dir_env);
    tcase_add_test(tc_data, test_data_dir_chdir_resolution);
    tcase_add_test(tc_data, test_find_data_file);
    tcase_add_test(tc_data, test_find_data_file_null_and_corner_cases);
    tcase_add_test(tc_data, test_usb_parse_speed);
    suite_add_tcase(s, tc_data);

    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(utils_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
