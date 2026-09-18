/* Tests for src/lib/usb_yaml.c */
#include <stdlib.h>
#include <check.h>

#include "usb_yaml.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

/* yaml_parse_from_file() must return NULL when the file cannot be opened. */
START_TEST(test_yaml_parse_from_file_missing_file)
{
    GList *p = yaml_parse_from_file(TEST_DATA_DIR "/does_not_exist.yaml");
    ck_assert_ptr_null(p);
}
END_TEST

/* yaml_parse_from_file() must parse tests/data/test1.yaml without crashing. */
START_TEST(test_yaml_parse_from_file_test1)
{
    GList *pkts = yaml_parse_from_file(TEST_DATA_DIR "/test1.yaml");
    /* Parsing the sample HID capture should not fault. */
    ck_assert(g_list_length(pkts) == 4);
}
END_TEST

static Suite *usb_yaml_suite(void)
{
    Suite *s = suite_create("usb_yaml");
    TCase *tc = tcase_create("yaml_parse_from_file");

    tcase_add_test(tc, test_yaml_parse_from_file_missing_file);
    tcase_add_test(tc, test_yaml_parse_from_file_test1);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    int failed;
    Suite *s = usb_yaml_suite();
    SRunner *sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
