/* Unit tests for src/lib/yaml_loader.c.
 *
 * yaml_scope_from_path() is fully implemented and tested here.
 * yaml_parse_from_file() / yaml_device_free() are stubs today — the few
 * assertions below just pin their current contract. Extend as the loader
 * grows (imports, templates, null autofill, flow load).
 */

#include <stdlib.h>
#include <check.h>

#include "yaml_loader.h"

/* ── yaml_scope_from_path: each recognized directory maps to its scope ── */

START_TEST(test_scope_each_dir)
{
    ck_assert_int_eq(yaml_scope_from_path("schemas/kbd_base.yaml"), YAML_SCOPE_SCHEMAS);
    ck_assert_int_eq(yaml_scope_from_path("devices/kbd.yaml"), YAML_SCOPE_DEVICES);
    ck_assert_int_eq(yaml_scope_from_path("flows/enum.yaml"),  YAML_SCOPE_FLOWS);
    ck_assert_int_eq(yaml_scope_from_path("tests/cve.yaml"),   YAML_SCOPE_TESTS);
}
END_TEST

/* Only the immediate parent directory decides the scope. */
START_TEST(test_scope_deep_path)
{
    ck_assert_int_eq(yaml_scope_from_path("tests/data/example2/flows/x.yaml"),
                     YAML_SCOPE_FLOWS);
    ck_assert_int_eq(yaml_scope_from_path("/abs/proj/tests/cve.yaml"),
                     YAML_SCOPE_TESTS);
}
END_TEST

/* A file with no directory component has no scope. */
START_TEST(test_scope_no_dir)
{
    ck_assert_int_eq(yaml_scope_from_path("bare.yaml"), YAML_SCOPE_UNKNOWN);
}
END_TEST

/* Unrecognized directories (incl. payloads/) are unknown scopes. */
START_TEST(test_scope_unknown_dir)
{
    ck_assert_int_eq(yaml_scope_from_path("payloads/blob.bin"), YAML_SCOPE_UNKNOWN);
    ck_assert_int_eq(yaml_scope_from_path("random/x.yaml"),     YAML_SCOPE_UNKNOWN);
}
END_TEST

/* Matching is exact: substrings / different case do not match. */
START_TEST(test_scope_no_fuzzy_match)
{
    ck_assert_int_eq(yaml_scope_from_path("mydevices/x.yaml"), YAML_SCOPE_UNKNOWN);
    ck_assert_int_eq(yaml_scope_from_path("Devices/x.yaml"),   YAML_SCOPE_UNKNOWN);
    ck_assert_int_eq(yaml_scope_from_path("device/x.yaml"),    YAML_SCOPE_UNKNOWN);
}
END_TEST

/* NULL path is guarded. */
START_TEST(test_scope_null)
{
    ck_assert_int_eq(yaml_scope_from_path(NULL), YAML_SCOPE_UNKNOWN);
}
END_TEST

/* ── loader stubs: pin current contract (update when implemented) ── */

START_TEST(test_parse_from_file_null_dir)
{
    /* NULL project dir -> NULL. */
    ck_assert_ptr_null(yaml_parse_from_file(NULL));
}
END_TEST

START_TEST(test_parse_from_file_missing_dir)
{
    /* Missing subdirs are not an error: returns an (empty) device today.
     * Parsers are stubs, so nothing populates the tree yet. */
    yaml_device_t *dev = yaml_parse_from_file("/nonexistent/project");
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_null(dev->tree);
    yaml_device_free(dev);
}
END_TEST

START_TEST(test_device_free_null)
{
    /* Freeing NULL must be a safe no-op. */
    yaml_device_free(NULL);
}
END_TEST

START_TEST(test_parse_from_file_example1)
{
    /* Load example 1 of data samples (the project manifest file). */
    yaml_parse_from_file("../data/example1/example1.yaml");
}
END_TEST

static Suite *yaml_loader_suite(void)
{
    Suite *s = suite_create("yaml_loader");

    TCase *tc_scope = tcase_create("scope_from_dir");
    tcase_add_test(tc_scope, test_scope_each_dir);
    tcase_add_test(tc_scope, test_scope_deep_path);
    tcase_add_test(tc_scope, test_scope_no_dir);
    tcase_add_test(tc_scope, test_scope_unknown_dir);
    tcase_add_test(tc_scope, test_scope_no_fuzzy_match);
    tcase_add_test(tc_scope, test_scope_null);
    suite_add_tcase(s, tc_scope);

    TCase *tc_load = tcase_create("loader");
    tcase_add_test(tc_load, test_parse_from_file_null_dir);
    tcase_add_test(tc_load, test_parse_from_file_missing_dir);
    tcase_add_test(tc_load, test_device_free_null);
    tcase_add_test(tc_load, test_parse_from_file_example1);
    suite_add_tcase(s, tc_load);

    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(yaml_loader_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
