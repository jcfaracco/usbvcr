/* Unit tests for match_version() (src/lib/version.c). */

#include <stdlib.h>
#include <check.h>

#include "version.h"

/* Exact supported string maps to its enum. */
START_TEST(test_match_exact)
{
    ck_assert_int_eq(match_version("1.0"), YAML_VERSION_1_0);
}
END_TEST

/* cmp_versions() is numeric: "1" and "1.0.0" compare equal to "1.0". */
START_TEST(test_match_numeric_equivalents)
{
    ck_assert_int_eq(match_version("1"),     YAML_VERSION_1_0);
    ck_assert_int_eq(match_version("1.0.0"), YAML_VERSION_1_0);
}
END_TEST

/* Unsupported versions return the INVALID sentinel. */
START_TEST(test_match_unsupported)
{
    ck_assert_int_eq(match_version("2.0"), YAML_VERSION_INVALID);
    ck_assert_int_eq(match_version("0.9"), YAML_VERSION_INVALID);
}
END_TEST

/* Non-numeric junk parses as 0 -> below 1.0 -> INVALID. */
START_TEST(test_match_garbage)
{
    ck_assert_int_eq(match_version("abc"), YAML_VERSION_INVALID);
    ck_assert_int_eq(match_version(""),    YAML_VERSION_INVALID);
}
END_TEST

/* NULL input is guarded -> INVALID, no deref. */
START_TEST(test_match_null)
{
    ck_assert_int_eq(match_version(NULL), YAML_VERSION_INVALID);
}
END_TEST

static Suite *version_suite(void)
{
    Suite *s = suite_create("version");
    TCase *tc = tcase_create("match_version");

    tcase_add_test(tc, test_match_exact);
    tcase_add_test(tc, test_match_numeric_equivalents);
    tcase_add_test(tc, test_match_unsupported);
    tcase_add_test(tc, test_match_garbage);
    tcase_add_test(tc, test_match_null);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(version_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
