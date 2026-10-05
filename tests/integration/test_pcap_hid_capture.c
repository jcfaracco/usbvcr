/*
 * Integration test: validate a real-world capture against example1.
 *
 * tests/data/pcap/hid_capture.pcapng is a genuine usbmon capture (taken with
 * tcpdump inside a VM) of tests/data/example1 being emulated: a HID keyboard
 * enumerating and then typing "Hello world". Unlike test_pcap_roundtrip.c
 * (which only checks that our own writer and reader agree with each other),
 * this exercises pcap_trace_load() against bytes it did not produce, and
 * cross-checks the decoded descriptors against example1's own device/flow
 * definitions (tests/data/example1/devices/keyboard_hello_world.yaml and
 * flows/hello_world_flow.yaml).
 *
 * Bus number and device address are whatever the kernel happened to assign
 * when the capture was taken, so this test never asserts their value - only
 * that every record in the trace agrees on the same one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <check.h>

#include "pcap/pcap.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

#define CAPTURE_PATH TEST_DATA_DIR "/pcap/hid_capture.pcapng"

/* Byte-for-byte dump of the GET_DESCRIPTOR(DEVICE) reply in the capture.
 * Matches tests/data/example1/devices/keyboard_hello_world.yaml's
 * device_descriptor (idVendor 0x04d9, idProduct 0x1702, bcdDevice 0x0406). */
static const uint8_t DEVICE_DESC[18] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08,
    0xd9, 0x04, 0x02, 0x17, 0x06, 0x04, 0x01, 0x02,
    0x00, 0x01,
};

/* GET_DESCRIPTOR(CONFIG) reply (config + interface + HID + endpoint, 34
 * bytes), matches the same device's configurations[0]. */
static const uint8_t CONFIG_DESC[34] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0x80,
    0x32, 0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01,
    0x01, 0x00, 0x09, 0x21, 0x11, 0x01, 0x00, 0x01,
    0x22, 0x2d, 0x00, 0x07, 0x05, 0x81, 0x03, 0x08,
    0x00, 0x0a,
};

/* GET_DESCRIPTOR(HID REPORT) reply, matches the report descriptor the
 * "hid_report" codec compiles from keyboard_hello_world.yaml. */
static const uint8_t HID_REPORT_DESC[45] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07,
    0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00,
    0x29, 0x65, 0x81, 0x00, 0xc0,
};

/* Boot-keyboard usage -> ASCII, only for the keys "Hello world" actually
 * uses. Not a general HID usage table. */
static char hid_keycode_to_char(uint8_t code)
{
    switch (code) {
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

START_TEST(test_hid_capture_decodes_hello_world)
{
    pcap_trace_t *t = pcap_trace_load(CAPTURE_PATH);
    ck_assert_ptr_nonnull(t);

    size_t n = pcap_trace_count(t);
    ck_assert_uint_eq(n, 67);

    bool have_device_desc = false, have_config_desc = false, have_report_desc = false;
    bool have_set_configuration = false, have_set_idle = false;
    uint8_t bus = 0, device = 0;
    bool have_addr = false;

    char typed[64];
    size_t typed_len = 0;

    for (size_t i = 0; i < n; i++) {
        const pcap_packet_t *p = pcap_trace_get(t, i);
        ck_assert_ptr_nonnull(p);

        /* Every record in a single-device capture must agree on bus/device -
         * their absolute value is an artifact of the capturing host/VM. */
        if (!have_addr) {
            bus = p->bus;
            device = p->device;
            have_addr = true;
        } else {
            ck_assert_uint_eq(p->bus, bus);
            ck_assert_uint_eq(p->device, device);
        }

        /* Regression guard for the flag_setup polarity bug: only a Control
         * Submit may ever carry a setup packet (see pcap_init_usbmon_header
         * in src/lib/pcap/pcap.c). */
        if (p->setup_valid)
            ck_assert_int_eq(p->xfer, PCAP_XFER_CONTROL);

        if (p->xfer != PCAP_XFER_CONTROL)
            continue;

        if (p->event_type == 'C') {
            if (p->data_len == sizeof(DEVICE_DESC) &&
                memcmp(p->data, DEVICE_DESC, sizeof(DEVICE_DESC)) == 0)
                have_device_desc = true;
            else if (p->data_len == sizeof(CONFIG_DESC) &&
                    memcmp(p->data, CONFIG_DESC, sizeof(CONFIG_DESC)) == 0)
                have_config_desc = true;
            else if (p->data_len == sizeof(HID_REPORT_DESC) &&
                    memcmp(p->data, HID_REPORT_DESC, sizeof(HID_REPORT_DESC)) == 0)
                have_report_desc = true;
        }

        if (p->event_type == 'S' && p->setup_valid) {
            /* SET_CONFIGURATION(1): standard, host->device, device recipient. */
            if (p->setup[0] == 0x00 && p->setup[1] == 0x09 && p->setup[2] == 0x01)
                have_set_configuration = true;
            /* SET_IDLE: HID class, host->device, interface recipient. */
            if (p->setup[0] == 0x21 && p->setup[1] == 0x0a)
                have_set_idle = true;
        }
    }

    ck_assert(have_device_desc);
    ck_assert(have_config_desc);
    ck_assert(have_report_desc);
    ck_assert(have_set_configuration);
    ck_assert(have_set_idle);

    /* Replay the Interrupt IN completions in capture order and reconstruct
     * the typed text straight from the boot-keyboard reports. */
    for (size_t i = 0; i < n; i++) {
        const pcap_packet_t *p = pcap_trace_get(t, i);
        if (p->xfer != PCAP_XFER_INT || p->event_type != 'C')
            continue;
        ck_assert_uint_ge(p->data_len, 3);

        uint8_t modifiers = p->data[0];
        uint8_t keycode = p->data[2];
        if (keycode == 0)
            continue;  /* key-release report */

        ck_assert_uint_lt(typed_len, sizeof(typed) - 1);
        char ch = hid_keycode_to_char(keycode);
        if (modifiers & 0x02)  /* either Shift */
            ch = (char)toupper((unsigned char)ch);
        typed[typed_len++] = ch;
    }
    typed[typed_len] = '\0';

    ck_assert_str_eq(typed, "Hello world");

    pcap_trace_free(t);
}
END_TEST

static Suite *hid_capture_suite(void)
{
    Suite *s = suite_create("pcap_hid_capture");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, test_hid_capture_decodes_hello_world);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = hid_capture_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
