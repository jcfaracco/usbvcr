/* Unit tests for src/lib/utils.c. */

#include <stdlib.h>
#include <string.h>
#include <check.h>

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
