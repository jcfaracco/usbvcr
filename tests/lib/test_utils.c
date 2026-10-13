/* Unit tests for src/lib/utils.c. */

#include <stdlib.h>
#include <string.h>
#include <check.h>

#include "utils.h"

/* ------------------------------------------------------------------ *
 * cmp_versions                                                        *
 * ------------------------------------------------------------------ */

START_TEST(test_cmp_versions_equal)
{
    ck_assert_int_eq(cmp_versions("1.0", "1.0"), 0);
    ck_assert_int_eq(cmp_versions("1", "1.0.0"), 0);
    ck_assert_int_eq(cmp_versions("", ""), 0);
}
END_TEST

START_TEST(test_cmp_versions_less_greater)
{
    ck_assert_int_eq(cmp_versions("1.0", "2.0"), -1);
    ck_assert_int_eq(cmp_versions("2.0", "1.0"), 1);
    ck_assert_int_eq(cmp_versions("1.9", "1.10"), -1);
    ck_assert_int_eq(cmp_versions("1.10", "1.9"), 1);
}
END_TEST

START_TEST(test_cmp_versions_different_lengths)
{
    /* Shorter string runs out of components first; missing ones compare
     * as 0 against the remaining numeric components of the longer one. */
    ck_assert_int_eq(cmp_versions("1.2", "1.2.1"), -1);
    ck_assert_int_eq(cmp_versions("1.2.1", "1.2"), 1);
    ck_assert_int_eq(cmp_versions("1.2.0", "1.2"), 0);
}
END_TEST

/* ------------------------------------------------------------------ *
 * str_index_in_set                                                    *
 * ------------------------------------------------------------------ */

START_TEST(test_str_index_in_set_found)
{
    const char *const set[] = { "alpha", "beta", "gamma" };
    ck_assert_int_eq(str_index_in_set("alpha", set, 3), 0);
    ck_assert_int_eq(str_index_in_set("beta", set, 3), 1);
    ck_assert_int_eq(str_index_in_set("gamma", set, 3), 2);
}
END_TEST

START_TEST(test_str_index_in_set_not_found)
{
    const char *const set[] = { "alpha", "beta" };
    ck_assert_int_eq(str_index_in_set("delta", set, 2), -1);
    ck_assert_int_eq(str_index_in_set("delta", set, 0), -1);
}
END_TEST

START_TEST(test_str_index_in_set_null_entries)
{
    const char *const set[] = { NULL, "beta", NULL };
    ck_assert_int_eq(str_index_in_set("beta", set, 3), 1);
    ck_assert_int_eq(str_index_in_set("missing", set, 3), -1);
}
END_TEST

START_TEST(test_str_index_in_set_null_needle)
{
    const char *const set[] = { "alpha" };
    ck_assert_int_eq(str_index_in_set(NULL, set, 1), -1);
}
END_TEST

/* ------------------------------------------------------------------ *
 * trim_string                                                         *
 * ------------------------------------------------------------------ */

START_TEST(test_trim_string_leading_trailing_space)
{
    char dst[32];
    trim_string(dst, "  hello world  ", sizeof(dst));
    ck_assert_str_eq(dst, "hello world");
}
END_TEST

START_TEST(test_trim_string_comment)
{
    char dst[32];
    trim_string(dst, "value # a trailing comment", sizeof(dst));
    ck_assert_str_eq(dst, "value");
}
END_TEST

START_TEST(test_trim_string_truncates_to_dst_size)
{
    char dst[5];
    trim_string(dst, "abcdefgh", sizeof(dst));
    ck_assert_str_eq(dst, "abcd");
}
END_TEST

START_TEST(test_trim_string_all_whitespace)
{
    char dst[8];
    trim_string(dst, "    ", sizeof(dst));
    ck_assert_str_eq(dst, "");
}
END_TEST

/* ------------------------------------------------------------------ */

static Suite *utils_suite(void)
{
    Suite *s = suite_create("utils");

    TCase *tc_cmp = tcase_create("cmp_versions");
    tcase_add_test(tc_cmp, test_cmp_versions_equal);
    tcase_add_test(tc_cmp, test_cmp_versions_less_greater);
    tcase_add_test(tc_cmp, test_cmp_versions_different_lengths);
    suite_add_tcase(s, tc_cmp);

    TCase *tc_set = tcase_create("str_index_in_set");
    tcase_add_test(tc_set, test_str_index_in_set_found);
    tcase_add_test(tc_set, test_str_index_in_set_not_found);
    tcase_add_test(tc_set, test_str_index_in_set_null_entries);
    tcase_add_test(tc_set, test_str_index_in_set_null_needle);
    suite_add_tcase(s, tc_set);

    TCase *tc_trim = tcase_create("trim_string");
    tcase_add_test(tc_trim, test_trim_string_leading_trailing_space);
    tcase_add_test(tc_trim, test_trim_string_comment);
    tcase_add_test(tc_trim, test_trim_string_truncates_to_dst_size);
    tcase_add_test(tc_trim, test_trim_string_all_whitespace);
    suite_add_tcase(s, tc_trim);

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
