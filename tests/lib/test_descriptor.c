/* Unit tests for src/lib/descriptor.c. */

#include <stdlib.h>
#include <stdint.h>
#include <check.h>

#include "descriptor.h"
#include "usb_node.h"
#include "yaml/loader.h"
#include "yaml_backend.h"


/* meson passes -DTEST_DATA_DIR="<srcdir>/data"; fall back for ad-hoc builds. */
#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "../data"
#endif


START_TEST(test_desc_parse_config_simple)
{
    const uint8_t desc_conf[] = {
        0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xA0,
        0x32, 0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01,
        0x01, 0x00, 0x09, 0x21, 0x11, 0x01, 0x00, 0x01,
        0x22, 0x3F, 0x00, 0x07, 0x05, 0x81, 0x03, 0x08,
        0x00, 0x0A,
    };
    uint8_t len = 34;

    usb_interface_info_header_t ii;
    usb_ep_info_header_t ei;

    ck_assert_int_eq(desc_parse_config(desc_conf, len, &ii, &ei), 0);
}
END_TEST

void test_desc_parse_config_from_file(const char *file)
{
    const uint8_t *out = NULL;
    uint16_t len = 0;
    usb_device_t *usb = NULL;

    usb_interface_info_header_t ii;
    usb_ep_info_header_t ei;

    usb = yaml_backend_create(file);

    ck_assert_ptr_nonnull(usb);
    ck_assert_int_eq(usb->ops->get_descriptor(usb, USB_DESC_CONFIG, 0, 0, &out, &len), 0);
    ck_assert_int_eq(desc_parse_config(out, len, &ii, &ei), 0);
}

START_TEST(test_desc_parse_config_1)
{
    test_desc_parse_config_from_file(TEST_DATA_DIR "/example1/example1.yaml");
}
END_TEST

START_TEST(test_desc_parse_config_2)
{
    test_desc_parse_config_from_file(TEST_DATA_DIR "/example2/example2.yaml");
}
END_TEST

START_TEST(test_yaml_backend_get_name)
{
    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);
    ck_assert_str_eq(yaml_backend_get_name(dev), "Hello World Injector Keyboard");

    dev = yaml_backend_create(TEST_DATA_DIR "/example2/example2.yaml");
    ck_assert_ptr_nonnull(dev);
    ck_assert_str_eq(yaml_backend_get_name(dev), "Virtual USB Keyboard");
}
END_TEST


static Suite *descriptor_suite(void)
{
    Suite *s = suite_create("descriptor");
    TCase *tc = tcase_create("desc_parse_config");

    tcase_add_test(tc, test_desc_parse_config_simple);
    tcase_add_test(tc, test_desc_parse_config_1);
    tcase_add_test(tc, test_desc_parse_config_2);
    tcase_add_test(tc, test_yaml_backend_get_name);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(descriptor_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

