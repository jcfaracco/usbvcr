/* Extended integration tests for src/lib/pcap/record.c.
 *
 * Covers paths not reached by test_record.c:
 *   - record_conn_recv  : RT_BULK_PACKET, RT_START_INTERRUPT_RECEIVING,
 *                         RT_SET_CONFIGURATION, short/unknown types
 *   - record_conn_send  : RT_BULK_PACKET, RT_CONFIGURATION_STATUS,
 *                         short/unknown types
 *   - record_open       : failure path (invalid path)
 *   - record_device_announce: NULL device_desc / NULL config_desc branches
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <check.h>

#include "pcap/pcap.h"
#include "pcap/record.h"
#include "protocol.h"
#include "transport.h"

/* Minimal EPB reader reused from test_record.c. */
typedef struct {
    usbmon_header_t hdr;
    uint8_t         payload[256];
    size_t          payload_len;
} decoded_epb_t;

static size_t read_epbs(const char *path, decoded_epb_t *out, size_t max)
{
    FILE *fp = fopen(path, "rb");
    ck_assert_ptr_nonnull(fp);

    pcapng_shb_t shb;
    uint32_t trailer;
    ck_assert_uint_eq(fread(&shb, sizeof(shb), 1, fp), 1);
    ck_assert_uint_eq(fread(&trailer, sizeof(trailer), 1, fp), 1);

    pcapng_idb_t idb;
    ck_assert_uint_eq(fread(&idb, sizeof(idb), 1, fp), 1);
    ck_assert_uint_eq(fread(&trailer, sizeof(trailer), 1, fp), 1);

    size_t n = 0;
    while (n < max) {
        pcapng_epb_t epb;
        if (fread(&epb, sizeof(epb), 1, fp) != 1)
            break;

        ck_assert_uint_eq(fread(&out[n].hdr, sizeof(usbmon_header_t), 1, fp), 1);
        out[n].payload_len = epb.cap_packet_len - sizeof(usbmon_header_t);
        ck_assert_uint_le(out[n].payload_len, sizeof(out[n].payload));
        if (out[n].payload_len > 0)
            ck_assert_uint_eq(fread(out[n].payload, 1, out[n].payload_len, fp),
                              out[n].payload_len);

        size_t padding = (4 - (epb.cap_packet_len % 4)) % 4;
        if (padding)
            ck_assert_int_eq(fseek(fp, (long)padding, SEEK_CUR), 0);

        ck_assert_uint_eq(fread(&trailer, sizeof(trailer), 1, fp), 1);
        n++;
    }

    fclose(fp);
    return n;
}

/* ── record_open failure ── */

START_TEST(test_record_open_invalid_path)
{
    /* A path to a non-existent directory should cause the open to fail. */
    int rc = record_open("/nonexistent/dir/capture.pcapng");
    ck_assert_int_eq(rc, -1);
    /* Calling close without a successful open must not crash. */
    record_close();
}
END_TEST

/* ── record_device_announce with partial descriptors ── */

START_TEST(test_record_announce_null_device_desc)
{
    const char *path = "/tmp/usbvcr_test_record_null_dd.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    uint8_t cd[9];
    memset(cd, 0xCC, sizeof(cd));
    usb_record_ctx_t ctx = { .bus = 0, .device = 1 };

    /* NULL device descriptor: only config announce (2 EPBs). */
    record_device_announce(NULL, 0, cd, sizeof(cd), &ctx);
    record_close();

    decoded_epb_t epbs[4];
    size_t n = read_epbs(path, epbs, 4);
    ck_assert_uint_eq(n, 2);
    ck_assert_int_eq(epbs[0].hdr.s.setup[3], USB_DESC_CONFIG);
    remove(path);
}
END_TEST

START_TEST(test_record_announce_null_config_desc)
{
    const char *path = "/tmp/usbvcr_test_record_null_cd.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    uint8_t dd[18];
    memset(dd, 0xDD, sizeof(dd));
    usb_record_ctx_t ctx = { .bus = 0, .device = 1 };

    /* NULL config descriptor: only device announce (2 EPBs). */
    record_device_announce(dd, sizeof(dd), NULL, 0, &ctx);
    record_close();

    decoded_epb_t epbs[4];
    size_t n = read_epbs(path, epbs, 4);
    ck_assert_uint_eq(n, 2);
    ck_assert_int_eq(epbs[0].hdr.s.setup[3], USB_DESC_DEVICE);
    remove(path);
}
END_TEST

/* ── record_conn_recv : bulk packet ── */

START_TEST(test_record_conn_recv_bulk)
{
    const char *path = "/tmp/usbvcr_test_record_recv_bulk.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    usb_record_ctx_t ctx = { .bus = 0, .device = 1 };
    usb_interrupt_packet_header_t ih = { .endpoint = 0x02, .status = 0, .length = 3 };
    uint8_t frame[sizeof(ih) + 3];
    memcpy(frame, &ih, sizeof(ih));
    memset(frame + sizeof(ih), 0xBB, 3);

    usb_header_t hdr = { .type = RT_BULK_PACKET, .length = sizeof(frame), .id = 10 };
    record_conn_recv(&hdr, frame, sizeof(frame), &ctx);
    record_close();

    decoded_epb_t epbs[2];
    size_t n = read_epbs(path, epbs, 2);
    ck_assert_uint_eq(n, 1);
    ck_assert_int_eq(epbs[0].hdr.type, 'S');
    ck_assert_uint_eq(epbs[0].hdr.xfer_type, PCAP_XFER_BULK);
    ck_assert_uint_eq(epbs[0].hdr.epnum, 0x02);
    ck_assert_uint_eq(epbs[0].payload_len, 3);

    remove(path);
}
END_TEST

/* ── record_conn_recv : RT_START_INTERRUPT_RECEIVING ── */

START_TEST(test_record_conn_recv_start_interrupt)
{
    const char *path = "/tmp/usbvcr_test_record_recv_startirq.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    usb_record_ctx_t ctx = { .bus = 0, .device = 1 };
    uint8_t ep = 0x81;
    usb_header_t hdr = { .type = RT_START_INTERRUPT_RECEIVING, .length = 1, .id = 5 };
    record_conn_recv(&hdr, &ep, 1, &ctx);
    record_close();

    decoded_epb_t epbs[2];
    size_t n = read_epbs(path, epbs, 2);
    ck_assert_uint_eq(n, 1);
    ck_assert_int_eq(epbs[0].hdr.type, 'S');
    ck_assert_uint_eq(epbs[0].hdr.xfer_type, PCAP_XFER_INT);
    /* Reconstructed IN ep: ep | 0x80 */
    ck_assert_uint_eq(epbs[0].hdr.epnum, 0x81);

    remove(path);
}
END_TEST

/* ── record_conn_recv : RT_SET_CONFIGURATION ── */

START_TEST(test_record_conn_recv_set_configuration)
{
    const char *path = "/tmp/usbvcr_test_record_recv_setcfg.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    usb_record_ctx_t ctx = { .bus = 0, .device = 1 };
    uint8_t cfg = 1;
    usb_header_t hdr = { .type = RT_SET_CONFIGURATION, .length = 1, .id = 3 };
    record_conn_recv(&hdr, &cfg, 1, &ctx);
    record_close();

    decoded_epb_t epbs[2];
    size_t n = read_epbs(path, epbs, 2);
    ck_assert_uint_eq(n, 1);
    ck_assert_int_eq(epbs[0].hdr.type, 'S');
    ck_assert_uint_eq(epbs[0].hdr.xfer_type, PCAP_XFER_CONTROL);
    ck_assert_uint_eq(epbs[0].hdr.s.setup[1], USB_REQ_SET_CONFIGURATION);
    ck_assert_uint_eq(epbs[0].hdr.s.setup[2], 1); /* config value */

    remove(path);
}
END_TEST

/* ── record_conn_recv : short/unknown type ── */

START_TEST(test_record_conn_recv_short_control_skipped)
{
    const char *path = "/tmp/usbvcr_test_record_recv_short.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    usb_record_ctx_t ctx = { .bus = 0, .device = 1 };
    uint8_t tiny[2] = { 0, 1 };
    /* Control header is 12 bytes; 2 bytes is too short → silently dropped. */
    usb_header_t hdr = { .type = RT_CONTROL_PACKET, .length = 2, .id = 1 };
    record_conn_recv(&hdr, tiny, 2, &ctx);
    record_close();

    decoded_epb_t epbs[2];
    size_t n = read_epbs(path, epbs, 2);
    ck_assert_uint_eq(n, 0);

    remove(path);
}
END_TEST

/* ── record_conn_send : bulk packet ── */

START_TEST(test_record_conn_send_bulk)
{
    const char *path = "/tmp/usbvcr_test_record_send_bulk.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    usb_record_ctx_t ctx = { .bus = 0, .device = 1 };
    usb_interrupt_packet_header_t ih = { .endpoint = 0x02, .status = 0, .length = 4 };
    uint8_t frame[sizeof(ih) + 4];
    memcpy(frame, &ih, sizeof(ih));
    memset(frame + sizeof(ih), 0xAA, 4);

    record_conn_send(RT_BULK_PACKET, 20, frame, sizeof(frame), &ctx);
    record_close();

    decoded_epb_t epbs[2];
    size_t n = read_epbs(path, epbs, 2);
    ck_assert_uint_eq(n, 1);
    ck_assert_int_eq(epbs[0].hdr.type, 'C');
    ck_assert_uint_eq(epbs[0].hdr.xfer_type, PCAP_XFER_BULK);
    /* send-side bulk: ep | 0x80 */
    ck_assert_uint_eq(epbs[0].hdr.epnum, 0x82);
    ck_assert_uint_eq(epbs[0].payload_len, 4);

    remove(path);
}
END_TEST

/* ── record_conn_send : RT_CONFIGURATION_STATUS ── */

START_TEST(test_record_conn_send_configuration_status)
{
    const char *path = "/tmp/usbvcr_test_record_send_cfgstatus.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    usb_record_ctx_t ctx = { .bus = 0, .device = 1 };
    uint8_t payload[2] = { 0, 1 };
    record_conn_send(RT_CONFIGURATION_STATUS, 8, payload, sizeof(payload), &ctx);
    record_close();

    decoded_epb_t epbs[2];
    size_t n = read_epbs(path, epbs, 2);
    ck_assert_uint_eq(n, 1);
    ck_assert_int_eq(epbs[0].hdr.type, 'C');
    ck_assert_uint_eq(epbs[0].hdr.xfer_type, PCAP_XFER_CONTROL);

    remove(path);
}
END_TEST

/* ── record_conn_send : short interrupt (below header size) ── */

START_TEST(test_record_conn_send_short_interrupt_skipped)
{
    const char *path = "/tmp/usbvcr_test_record_send_shortirq.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    usb_record_ctx_t ctx = { .bus = 0, .device = 1 };
    uint8_t tiny[1] = { 0 };
    /* Interrupt header is 4 bytes; 1 byte → silently dropped. */
    record_conn_send(RT_INTERRUPT_PACKET, 1, tiny, sizeof(tiny), &ctx);
    record_close();

    decoded_epb_t epbs[2];
    size_t n = read_epbs(path, epbs, 2);
    ck_assert_uint_eq(n, 0);

    remove(path);
}
END_TEST

static Suite *record_ext_suite(void)
{
    Suite *s = suite_create("record_ext");

    TCase *tc_open = tcase_create("open");
    tcase_add_test(tc_open, test_record_open_invalid_path);
    suite_add_tcase(s, tc_open);

    TCase *tc_ann = tcase_create("announce");
    tcase_add_test(tc_ann, test_record_announce_null_device_desc);
    tcase_add_test(tc_ann, test_record_announce_null_config_desc);
    suite_add_tcase(s, tc_ann);

    TCase *tc_recv = tcase_create("recv");
    tcase_add_test(tc_recv, test_record_conn_recv_bulk);
    tcase_add_test(tc_recv, test_record_conn_recv_start_interrupt);
    tcase_add_test(tc_recv, test_record_conn_recv_set_configuration);
    tcase_add_test(tc_recv, test_record_conn_recv_short_control_skipped);
    suite_add_tcase(s, tc_recv);

    TCase *tc_send = tcase_create("send");
    tcase_add_test(tc_send, test_record_conn_send_bulk);
    tcase_add_test(tc_send, test_record_conn_send_configuration_status);
    tcase_add_test(tc_send, test_record_conn_send_short_interrupt_skipped);
    suite_add_tcase(s, tc_send);

    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(record_ext_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
