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
    yaml_backend_destroy(usb);
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

void desc_set_interface_info_header(const uint8_t *cfg, uint8_t cur_iface,
                                    size_t offset,
                                    usb_interface_info_header_t *ii);
void desc_set_ep_info_header(const uint8_t *cfg, uint8_t cur_iface,
                             size_t offset,
                             usb_ep_info_header_t *ei);

START_TEST(test_desc_parse_config_errors_and_boundaries)
{
    usb_interface_info_header_t ii;
    usb_ep_info_header_t ei;

    /* 1. Zero-length descriptor */
    const uint8_t zero_len_desc[] = { 0x00, 0x02 };
    ck_assert_int_eq(desc_parse_config(zero_len_desc, sizeof(zero_len_desc), &ii, &ei), -1);

    /* 2. Buffer overrun (bLength larger than remaining buffer) */
    const uint8_t overrun_desc[] = { 0x09, 0x02, 0x00, 0x00 };
    ck_assert_int_eq(desc_parse_config(overrun_desc, sizeof(overrun_desc), &ii, &ei), -1);

    /* 3. Short interface descriptor (bLength < 9) */
    const uint8_t short_iface_desc[] = {
        0x09, 0x02, 0x0f, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32, /* Config */
        0x06, 0x04, 0x00, 0x00, 0x00, 0x00                     /* Interface with blen=6 (<9) */
    };
    ck_assert_int_eq(desc_parse_config(short_iface_desc, sizeof(short_iface_desc), &ii, &ei), 0);
    ck_assert_uint_eq(ii.interface_count, 0);

    /* 4. Interface number exceeding max supported (cur_iface >= 32) */
    const uint8_t out_of_range_iface_desc[] = {
        0x09, 0x02, 0x12, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32, /* Config */
        0x09, 0x04, 0x20, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00  /* bInterfaceNumber = 32 */
    };
    ck_assert_int_eq(desc_parse_config(out_of_range_iface_desc, sizeof(out_of_range_iface_desc), &ii, &ei), 0);
    ck_assert_uint_eq(ii.interface_count, 0);

    /* 5. Short endpoint descriptor (bLength < 7) */
    const uint8_t short_ep_desc[] = {
        0x09, 0x02, 0x15, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32, /* Config */
        0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, /* Interface 0 */
        0x05, 0x05, 0x81, 0x03, 0x08                          /* Endpoint with blen=5 (<7) */
    };
    ck_assert_int_eq(desc_parse_config(short_ep_desc, sizeof(short_ep_desc), &ii, &ei), 0);
    /* Endpoint was skipped, so type should remain invalid (255) */
    ck_assert_uint_eq(ei.type[usb_ep_index(0x81)], 255);

    /* 6. Valid endpoint parsing verification */
    const uint8_t valid_ep_desc[] = {
        0x09, 0x02, 0x19, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32, /* Config */
        0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00, /* Interface 0, class=3, sub=1, proto=2 */
        0x07, 0x05, 0x82, 0x03, 0x40, 0x00, 0x08               /* Endpoint 0x82, attr=3, maxpkt=64, interval=8 */
    };
    ck_assert_int_eq(desc_parse_config(valid_ep_desc, sizeof(valid_ep_desc), &ii, &ei), 0);
    ck_assert_uint_eq(ii.interface_count, 1);
    ck_assert_uint_eq(ii.interface_class[0], 3);
    ck_assert_uint_eq(ii.interface_subclass[0], 1);
    ck_assert_uint_eq(ii.interface_protocol[0], 2);
    uint8_t ep2_idx = usb_ep_index(0x82);
    ck_assert_uint_eq(ei.type[ep2_idx], 3);
    ck_assert_uint_eq(ei.interval[ep2_idx], 8);
    ck_assert_uint_eq(ei.max_packet_size[ep2_idx], 64);
    ck_assert_uint_eq(ei.interface[ep2_idx], 0);
}
END_TEST

START_TEST(test_desc_set_interface_and_ep_direct)
{
    usb_interface_info_header_t ii = {0};
    usb_ep_info_header_t ei = {0};

    const uint8_t iface_raw[] = {
        0x09, 0x04, 0x02, 0x00, 0x00, 0x0A, 0x0B, 0x0C, 0x00
    };
    /* Test cur_iface = 2 when count was 0: should set count to 3 */
    desc_set_interface_info_header(iface_raw, 2, 0, &ii);
    ck_assert_uint_eq(ii.interface_count, 3);
    ck_assert_uint_eq(ii.interface[2], 2);
    ck_assert_uint_eq(ii.interface_class[2], 0x0A);
    ck_assert_uint_eq(ii.interface_subclass[2], 0x0B);
    ck_assert_uint_eq(ii.interface_protocol[2], 0x0C);

    /* Calling again with smaller cur_iface should not decrease count */
    desc_set_interface_info_header(iface_raw, 1, 0, &ii);
    ck_assert_uint_eq(ii.interface_count, 3);

    /* Calling with cur_iface >= 32 */
    desc_set_interface_info_header(iface_raw, 32, 0, &ii);
    ck_assert_uint_eq(ii.interface_count, 3);

    /* Direct EP header setting */
    const uint8_t ep_raw[] = {
        0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x04 /* EP 0x02 OUT, Bulk (2), MaxPacket=512, Interval=4 */
    };
    desc_set_ep_info_header(ep_raw, 1, 0, &ei);
    uint8_t ep2_out = usb_ep_index(0x02);
    ck_assert_uint_eq(ei.type[ep2_out], 2);
    ck_assert_uint_eq(ei.interface[ep2_out], 1);
    ck_assert_uint_eq(ei.max_packet_size[ep2_out], 512);
    ck_assert_uint_eq(ei.interval[ep2_out], 4);
}
END_TEST

START_TEST(test_yaml_backend_get_name)
{
    usb_device_t *dev = yaml_backend_create(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);
    ck_assert_str_eq(yaml_backend_get_name(dev), "Hello World Injector Keyboard");
    yaml_backend_destroy(dev);

    dev = yaml_backend_create(TEST_DATA_DIR "/example2/example2.yaml");
    ck_assert_ptr_nonnull(dev);
    ck_assert_str_eq(yaml_backend_get_name(dev), "Virtual USB Keyboard");
    yaml_backend_destroy(dev);
}
END_TEST


static Suite *descriptor_suite(void)
{
    Suite *s = suite_create("descriptor");
    TCase *tc = tcase_create("desc_parse_config");

    tcase_add_test(tc, test_desc_parse_config_simple);
    tcase_add_test(tc, test_desc_parse_config_1);
    tcase_add_test(tc, test_desc_parse_config_2);
    tcase_add_test(tc, test_desc_parse_config_errors_and_boundaries);
    tcase_add_test(tc, test_desc_set_interface_and_ep_direct);
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

