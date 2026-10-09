/*
 * test_pcap_backend.c - Unit test validating pcap_backend against the real
 * usbmon capture tests/data/pcap/hid_capture.pcapng.
 *
 * Where tests/integration/test_pcap_backend.c exercises the backend against
 * a synthetic test.pcapng fixture, this one drives it with genuine usbmon
 * bytes (the same capture tests/integration/test_pcap_hid_capture.c decodes
 * at the pcap_trace_t level), so the expected descriptors/report bytes below
 * must match that file, not the integration test's.
 *
 * Covers:
 *   1. create()/destroy() lifecycle, including a missing-file error path.
 *   2. get_descriptor() for DEVICE and CONFIG.
 *   3. control() replies for GET_DESCRIPTOR (device/HID report),
 *      SET_CONFIGURATION and SET_IDLE.
 *   4. ep_in() replaying Interrupt IN reports that spell "Hello world".
 *   5. ep_out() on an endpoint the capture never sent OUT data to.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <check.h>

#include "pcap_backend.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

#define CAPTURE_PATH TEST_DATA_DIR "/pcap/hid_capture.pcapng"

/* Device descriptor captured in hid_capture.pcapng (18 bytes).
 * Vendor 0x04d9, Product 0x1702 (Holtek USB Keyboard). */
static const uint8_t EXPECTED_DEVICE_DESC[18] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08,
    0xd9, 0x04, 0x02, 0x17, 0x06, 0x04, 0x01, 0x02,
    0x00, 0x01,
};

/* Configuration descriptor captured in hid_capture.pcapng (34 bytes total). */
static const uint8_t EXPECTED_CONFIG_DESC[34] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0x80,
    0x32, 0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01,
    0x01, 0x00, 0x09, 0x21, 0x11, 0x01, 0x00, 0x01,
    0x22, 0x2d, 0x00, 0x07, 0x05, 0x81, 0x03, 0x08,
    0x00, 0x0a,
};

/* HID report descriptor captured in hid_capture.pcapng (45 bytes). */
static const uint8_t EXPECTED_HID_REPORT_DESC[45] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07,
    0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00,
    0x29, 0x65, 0x81, 0x00, 0xc0,
};

/* Boot-keyboard usage -> ASCII, only for the keys "Hello world" uses. */
static char hid_keycode_to_char(uint8_t code)
{
    switch (code)
    {
    case 0x0b: return 'h';
    case 0x08: return 'e';
    case 0x0f: return 'l';
    case 0x12: return 'o';
    case 0x2c: return ' ';
    case 0x1a: return 'w';
    case 0x15: return 'r';
    case 0x07: return 'd';
    default:   return '?';
    }
}

START_TEST(test_pcap_backend_lifecycle)
{
    usb_device_t *dev;

    dev = pcap_backend_create("/nonexistent/path/capture.pcapng");
    ck_assert_ptr_null(dev);

    dev = pcap_backend_create(CAPTURE_PATH);
    ck_assert_ptr_nonnull(dev);
    ck_assert_str_eq(dev->ops->name, "pcap");

    pcap_backend_destroy(dev);
}
END_TEST

START_TEST(test_pcap_backend_get_descriptor)
{
    usb_device_t *dev;
    const uint8_t *desc;
    uint16_t len;
    int rc;

    dev = pcap_backend_create(CAPTURE_PATH);
    ck_assert_ptr_nonnull(dev);

    /* Query device descriptor */
    desc = NULL;
    len = 0;
    rc = dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, &desc, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_DEVICE_DESC));
    ck_assert_mem_eq(desc, EXPECTED_DEVICE_DESC, sizeof(EXPECTED_DEVICE_DESC));

    /* Query config descriptor */
    desc = NULL;
    len = 0;
    rc = dev->ops->get_descriptor(dev, USB_DESC_CONFIG, 0, 0, &desc, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_CONFIG_DESC));
    ck_assert_mem_eq(desc, EXPECTED_CONFIG_DESC, sizeof(EXPECTED_CONFIG_DESC));

    /* Unknown descriptor type stalls */
    rc = dev->ops->get_descriptor(dev, 0xfe, 0, 0, &desc, &len);
    ck_assert_int_eq(rc, -1);

    pcap_backend_destroy(dev);
}
END_TEST

START_TEST(test_pcap_backend_control_submissions)
{
    usb_device_t *dev;
    usb_control_packet_header_t setup;
    uint8_t data[256];
    uint16_t len;
    int rc;

    dev = pcap_backend_create(CAPTURE_PATH);
    ck_assert_ptr_nonnull(dev);

    /* 1. GET_DESCRIPTOR (Device) */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x80;
    setup.request     = USB_REQ_GET_DESCRIPTOR;
    setup.value       = 0x0100;
    setup.index       = 0;
    setup.length      = sizeof(EXPECTED_DEVICE_DESC);
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_DEVICE_DESC));
    ck_assert_mem_eq(data, EXPECTED_DEVICE_DESC, sizeof(EXPECTED_DEVICE_DESC));

    /* 2. GET_DESCRIPTOR (HID Report Descriptor, 0x2200) */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x81;
    setup.request     = USB_REQ_GET_DESCRIPTOR;
    setup.value       = 0x2200;
    setup.index       = 0;
    setup.length      = sizeof(EXPECTED_HID_REPORT_DESC);
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_HID_REPORT_DESC));
    ck_assert_mem_eq(data, EXPECTED_HID_REPORT_DESC, sizeof(EXPECTED_HID_REPORT_DESC));

    /* 3. SET_CONFIGURATION (Value 1) */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x00;
    setup.request     = 0x09;
    setup.value       = 1;
    setup.index       = 0;
    setup.length      = 0;
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);

    /* 4. SET_IDLE */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x21;
    setup.request     = 0x0a;
    setup.value       = 0;
    setup.index       = 0;
    setup.length      = 0;
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);

    pcap_backend_destroy(dev);
}
END_TEST

START_TEST(test_pcap_backend_ep_in_hello_world)
{
    usb_device_t *dev;
    uint8_t buf[64];
    char typed[64];
    size_t typed_len;
    int n;

    dev = pcap_backend_create(CAPTURE_PATH);
    ck_assert_ptr_nonnull(dev);

    typed_len = 0;

    /* Drain all interrupt reports on ep 0x81 */
    while ((n = dev->ops->ep_in(dev, 0x81, buf, sizeof(buf))) > 0)
    {
        uint8_t modifiers;
        uint8_t keycode;

        ck_assert_int_ge(n, 3);
        modifiers = buf[0];
        keycode = buf[2];

        if (keycode != 0)
        {
            char ch = hid_keycode_to_char(keycode);
            if (modifiers & 0x02)
            {
                ch = (char)toupper((unsigned char)ch);
            }
            ck_assert_uint_lt(typed_len, sizeof(typed) - 1);
            typed[typed_len++] = ch;
        }
    }
    typed[typed_len] = '\0';

    ck_assert_str_eq(typed, "Hello world");

    /* Drained queue returns 0 (NAK) */
    n = dev->ops->ep_in(dev, 0x81, buf, sizeof(buf));
    ck_assert_int_eq(n, 0);

    pcap_backend_destroy(dev);
}
END_TEST

START_TEST(test_pcap_backend_ep_out_validation)
{
    usb_device_t *dev;
    uint8_t dummy[1] = { 0x02 };
    int rc;

    dev = pcap_backend_create(CAPTURE_PATH);
    ck_assert_ptr_nonnull(dev);

    /* hid_capture.pcapng never records an OUT transfer on ep 0x01, so the
     * backend has nothing queued to compare against. */
    rc = dev->ops->ep_out(dev, 0x01, dummy, sizeof(dummy));
    ck_assert_int_eq(rc, -1);

    pcap_backend_destroy(dev);
}
END_TEST

static Suite *pcap_backend_suite(void)
{
    Suite *s;
    TCase *tc;

    s = suite_create("pcap_backend_hid_capture");
    tc = tcase_create("core");

    tcase_add_test(tc, test_pcap_backend_lifecycle);
    tcase_add_test(tc, test_pcap_backend_get_descriptor);
    tcase_add_test(tc, test_pcap_backend_control_submissions);
    tcase_add_test(tc, test_pcap_backend_ep_in_hello_world);
    tcase_add_test(tc, test_pcap_backend_ep_out_validation);

    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s;
    SRunner *sr;
    int failed;

    s = pcap_backend_suite();
    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
