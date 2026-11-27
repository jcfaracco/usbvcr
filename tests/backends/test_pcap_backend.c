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
#include <glib.h>
#include <glib/gstdio.h>

#include "pcap_backend.h"
#include "pcap/pcap.h"

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

START_TEST(test_pcap_backend_control_edge_cases)
{
    usb_device_t *dev = pcap_backend_create(CAPTURE_PATH);
    usb_control_packet_header_t setup;
    uint8_t data[256];
    uint16_t len;
    int rc;
    usb_device_t fake;

    ck_assert_ptr_nonnull(dev);

    /* 1. GET_DESCRIPTOR (Device) - first query pops from control_queue */
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

    /* 2. Re-querying the same setup hits control_cache */
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(EXPECTED_DEVICE_DESC));
    ck_assert_mem_eq(data, EXPECTED_DEVICE_DESC, sizeof(EXPECTED_DEVICE_DESC));

    /* 3. Re-query with len == NULL */
    rc = dev->ops->control(dev, &setup, data, NULL);
    ck_assert_int_eq(rc, 0);

    /* 4. Fallback SET_CONFIGURATION (request 9, length 0) not in trace */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x00;
    setup.request     = USB_REQ_SET_CONFIGURATION;
    setup.value       = 42;
    setup.length      = 0;
    len = 99;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, 0);
    /* Also with len == NULL */
    rc = dev->ops->control(dev, &setup, data, NULL);
    ck_assert_int_eq(rc, 0);

    /* 5. Fallback SET_INTERFACE (request 11, length 0) not in trace */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x01;
    setup.request     = USB_REQ_SET_INTERFACE;
    setup.value       = 1;
    setup.index       = 1;
    setup.length      = 0;
    len = 99;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, 0);
    /* Also with len == NULL */
    rc = dev->ops->control(dev, &setup, data, NULL);
    ck_assert_int_eq(rc, 0);

    /* 6. Unhandled control setup returns -1 */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0xc0;
    setup.request     = 0xee;
    setup.value       = 0;
    setup.length      = 16;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, -1);

    /* 7. Invalid parameters */
    ck_assert_int_eq(dev->ops->control(NULL, &setup, data, &len), -1);
    memset(&fake, 0, sizeof(fake));
    fake.ops = dev->ops;
    fake.state = NULL;
    ck_assert_int_eq(dev->ops->control(&fake, &setup, data, &len), -1);
    ck_assert_int_eq(dev->ops->control(dev, NULL, data, &len), -1);

    pcap_backend_destroy(dev);
}
END_TEST

START_TEST(test_pcap_backend_descriptor_edge_cases)
{
    usb_device_t *dev = pcap_backend_create(CAPTURE_PATH);
    const uint8_t *desc = NULL;
    uint16_t len = 0;
    usb_device_t fake;

    ck_assert_ptr_nonnull(dev);

    /* Optional out and len pointers */
    ck_assert_int_eq(dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, NULL, &len), 0);
    ck_assert_int_eq(dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, &desc, NULL), 0);
    ck_assert_int_eq(dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, NULL, NULL), 0);

    /* Unknown descriptor type */
    ck_assert_int_eq(dev->ops->get_descriptor(dev, 0x99, 0, 0, &desc, &len), -1);

    /* Invalid device pointer */
    ck_assert_int_eq(dev->ops->get_descriptor(NULL, USB_DESC_DEVICE, 0, 0, &desc, &len), -1);
    memset(&fake, 0, sizeof(fake));
    fake.ops = dev->ops;
    fake.state = NULL;
    ck_assert_int_eq(dev->ops->get_descriptor(&fake, USB_DESC_DEVICE, 0, 0, &desc, &len), -1);

    pcap_backend_destroy(dev);
}
END_TEST

START_TEST(test_pcap_backend_ops_and_ep_edge_cases)
{
    usb_device_t *dev = pcap_backend_create(CAPTURE_PATH);
    uint8_t buf[64];
    usb_device_t fake;

    ck_assert_ptr_nonnull(dev);

    /* tick hook */
    dev->ops->tick(dev);

    /* get_speed returns -1 */
    ck_assert_int_eq(dev->ops->get_speed(dev), -1);

    /* ep_in invalid dev / state */
    ck_assert_int_eq(dev->ops->ep_in(NULL, 0x81, buf, sizeof(buf)), -1);
    memset(&fake, 0, sizeof(fake));
    fake.ops = dev->ops;
    fake.state = NULL;
    ck_assert_int_eq(dev->ops->ep_in(&fake, 0x81, buf, sizeof(buf)), -1);

    /* ep_in on unrecorded endpoint returns 0 (NAK) */
    ck_assert_int_eq(dev->ops->ep_in(dev, 0x85, buf, sizeof(buf)), 0);

    /* ep_in with buf == NULL returns copy_len without writing */
    ck_assert_int_gt(dev->ops->ep_in(dev, 0x81, NULL, sizeof(buf)), 0);
    ck_assert_int_eq(dev->ops->ep_in(dev, 0x81, NULL, 0), 0);

    /* ep_out invalid dev / state */
    ck_assert_int_eq(dev->ops->ep_out(NULL, 0x01, buf, 1), -1);
    ck_assert_int_eq(dev->ops->ep_out(&fake, 0x01, buf, 1), -1);

    /* destroy NULL is a safe no-op */
    pcap_backend_destroy(NULL);

    pcap_backend_destroy(dev);
}
END_TEST

START_TEST(test_pcap_backend_synthetic_isoc_and_bulk)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_pcap_test_XXXXXX", NULL);
    g_autofree char *pcap_path = g_build_filename(dir, "synth.pcapng", NULL);
    pcap_writer_t *w = pcap_writer_open(pcap_path);
    ck_assert_ptr_nonnull(w);

    pcap_packet_t pkt;

    /* 1. GET_DESCRIPTOR(Device) probe (8 bytes) */
    static const uint8_t dev_setup[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00 };
    static const uint8_t dev_desc_short[8] = { 0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40 };
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 1; pkt.endpoint = 0x80; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'S'; pkt.setup_valid = 1;
    memcpy(pkt.setup, dev_setup, 8);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 1; pkt.endpoint = 0x80; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'C';
    pkt.data = dev_desc_short; pkt.data_len = sizeof(dev_desc_short);
    pcap_writer_add(w, &pkt);

    /* 2. GET_DESCRIPTOR(Device) full (18 bytes) -> triggers record_descriptor len > buf->len update */
    static const uint8_t dev_desc_full[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
        0x11, 0x22, 0x33, 0x44, 0x00, 0x01, 0x01, 0x02,
        0x00, 0x01
    };
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 2; pkt.endpoint = 0x80; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'S'; pkt.setup_valid = 1;
    memcpy(pkt.setup, dev_setup, 8);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 2; pkt.endpoint = 0x80; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'C';
    pkt.data = dev_desc_full; pkt.data_len = sizeof(dev_desc_full);
    pcap_writer_add(w, &pkt);

    /* 3. Control transaction with 0 reply data (e.g. SET_CONFIGURATION) */
    static const uint8_t set_cfg[8] = { 0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 3; pkt.endpoint = 0x00; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'S'; pkt.setup_valid = 1;
    memcpy(pkt.setup, set_cfg, 8);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 3; pkt.endpoint = 0x00; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'C';
    pkt.data = NULL; pkt.data_len = 0;
    pcap_writer_add(w, &pkt);

    /* 4. Orphaned control completion without prior submit (ignored) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 999; pkt.endpoint = 0x80; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'C';
    pkt.data = dev_desc_short; pkt.data_len = sizeof(dev_desc_short);
    pcap_writer_add(w, &pkt);

    /* 5. Control submit without setup_valid (ignored) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 4; pkt.endpoint = 0x00; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'S'; pkt.setup_valid = 0;
    pcap_writer_add(w, &pkt);

    /* 6. Control with unknown event type (ignored) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 5; pkt.endpoint = 0x00; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'E';
    pcap_writer_add(w, &pkt);

    /* 7. Bulk IN packet (completion 'C') */
    static const uint8_t bulk_in[4] = { 0xde, 0xad, 0xbe, 0xef };
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 10; pkt.endpoint = 0x81; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_BULK; pkt.event_type = 'C';
    pkt.data = bulk_in; pkt.data_len = sizeof(bulk_in);
    pcap_writer_add(w, &pkt);

    /* 8. Bulk IN packet with 0 data_len (ignored) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 11; pkt.endpoint = 0x81; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_BULK; pkt.event_type = 'C';
    pkt.data = NULL; pkt.data_len = 0;
    pcap_writer_add(w, &pkt);

    /* 9. Bulk OUT packet (submit 'S') */
    static const uint8_t bulk_out[4] = { 0x01, 0x02, 0x03, 0x04 };
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 20; pkt.endpoint = 0x02; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_BULK; pkt.event_type = 'S';
    pkt.data = bulk_out; pkt.data_len = sizeof(bulk_out);
    pcap_writer_add(w, &pkt);

    /* 10. ISO packet: ndesc == 2, ep 0x84 */
    uint8_t iso_pkt[32 + 8];
    memset(iso_pkt, 0, sizeof(iso_pkt));
    /* Desc 0: offset 0, len 4 */
    iso_pkt[4] = 0; iso_pkt[8] = 4;
    /* Desc 1: offset 4, len 4 */
    iso_pkt[16 + 4] = 4; iso_pkt[16 + 8] = 4;
    /* Data payload (8 bytes) */
    iso_pkt[32] = 0x10; iso_pkt[33] = 0x20; iso_pkt[34] = 0x30; iso_pkt[35] = 0x40;
    iso_pkt[36] = 0x50; iso_pkt[37] = 0x60; iso_pkt[38] = 0x70; iso_pkt[39] = 0x80;

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 30; pkt.endpoint = 0x84; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_ISO; pkt.event_type = 'C';
    pkt.ndesc = 2;
    pkt.data = iso_pkt; pkt.data_len = sizeof(iso_pkt);
    pcap_writer_add(w, &pkt);

    /* 11. ISO packet: ndesc == 0, ep 0x85 */
    static const uint8_t iso_raw[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 31; pkt.endpoint = 0x85; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_ISO; pkt.event_type = 'C';
    pkt.ndesc = 0;
    pkt.data = iso_raw; pkt.data_len = sizeof(iso_raw);
    pcap_writer_add(w, &pkt);

    /* 12. ISO packet: data_len < desc_bytes (ndesc = 1, data_len = 8 < 16) (ignored) */
    uint8_t short_iso[8] = { 0 };
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 32; pkt.endpoint = 0x86; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_ISO; pkt.event_type = 'C';
    pkt.ndesc = 1;
    pkt.data = short_iso; pkt.data_len = sizeof(short_iso);
    pcap_writer_add(w, &pkt);

    /* 13. ISO packet: ndesc = 1, flen = 0 (skipped) */
    uint8_t zero_flen_iso[16];
    memset(zero_flen_iso, 0, sizeof(zero_flen_iso));
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 33; pkt.endpoint = 0x86; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_ISO; pkt.event_type = 'C';
    pkt.ndesc = 1;
    pkt.data = zero_flen_iso; pkt.data_len = sizeof(zero_flen_iso);
    pcap_writer_add(w, &pkt);

    /* 14. ISO packet: ndesc = 1, off + flen > total_buf_len (skipped) */
    uint8_t oob_iso[16 + 2];
    memset(oob_iso, 0, sizeof(oob_iso));
    oob_iso[4] = 5; /* off = 5 */
    oob_iso[8] = 5; /* flen = 5; total_buf_len = 2 */
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 34; pkt.endpoint = 0x86; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_ISO; pkt.event_type = 'C';
    pkt.ndesc = 1;
    pkt.data = oob_iso; pkt.data_len = sizeof(oob_iso);
    pcap_writer_add(w, &pkt);

    /* 15. ISO packets with dir == OUT, event_type == 'S', and data_len == 0 (early returns) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 35; pkt.endpoint = 0x04; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_ISO; pkt.event_type = 'C';
    pkt.data = iso_raw; pkt.data_len = sizeof(iso_raw);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 36; pkt.endpoint = 0x84; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_ISO; pkt.event_type = 'S';
    pkt.data = iso_raw; pkt.data_len = sizeof(iso_raw);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 37; pkt.endpoint = 0x84; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_ISO; pkt.event_type = 'C';
    pkt.data = NULL; pkt.data_len = 0;
    pcap_writer_add(w, &pkt);

    /* 16. Vendor control transaction with data (to test control_cache lines 441-446) */
    static const uint8_t vendor_setup[8] = { 0xc0, 0x01, 0x34, 0x12, 0x00, 0x00, 0x02, 0x00 };
    static const uint8_t vendor_reply[2] = { 0xaa, 0xbb };
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 50; pkt.endpoint = 0x80; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'S'; pkt.setup_valid = 1;
    memcpy(pkt.setup, vendor_setup, 8);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 50; pkt.endpoint = 0x80; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'C';
    pkt.data = vendor_reply; pkt.data_len = sizeof(vendor_reply);
    pcap_writer_add(w, &pkt);

    /* 17. Vendor control transaction with 0 reply data (to test txn->reply_data == NULL in pb_control line 467) */
    static const uint8_t vendor_nodata_setup[8] = { 0x40, 0x02, 0x78, 0x56, 0x00, 0x00, 0x00, 0x00 };
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 51; pkt.endpoint = 0x00; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'S'; pkt.setup_valid = 1;
    memcpy(pkt.setup, vendor_nodata_setup, 8);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 51; pkt.endpoint = 0x00; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'C';
    pkt.data = NULL; pkt.data_len = 0;
    pcap_writer_add(w, &pkt);

    pcap_writer_close(w);

    /* Open backend and verify replay behavior */
    usb_device_t *dev = pcap_backend_create(pcap_path);
    ck_assert_ptr_nonnull(dev);

    /* Check device descriptor was updated to 18 bytes */
    const uint8_t *desc = NULL;
    uint16_t len = 0;
    ck_assert_int_eq(dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, &desc, &len), 0);
    ck_assert_uint_eq(len, sizeof(dev_desc_full));
    ck_assert_mem_eq(desc, dev_desc_full, sizeof(dev_desc_full));

    /* Vendor control transaction: 1st call pops from queue */
    usb_control_packet_header_t setup;
    uint8_t cdata[16];
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0xc0;
    setup.request     = 0x01;
    setup.value       = 0x1234;
    setup.index       = 0;
    setup.length      = 2;
    len = 0;
    ck_assert_int_eq(dev->ops->control(dev, &setup, cdata, &len), 0);
    ck_assert_uint_eq(len, sizeof(vendor_reply));
    ck_assert_mem_eq(cdata, vendor_reply, sizeof(vendor_reply));

    /* 2nd call hits control_cache (lines 441-446) */
    len = 0;
    ck_assert_int_eq(dev->ops->control(dev, &setup, cdata, &len), 0);
    ck_assert_uint_eq(len, sizeof(vendor_reply));
    ck_assert_mem_eq(cdata, vendor_reply, sizeof(vendor_reply));

    /* 3rd call with len == NULL */
    ck_assert_int_eq(dev->ops->control(dev, &setup, cdata, NULL), 0);

    /* Vendor control transaction with 0 reply data (line 467) */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = 0x40;
    setup.request     = 0x02;
    setup.value       = 0x5678;
    setup.index       = 0;
    setup.length      = 0;
    len = 99;
    ck_assert_int_eq(dev->ops->control(dev, &setup, cdata, &len), 0);
    ck_assert_uint_eq(len, 0);

    /* Check bulk IN */
    uint8_t in_buf[16];
    int n = dev->ops->ep_in(dev, 0x81, in_buf, sizeof(in_buf));
    ck_assert_int_eq(n, sizeof(bulk_in));
    ck_assert_mem_eq(in_buf, bulk_in, sizeof(bulk_in));

    /* Check ISO ep 0x84 continuous streaming loop */
    n = dev->ops->ep_in(dev, 0x84, in_buf, sizeof(in_buf));
    ck_assert_int_eq(n, 4);
    ck_assert_mem_eq(in_buf, iso_pkt + 32, 4);

    n = dev->ops->ep_in(dev, 0x84, in_buf, sizeof(in_buf));
    ck_assert_int_eq(n, 4);
    ck_assert_mem_eq(in_buf, iso_pkt + 36, 4);

    /* Continuous loop: pops head, pushes tail */
    n = dev->ops->ep_in(dev, 0x84, in_buf, sizeof(in_buf));
    ck_assert_int_eq(n, 4);
    ck_assert_mem_eq(in_buf, iso_pkt + 32, 4);

    /* Check ISO ep 0x85 (ndesc == 0) */
    n = dev->ops->ep_in(dev, 0x85, in_buf, sizeof(in_buf));
    ck_assert_int_eq(n, sizeof(iso_raw));
    ck_assert_mem_eq(in_buf, iso_raw, sizeof(iso_raw));

    /* Check OUT length mismatch */
    uint8_t bad_len[2] = { 0x01, 0x02 };
    ck_assert_int_eq(dev->ops->ep_out(dev, 0x02, bad_len, sizeof(bad_len)), -1);

    pcap_backend_destroy(dev);

    /* Re-open to test OUT data mismatch and success */
    dev = pcap_backend_create(pcap_path);
    ck_assert_ptr_nonnull(dev);

    uint8_t bad_data[4] = { 0x01, 0x02, 0x03, 0xff };
    ck_assert_int_eq(dev->ops->ep_out(dev, 0x02, bad_data, sizeof(bad_data)), -1);

    pcap_backend_destroy(dev);

    dev = pcap_backend_create(pcap_path);
    ck_assert_ptr_nonnull(dev);

    uint8_t ok_data[4] = { 0x01, 0x02, 0x03, 0x04 };
    ck_assert_int_eq(dev->ops->ep_out(dev, 0x02, ok_data, sizeof(ok_data)), 0);

    pcap_backend_destroy(dev);

    g_remove(pcap_path);
    g_rmdir(dir);
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
    tcase_add_test(tc, test_pcap_backend_control_edge_cases);
    tcase_add_test(tc, test_pcap_backend_descriptor_edge_cases);
    tcase_add_test(tc, test_pcap_backend_ops_and_ep_edge_cases);
    tcase_add_test(tc, test_pcap_backend_synthetic_isoc_and_bulk);

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
