/*
 * test_yaml_backend.c - Integration test: load each tests/data/example{1,2,3}
 * canonical-dialect project through yaml_backend and assert
 * get_descriptor/control/ep_in reproduce the exact descriptor and payload
 * bytes the device project declares.
 *
 * Unlike test_yaml_loader.c / test_flow.c (which test the loader and flow
 * engine in isolation), this drives the full usb_device_ops seam yaml_backend
 * bridges them into - the same seam pcap_backend is tested against in
 * test_pcap_backend.c (integration/ and backends/). example1 is plain
 * literal fields; example2 adds schema imports/overrides; example3 (UVC) adds
 * the raw hex escape hatch and isochronous streaming.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <check.h>

#include "yaml_backend.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

#define PROJECT_PATH TEST_DATA_DIR "/example3/example3.yaml"

/* device_descriptor serialized from devices/uvc_camera.yaml, field by field
 * in YAML key order: bLength, bDescriptorType, bcdUSB, bDeviceClass,
 * bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0, idVendor (Logitech
 * 0x046d), idProduct (C920 0x082d), bcdDevice, iManufacturer, iProduct,
 * iSerialNumber, bNumConfigurations. */
static const uint8_t EXPECTED_DEVICE_DESC[18] = {
    0x12, 0x01, 0x00, 0x02, 0xef, 0x02, 0x01, 0x40, 0x6d, 0x04, 0x2d, 0x08,
    0x00, 0x00, 0x01, 0x02, 0x03, 0x01,
};

/* configurations[0] fully serialized: config header + IAD + VC interface
 * (class-specific header/terminals/selector/processing-unit + status
 * endpoint + CS endpoint) + VS interface alt0 (format/frame) + VS interface
 * alt1 (isochronous endpoint). wTotalLength (byte 2-3, LE) is 208, matching
 * sizeof() here. Cross-checked against the YAML by hand and by a standalone
 * Python re-serialization of the same file (see the PR description). */
static const uint8_t EXPECTED_CONFIG_DESC[208] = {
    0x09, 0x02, 0xd0, 0x00, 0x02, 0x01, 0x04, 0xc0, 0xfa, 0x08, 0x0b, 0x00,
    0x02, 0x0e, 0x03, 0x00, 0x05, 0x09, 0x04, 0x00, 0x00, 0x01, 0x0e, 0x01,
    0x00, 0x05, 0x0d, 0x24, 0x01, 0x00, 0x01, 0x42, 0x00, 0x80, 0x8d, 0x5b,
    0x00, 0x01, 0x01, 0x11, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x08, 0x24, 0x02, 0x02,
    0x01, 0x04, 0x00, 0x00, 0x09, 0x24, 0x03, 0x03, 0x01, 0x01, 0x00, 0x05,
    0x00, 0x08, 0x24, 0x04, 0x04, 0x02, 0x01, 0x02, 0x00, 0x0b, 0x24, 0x05,
    0x05, 0x04, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x07, 0x05, 0x81, 0x03,
    0x08, 0x00, 0xff, 0x05, 0x25, 0x03, 0x08, 0x00, 0x09, 0x04, 0x01, 0x00,
    0x00, 0x0e, 0x02, 0x00, 0x06, 0x0e, 0x24, 0x01, 0x01, 0x4f, 0x00, 0x82,
    0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x1b, 0x24, 0x04, 0x01, 0x01,
    0x59, 0x55, 0x59, 0x32, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa,
    0x00, 0x38, 0x9b, 0x71, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x26, 0x24,
    0x05, 0x01, 0x00, 0x00, 0x05, 0xc0, 0x03, 0x00, 0x00, 0x94, 0x11, 0x00,
    0x00, 0x94, 0x11, 0x00, 0x80, 0x25, 0x00, 0x2a, 0x2c, 0x0a, 0x00, 0x00,
    0x2a, 0x2c, 0x0a, 0x00, 0x2a, 0x2c, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x01, 0x01, 0x01, 0x0e, 0x02, 0x00, 0x06, 0x07, 0x05, 0x82,
    0x05, 0xff, 0x03, 0x01,
};

/* probe_commit_response: 26-byte Video Probe/Commit Control reply. */
static const uint8_t EXPECTED_PROBE_COMMIT[26] = {
    0x00, 0x00, 0x01, 0x01, 0x2a, 0x2c, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x20, 0x00, 0x00, 0x80, 0x25, 0x00, 0xff, 0x03,
    0x00, 0x00,
};

/* First two isochronous payloads queued by SET_INTERFACE(alt=1): 2-byte UVC
 * header (bHeaderLength, bmHeaderInfo FID/EOF) + 8 bytes of the device's
 * blank YUYV frame (Y=0, U/V=128 repeating). */
static const uint8_t EXPECTED_ISO_FRAME_0[10] = {
    0x02, 0x02, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80,
};
static const uint8_t EXPECTED_ISO_FRAME_1[10] = {
    0x02, 0x03, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80,
};

/* ── example1: plain literal device (no schemas) ───────────────────────── */

#define PROJECT_PATH_E1 TEST_DATA_DIR "/example1/example1.yaml"

static const uint8_t EXPECTED_DEVICE_DESC_E1[18] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0xd9, 0x04, 0x02, 0x17,
    0x06, 0x04, 0x01, 0x02, 0x00, 0x01,
};

static const uint8_t EXPECTED_CONFIG_DESC_E1[34] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xa0, 0x32, 0x09, 0x04, 0x00,
    0x00, 0x01, 0x03, 0x01, 0x01, 0x00, 0x09, 0x21, 0x11, 0x01, 0x00, 0x01,
    0x22, 0x2d, 0x00, 0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0a,
};

/* report_descriptor codec output, matching the real usbmon capture in
 * tests/integration/test_pcap_hid_capture.c (same keyboard_hello_world.yaml
 * device, encoded through codecs/hid_report.yaml). */
static const uint8_t EXPECTED_REPORT_DESC_E1[45] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
};

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

START_TEST(test_yaml_backend_e1_lifecycle)
{
    usb_device_t *dev = yaml_backend_create(PROJECT_PATH_E1);
    ck_assert_ptr_nonnull(dev);
    ck_assert_str_eq(dev->ops->name, "yaml");
    ck_assert_str_eq(yaml_backend_get_name(dev), "Hello World Injector Keyboard");
}
END_TEST

START_TEST(test_yaml_backend_e1_get_descriptor)
{
    usb_device_t *dev = yaml_backend_create(PROJECT_PATH_E1);
    const uint8_t *desc;
    uint16_t len;
    int rc;

    ck_assert_ptr_nonnull(dev);

    desc = NULL; len = 0;
    rc = dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, &desc, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_DEVICE_DESC_E1));
    ck_assert_mem_eq(desc, EXPECTED_DEVICE_DESC_E1, sizeof(EXPECTED_DEVICE_DESC_E1));

    desc = NULL; len = 0;
    rc = dev->ops->get_descriptor(dev, USB_DESC_CONFIG, 0, 0, &desc, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_CONFIG_DESC_E1));
    ck_assert_mem_eq(desc, EXPECTED_CONFIG_DESC_E1, sizeof(EXPECTED_CONFIG_DESC_E1));
}
END_TEST

START_TEST(test_yaml_backend_e1_control)
{
    usb_device_t *dev = yaml_backend_create(PROJECT_PATH_E1);
    usb_control_packet_header_t setup;
    uint8_t data[256];
    uint16_t len;
    int rc;

    ck_assert_ptr_nonnull(dev);

    /* GET_DESCRIPTOR (HID report), issued on the interface. */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x81;
    setup.request     = 6;
    setup.value       = 0x2200;
    setup.index       = 0;
    setup.length      = sizeof(EXPECTED_REPORT_DESC_E1);
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_REPORT_DESC_E1));
    ck_assert_mem_eq(data, EXPECTED_REPORT_DESC_E1, sizeof(EXPECTED_REPORT_DESC_E1));

    /* SET_IDLE (HID class): ACK, no payload. */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x21;
    setup.request     = 0x0a;
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, 0);

    /* SET_CONFIGURATION(1): ACK, then queues "Hello world" on ep 0x81. */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x00;
    setup.request     = 9;
    setup.value       = 1;
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, 0);
}
END_TEST

START_TEST(test_yaml_backend_e1_ep_in_hello_world)
{
    usb_device_t *dev = yaml_backend_create(PROJECT_PATH_E1);
    usb_control_packet_header_t setup;
    uint8_t data[8];
    char typed[64];
    size_t typed_len = 0;
    uint16_t len;
    int rc, n;

    ck_assert_ptr_nonnull(dev);

    /* Arm the queue via SET_CONFIGURATION(1). */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x00;
    setup.request     = 9;
    setup.value       = 1;
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);

    while ((n = dev->ops->ep_in(dev, 0x81, data, sizeof(data))) > 0)
    {
        ck_assert_int_eq(n, 8);
        uint8_t modifiers = data[0];
        uint8_t keycode = data[2];

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

    /* Drained queue NAKs. */
    n = dev->ops->ep_in(dev, 0x81, data, sizeof(data));
    ck_assert_int_eq(n, 0);
}
END_TEST

/* ── example2: schema imports/overrides (usb_base, hid_base) ───────────── */

#define PROJECT_PATH_E2 TEST_DATA_DIR "/example2/example2.yaml"

/* bcdUSB 0x0110 here (usb_base's template default), unlike example1's
 * 0x0200 - example2 never overrides it. */
static const uint8_t EXPECTED_DEVICE_DESC_E2[18] = {
    0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x08, 0xd9, 0x04, 0x02, 0x17,
    0x06, 0x04, 0x01, 0x02, 0x00, 0x01,
};

/* Two HID interfaces (schema hid_base.interface_descriptor, field order
 * governed by the schema's own order since override fields replace in
 * place - see usb_node_set_prop). */
static const uint8_t EXPECTED_CONFIG_DESC_E2[59] = {
    0x09, 0x02, 0x3b, 0x00, 0x02, 0x01, 0x00, 0xa0, 0x32, 0x09, 0x04, 0x00,
    0x00, 0x01, 0x03, 0x01, 0x01, 0x00, 0x09, 0x21, 0x10, 0x01, 0x00, 0x01,
    0x22, 0x3f, 0x00, 0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0a, 0x09, 0x04,
    0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x09, 0x21, 0x10, 0x01, 0x00,
    0x01, 0x22, 0x65, 0x00, 0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x0a,
};

/* report_descriptor is shorter than example1's: no 6-key rollover array,
 * so its 29 bytes don't actually match either interface's declared
 * wDescriptorLength (63/101) - example2 is the schema/CVE fixture
 * (tests/cve_hid_validation.yaml), not a byte-perfect enumeration like
 * example1. */
static const uint8_t EXPECTED_REPORT_DESC_E2[29] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0xc0,
};

START_TEST(test_yaml_backend_e2_lifecycle)
{
    usb_device_t *dev = yaml_backend_create(PROJECT_PATH_E2);
    ck_assert_ptr_nonnull(dev);
    ck_assert_str_eq(dev->ops->name, "yaml");
    ck_assert_str_eq(yaml_backend_get_name(dev), "Virtual USB Keyboard");
}
END_TEST

START_TEST(test_yaml_backend_e2_get_descriptor)
{
    usb_device_t *dev = yaml_backend_create(PROJECT_PATH_E2);
    const uint8_t *desc;
    uint16_t len;
    int rc;

    ck_assert_ptr_nonnull(dev);

    desc = NULL; len = 0;
    rc = dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, &desc, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_DEVICE_DESC_E2));
    ck_assert_mem_eq(desc, EXPECTED_DEVICE_DESC_E2, sizeof(EXPECTED_DEVICE_DESC_E2));

    desc = NULL; len = 0;
    rc = dev->ops->get_descriptor(dev, USB_DESC_CONFIG, 0, 0, &desc, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_CONFIG_DESC_E2));
    ck_assert_mem_eq(desc, EXPECTED_CONFIG_DESC_E2, sizeof(EXPECTED_CONFIG_DESC_E2));
}
END_TEST

START_TEST(test_yaml_backend_e2_control)
{
    usb_device_t *dev = yaml_backend_create(PROJECT_PATH_E2);
    usb_control_packet_header_t setup;
    uint8_t data[256];
    uint16_t len;
    int rc;

    ck_assert_ptr_nonnull(dev);

    /* GET_DESCRIPTOR (HID report). */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x81;
    setup.request     = 6;
    setup.value       = 0x2200;
    setup.index       = 0;
    setup.length      = sizeof(EXPECTED_REPORT_DESC_E2);
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_REPORT_DESC_E2));
    ck_assert_mem_eq(data, EXPECTED_REPORT_DESC_E2, sizeof(EXPECTED_REPORT_DESC_E2));

    /* Vendor request 0xC0/0xAA: the flow's only action for it is
     * `respond_raw`, which flow.c's parse_actions doesn't implement yet (only
     * respond_with/respond_with_status/transition_state/
     * return_nak_if_queue_empty/queue_endpoint_data are). The rule's action
     * list parses to empty, nothing sets `handled`, so this stalls today -
     * pinning the current (unimplemented) behavior rather than the YAML
     * file's intent. */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0xc0;
    setup.request     = 0xaa;
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, -1);
}
END_TEST

/* ── example3: raw hex escape hatch (UVC class-specific descriptors) and
 * isochronous streaming (SET_INTERFACE -> queue_endpoint_data) ───────── */

START_TEST(test_yaml_backend_lifecycle)
{
    usb_device_t *dev;

    dev = yaml_backend_create("/nonexistent/project.yaml");
    ck_assert_ptr_null(dev);

    dev = yaml_backend_create(PROJECT_PATH);
    ck_assert_ptr_nonnull(dev);
    ck_assert_str_eq(dev->ops->name, "yaml");
    ck_assert_str_eq(yaml_backend_get_name(dev), "USB Video Class Webcam Sample");
}
END_TEST

START_TEST(test_yaml_backend_get_descriptor)
{
    usb_device_t *dev;
    const uint8_t *desc;
    uint16_t len;
    int rc;

    dev = yaml_backend_create(PROJECT_PATH);
    ck_assert_ptr_nonnull(dev);

    desc = NULL; len = 0;
    rc = dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, &desc, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_DEVICE_DESC));
    ck_assert_mem_eq(desc, EXPECTED_DEVICE_DESC, sizeof(EXPECTED_DEVICE_DESC));

    desc = NULL; len = 0;
    rc = dev->ops->get_descriptor(dev, USB_DESC_CONFIG, 0, 0, &desc, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_CONFIG_DESC));
    ck_assert_mem_eq(desc, EXPECTED_CONFIG_DESC, sizeof(EXPECTED_CONFIG_DESC));

    /* Unknown descriptor index stalls. */
    rc = dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 1, 0, &desc, &len);
    ck_assert_int_eq(rc, -1);
}
END_TEST

START_TEST(test_yaml_backend_control_enumeration)
{
    usb_device_t *dev;
    usb_control_packet_header_t setup;
    uint8_t data[256];
    uint16_t len;
    int rc;

    dev = yaml_backend_create(PROJECT_PATH);
    ck_assert_ptr_nonnull(dev);

    /* GET_DESCRIPTOR (device), truncated to an 8-byte wLength like a real
     * enumeration's first probe. */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x80;
    setup.request     = 0x06;
    setup.value       = 0x0100;
    setup.length      = 8;
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, 8);
    ck_assert_mem_eq(data, EXPECTED_DEVICE_DESC, 8);

    /* GET_DESCRIPTOR (configuration), full length. */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x80;
    setup.request     = 0x06;
    setup.value       = 0x0200;
    setup.length      = sizeof(EXPECTED_CONFIG_DESC);
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_CONFIG_DESC));
    ck_assert_mem_eq(data, EXPECTED_CONFIG_DESC, sizeof(EXPECTED_CONFIG_DESC));

    /* SET_CONFIGURATION(1): ACK. */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x00;
    setup.request     = 0x09;
    setup.value       = 1;
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, 0);
}
END_TEST

START_TEST(test_yaml_backend_control_uvc_classes)
{
    usb_device_t *dev;
    usb_control_packet_header_t setup;
    uint8_t data[256];
    uint16_t len;
    int rc;

    dev = yaml_backend_create(PROJECT_PATH);
    ck_assert_ptr_nonnull(dev);

    /* VS PROBE_CONTROL GET_CUR on interface 1. */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x81;
    setup.request     = 0x81;      /* UVC_GET_CUR */
    setup.value       = 0x0100;    /* cs = UVC_VS_PROBE_CONTROL */
    setup.index       = 0x0001;    /* interface 1 */
    setup.length      = sizeof(EXPECTED_PROBE_COMMIT);
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_PROBE_COMMIT));
    ck_assert_mem_eq(data, EXPECTED_PROBE_COMMIT, sizeof(EXPECTED_PROBE_COMMIT));

    /* VS COMMIT_CONTROL GET_CUR: same negotiated block. */
    setup.value = 0x0200;
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_mem_eq(data, EXPECTED_PROBE_COMMIT, sizeof(EXPECTED_PROBE_COMMIT));

    /* SU4 selector unit input select GET_CUR (unit 4, iface 0). */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x81;
    setup.request     = 0x81;
    setup.value       = 0x0100;
    setup.index       = 0x0004;
    setup.length      = 1;
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, 1);
    ck_assert_uint_eq(data[0], 0x01);
}
END_TEST

START_TEST(test_yaml_backend_streaming)
{
    usb_device_t *dev;
    usb_control_packet_header_t setup;
    uint8_t data[256];
    uint16_t len;
    int rc, n;

    dev = yaml_backend_create(PROJECT_PATH);
    ck_assert_ptr_nonnull(dev);

    /* Before SET_INTERFACE(alt=1), nothing is queued on the iso endpoint. */
    n = dev->ops->ep_in(dev, 0x82, data, sizeof(data));
    ck_assert_int_eq(n, 0);

    /* SET_INTERFACE(iface=1, alt=1): arms streaming and queues two
     * synthetic isochronous payloads. */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x01;      /* Host-to-device, Standard, Interface */
    setup.request     = 0x0b;      /* SET_INTERFACE */
    setup.value       = 1;         /* alternate setting 1 */
    setup.index       = 1;         /* interface 1 */
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, 0);

    n = dev->ops->ep_in(dev, 0x82, data, sizeof(data));
    ck_assert_int_eq(n, sizeof(EXPECTED_ISO_FRAME_0));
    ck_assert_mem_eq(data, EXPECTED_ISO_FRAME_0, sizeof(EXPECTED_ISO_FRAME_0));

    n = dev->ops->ep_in(dev, 0x82, data, sizeof(data));
    ck_assert_int_eq(n, sizeof(EXPECTED_ISO_FRAME_1));
    ck_assert_mem_eq(data, EXPECTED_ISO_FRAME_1, sizeof(EXPECTED_ISO_FRAME_1));

    /* Drained queue NAKs. */
    n = dev->ops->ep_in(dev, 0x82, data, sizeof(data));
    ck_assert_int_eq(n, 0);
}
END_TEST

static Suite *yaml_backend_suite(void)
{
    Suite *s;
    TCase *tc_e1, *tc_e2, *tc_e3;

    s = suite_create("yaml_backend");

    tc_e1 = tcase_create("example1_literal");
    tcase_add_test(tc_e1, test_yaml_backend_e1_lifecycle);
    tcase_add_test(tc_e1, test_yaml_backend_e1_get_descriptor);
    tcase_add_test(tc_e1, test_yaml_backend_e1_control);
    tcase_add_test(tc_e1, test_yaml_backend_e1_ep_in_hello_world);
    suite_add_tcase(s, tc_e1);

    tc_e2 = tcase_create("example2_schema");
    tcase_add_test(tc_e2, test_yaml_backend_e2_lifecycle);
    tcase_add_test(tc_e2, test_yaml_backend_e2_get_descriptor);
    tcase_add_test(tc_e2, test_yaml_backend_e2_control);
    suite_add_tcase(s, tc_e2);

    tc_e3 = tcase_create("example3_uvc");
    tcase_add_test(tc_e3, test_yaml_backend_lifecycle);
    tcase_add_test(tc_e3, test_yaml_backend_get_descriptor);
    tcase_add_test(tc_e3, test_yaml_backend_control_enumeration);
    tcase_add_test(tc_e3, test_yaml_backend_control_uvc_classes);
    tcase_add_test(tc_e3, test_yaml_backend_streaming);
    suite_add_tcase(s, tc_e3);

    return s;
}

int main(void)
{
    Suite *s;
    SRunner *sr;
    int failed;

    s = yaml_backend_suite();
    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
