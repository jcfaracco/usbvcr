/*
 * test_pcap_backend.c - Integration test: record a session with pcap_writer,
 * then replay it through pcap_backend and assert every op reproduces the
 * exact payload bytes that were recorded.
 *
 * This is the "recording round-trip" golden test flagged as a TODO in
 * tests/integration/test_pcap_roundtrip.c: pcap_writer_* and pcap_backend
 * are the write and replay halves of the same usbmon capture. A synthetic
 * session is written here (no external fixture) covering:
 *   - GET_DESCRIPTOR(DEVICE) and GET_DESCRIPTOR(CONFIG) control pairs
 *     (exercises both get_descriptor() and control()'s cached-descriptor
 *     path, which key off the submit's wValue regardless of recipient).
 *   - SET_CONFIGURATION, replayed from the generic control-txn queue.
 *   - Interrupt IN completions replayed byte-for-byte via ep_in().
 *   - Interrupt OUT submissions validated byte-for-byte via ep_out(),
 *     including the mismatch path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <check.h>

#include "pcap_backend.h"
#include "pcap/pcap.h"

#define CAPTURE_PATH "/tmp/usbvcr_pcap_backend_record.pcapng"

static const uint8_t DEVICE_DESC_SETUP[8] = {
    0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00
};
static const uint8_t DEVICE_DESC[18] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08,
    0xd9, 0x04, 0x02, 0x17, 0x06, 0x04, 0x01, 0x02,
    0x00, 0x01,
};

static const uint8_t CONFIG_DESC_SETUP[8] = {
    0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0x09, 0x00
};
static const uint8_t CONFIG_DESC[9] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32
};

static const uint8_t SET_CONFIGURATION_SETUP[8] = {
    0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t EP_IN_REPORT_1[8] = { 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t EP_IN_REPORT_2[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static const uint8_t EP_OUT_LED_ON[1]  = { 0x02 };
static const uint8_t EP_OUT_LED_OFF[1] = { 0x00 };

/* Record a representative session: enumeration (two descriptors +
 * SET_CONFIGURATION), two interrupt IN reports, two interrupt OUT
 * submissions. Mirrors a real usbmon capture's shape closely enough to
 * exercise every pcap_backend op. */
static void record_session(const char *path)
{
    pcap_writer_t *w;
    pcap_packet_t pkt;

    remove(path);
    w = pcap_writer_open(path);
    ck_assert_ptr_nonnull(w);

    /* GET_DESCRIPTOR(DEVICE) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 1; pkt.bus = 2; pkt.device = 5;
    pkt.endpoint = 0x80; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'S';
    pkt.setup_valid = 1;
    memcpy(pkt.setup, DEVICE_DESC_SETUP, 8);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 1; pkt.bus = 2; pkt.device = 5;
    pkt.endpoint = 0x80; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'C';
    pkt.data = DEVICE_DESC; pkt.data_len = sizeof(DEVICE_DESC);
    pcap_writer_add(w, &pkt);

    /* GET_DESCRIPTOR(CONFIG) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 2; pkt.bus = 2; pkt.device = 5;
    pkt.endpoint = 0x80; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'S';
    pkt.setup_valid = 1;
    memcpy(pkt.setup, CONFIG_DESC_SETUP, 8);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 2; pkt.bus = 2; pkt.device = 5;
    pkt.endpoint = 0x80; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'C';
    pkt.data = CONFIG_DESC; pkt.data_len = sizeof(CONFIG_DESC);
    pcap_writer_add(w, &pkt);

    /* SET_CONFIGURATION(1) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 3; pkt.bus = 2; pkt.device = 5;
    pkt.endpoint = 0x00; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'S';
    pkt.setup_valid = 1;
    memcpy(pkt.setup, SET_CONFIGURATION_SETUP, 8);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 3; pkt.bus = 2; pkt.device = 5;
    pkt.endpoint = 0x00; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_CONTROL; pkt.event_type = 'C';
    pcap_writer_add(w, &pkt);

    /* Interrupt IN completions, ep 0x81 */
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 10; pkt.bus = 2; pkt.device = 5;
    pkt.endpoint = 0x81; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_INT; pkt.event_type = 'C';
    pkt.data = EP_IN_REPORT_1; pkt.data_len = sizeof(EP_IN_REPORT_1);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 11; pkt.bus = 2; pkt.device = 5;
    pkt.endpoint = 0x81; pkt.dir = PCAP_DIR_IN;
    pkt.xfer = PCAP_XFER_INT; pkt.event_type = 'C';
    pkt.data = EP_IN_REPORT_2; pkt.data_len = sizeof(EP_IN_REPORT_2);
    pcap_writer_add(w, &pkt);

    /* Interrupt OUT submissions, ep 0x01 (LED state) */
    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 20; pkt.bus = 2; pkt.device = 5;
    pkt.endpoint = 0x01; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_INT; pkt.event_type = 'S';
    pkt.data = EP_OUT_LED_ON; pkt.data_len = sizeof(EP_OUT_LED_ON);
    pcap_writer_add(w, &pkt);

    memset(&pkt, 0, sizeof(pkt));
    pkt.urb_id = 21; pkt.bus = 2; pkt.device = 5;
    pkt.endpoint = 0x01; pkt.dir = PCAP_DIR_OUT;
    pkt.xfer = PCAP_XFER_INT; pkt.event_type = 'S';
    pkt.data = EP_OUT_LED_OFF; pkt.data_len = sizeof(EP_OUT_LED_OFF);
    pcap_writer_add(w, &pkt);

    pcap_writer_close(w);
}

START_TEST(test_pcap_backend_lifecycle)
{
    usb_device_t *dev;

    dev = pcap_backend_create("/nonexistent/path/capture.pcapng");
    ck_assert_ptr_null(dev);

    record_session(CAPTURE_PATH);
    dev = pcap_backend_create(CAPTURE_PATH);
    ck_assert_ptr_nonnull(dev);
    ck_assert_str_eq(dev->ops->name, "pcap");

    pcap_backend_destroy(dev);
    remove(CAPTURE_PATH);
}
END_TEST

START_TEST(test_pcap_backend_get_descriptor_matches_recording)
{
    usb_device_t *dev;
    const uint8_t *desc;
    uint16_t len;
    int rc;

    record_session(CAPTURE_PATH);
    dev = pcap_backend_create(CAPTURE_PATH);
    ck_assert_ptr_nonnull(dev);

    desc = NULL; len = 0;
    rc = dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, &desc, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(DEVICE_DESC));
    ck_assert_mem_eq(desc, DEVICE_DESC, sizeof(DEVICE_DESC));

    desc = NULL; len = 0;
    rc = dev->ops->get_descriptor(dev, USB_DESC_CONFIG, 0, 0, &desc, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(CONFIG_DESC));
    ck_assert_mem_eq(desc, CONFIG_DESC, sizeof(CONFIG_DESC));

    pcap_backend_destroy(dev);
    remove(CAPTURE_PATH);
}
END_TEST

START_TEST(test_pcap_backend_control_matches_recording)
{
    usb_device_t *dev;
    usb_control_packet_header_t setup;
    uint8_t data[256];
    uint16_t len;
    int rc;

    record_session(CAPTURE_PATH);
    dev = pcap_backend_create(CAPTURE_PATH);
    ck_assert_ptr_nonnull(dev);

    /* GET_DESCRIPTOR(DEVICE) replayed through control() must match the
     * exact bytes recorded in the capture's completion packet. */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = DEVICE_DESC_SETUP[0];
    setup.request     = DEVICE_DESC_SETUP[1];
    setup.value       = (uint16_t)DEVICE_DESC_SETUP[2] | ((uint16_t)DEVICE_DESC_SETUP[3] << 8);
    setup.index       = (uint16_t)DEVICE_DESC_SETUP[4] | ((uint16_t)DEVICE_DESC_SETUP[5] << 8);
    setup.length      = (uint16_t)DEVICE_DESC_SETUP[6] | ((uint16_t)DEVICE_DESC_SETUP[7] << 8);
    len = 0;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, sizeof(DEVICE_DESC));
    ck_assert_mem_eq(data, DEVICE_DESC, sizeof(DEVICE_DESC));

    /* SET_CONFIGURATION(1) replayed through the generic control-txn queue:
     * recorded with a zero-length completion, so the replay must be ACKed
     * with len 0 too. */
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = SET_CONFIGURATION_SETUP[0];
    setup.request     = SET_CONFIGURATION_SETUP[1];
    setup.value       = (uint16_t)SET_CONFIGURATION_SETUP[2] | ((uint16_t)SET_CONFIGURATION_SETUP[3] << 8);
    setup.index       = (uint16_t)SET_CONFIGURATION_SETUP[4] | ((uint16_t)SET_CONFIGURATION_SETUP[5] << 8);
    setup.length      = (uint16_t)SET_CONFIGURATION_SETUP[6] | ((uint16_t)SET_CONFIGURATION_SETUP[7] << 8);
    len = 0xffff;
    rc = dev->ops->control(dev, &setup, data, &len);
    ck_assert_int_eq(rc, 0);
    ck_assert_uint_eq(len, 0);

    pcap_backend_destroy(dev);
    remove(CAPTURE_PATH);
}
END_TEST

START_TEST(test_pcap_backend_ep_in_matches_recording)
{
    usb_device_t *dev;
    uint8_t buf[64];
    int n;

    record_session(CAPTURE_PATH);
    dev = pcap_backend_create(CAPTURE_PATH);
    ck_assert_ptr_nonnull(dev);

    /* Reports must replay in recording order, byte-for-byte. */
    n = dev->ops->ep_in(dev, 0x81, buf, sizeof(buf));
    ck_assert_int_eq(n, sizeof(EP_IN_REPORT_1));
    ck_assert_mem_eq(buf, EP_IN_REPORT_1, sizeof(EP_IN_REPORT_1));

    n = dev->ops->ep_in(dev, 0x81, buf, sizeof(buf));
    ck_assert_int_eq(n, sizeof(EP_IN_REPORT_2));
    ck_assert_mem_eq(buf, EP_IN_REPORT_2, sizeof(EP_IN_REPORT_2));

    /* Drained queue NAKs. */
    n = dev->ops->ep_in(dev, 0x81, buf, sizeof(buf));
    ck_assert_int_eq(n, 0);

    pcap_backend_destroy(dev);
    remove(CAPTURE_PATH);
}
END_TEST

START_TEST(test_pcap_backend_ep_out_matches_recording)
{
    usb_device_t *dev;
    int rc;

    record_session(CAPTURE_PATH);
    dev = pcap_backend_create(CAPTURE_PATH);
    ck_assert_ptr_nonnull(dev);

    /* First recorded OUT payload replayed back verbatim: matches. */
    rc = dev->ops->ep_out(dev, 0x01, EP_OUT_LED_ON, sizeof(EP_OUT_LED_ON));
    ck_assert_int_eq(rc, 0);

    /* Second recorded OUT payload, fed the wrong bytes: mismatch detected. */
    rc = dev->ops->ep_out(dev, 0x01, EP_OUT_LED_ON, sizeof(EP_OUT_LED_ON));
    ck_assert_int_eq(rc, -1);

    /* Queue now drained: unexpected OUT. */
    rc = dev->ops->ep_out(dev, 0x01, EP_OUT_LED_OFF, sizeof(EP_OUT_LED_OFF));
    ck_assert_int_eq(rc, -1);

    pcap_backend_destroy(dev);
    remove(CAPTURE_PATH);
}
END_TEST

static Suite *pcap_backend_suite(void)
{
    Suite *s;
    TCase *tc;

    s = suite_create("pcap_backend_record");
    tc = tcase_create("core");

    tcase_add_test(tc, test_pcap_backend_lifecycle);
    tcase_add_test(tc, test_pcap_backend_get_descriptor_matches_recording);
    tcase_add_test(tc, test_pcap_backend_control_matches_recording);
    tcase_add_test(tc, test_pcap_backend_ep_in_matches_recording);
    tcase_add_test(tc, test_pcap_backend_ep_out_matches_recording);

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
