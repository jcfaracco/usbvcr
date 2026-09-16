/* Tests for src/lib/usb_yaml.c */
#include <stdlib.h>
#include <check.h>

#include "usb_yaml.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

/* read_yaml() must return NULL when the file cannot be opened. */
START_TEST(test_read_yaml_missing_file)
{
    packet_t *p = read_yaml(TEST_DATA_DIR "/does_not_exist.yaml");
    ck_assert_ptr_null(p);
}
END_TEST

/* read_yaml() must parse tests/data/hid.yaml without crashing. */
START_TEST(test_read_yaml_hid)
{
    packet_t *p = read_yaml(TEST_DATA_DIR "/hid.yaml");
    /* Parsing the sample HID capture should not fault. */
    (void)p;
}
END_TEST

static Suite *usb_yaml_suite(void)
{
    Suite *s = suite_create("usb_yaml");
    TCase *tc = tcase_create("read_yaml");

    tcase_add_test(tc, test_read_yaml_missing_file);
    tcase_add_test(tc, test_read_yaml_hid);
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
