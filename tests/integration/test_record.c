/*
 * Integration test: verify transfers actually land in the pcap capture.
 *
 * record_device_announce()/record_conn_send()/record_conn_recv() are the
 * only three places allowed to turn USB activity into Enhanced Packet
 * Blocks (record.c). Since pcap_trace_load() (pcap.c) is still a stub, this
 * test walks the pcapng file byte-for-byte using the same structs the
 * writer uses (pcap.h), instead of going through a reader API that doesn't
 * exist yet.
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

typedef struct {
    usbmon_header_t hdr;
    uint8_t         payload[256];
    size_t          payload_len;
} decoded_epb_t;

/* Walk the Section Header + Interface Description + N Enhanced Packet
 * Blocks pcap_writer_add() wrote, decoding each EPB's usbmon header and
 * payload into `out`. Returns the number of EPBs found. */
static size_t read_epbs(const char *path, decoded_epb_t *out, size_t max)
{
    FILE *fp = fopen(path, "rb");
    ck_assert_ptr_nonnull(fp);

    pcapng_shb_t shb;
    uint32_t trailer;
    ck_assert_uint_eq(fread(&shb, sizeof(shb), 1, fp), 1);
    ck_assert_uint_eq(shb.block_type, 0x0A0D0D0A);
    ck_assert_uint_eq(fread(&trailer, sizeof(trailer), 1, fp), 1);
    ck_assert_uint_eq(trailer, shb.block_len);

    pcapng_idb_t idb;
    ck_assert_uint_eq(fread(&idb, sizeof(idb), 1, fp), 1);
    ck_assert_uint_eq(idb.block_type, 0x00000001);
    ck_assert_uint_eq(fread(&trailer, sizeof(trailer), 1, fp), 1);
    ck_assert_uint_eq(trailer, idb.block_len);

    size_t n = 0;
    while (n < max) {
        pcapng_epb_t epb;
        if (fread(&epb, sizeof(epb), 1, fp) != 1)
            break;  /* EOF */
        ck_assert_uint_eq(epb.block_type, 0x00000006);

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
        ck_assert_uint_eq(trailer, epb.block_len);

        n++;
    }

    fclose(fp);
    return n;
}

START_TEST(test_record_device_announce_writes_four_packets)
{
    const char *path = "/tmp/usbvcr_test_record_announce.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    uint8_t dd[18], cd[9];
    memset(dd, 0xAA, sizeof(dd));
    memset(cd, 0xBB, sizeof(cd));

    usb_record_ctx_t ctx = { .bus = 0, .device = 1 };
    record_device_announce(dd, sizeof(dd), cd, sizeof(cd), &ctx);
    record_close();

    decoded_epb_t epbs[8];
    size_t n = read_epbs(path, epbs, 8);
    ck_assert_uint_eq(n, 4);

    /* GET_DESCRIPTOR(device): Submit then Complete carrying dd on ep0 IN. */
    ck_assert_int_eq(epbs[0].hdr.type, 'S');
    ck_assert_uint_eq(epbs[0].hdr.xfer_type, PCAP_XFER_CONTROL);
    ck_assert_uint_eq(epbs[0].hdr.epnum, 0x80);
    ck_assert_uint_eq(epbs[0].hdr.devnum, 1);
    ck_assert_uint_eq(epbs[0].hdr.busnum, 0);
    ck_assert_int_eq(epbs[0].hdr.s.setup[1], USB_REQ_GET_DESCRIPTOR);
    ck_assert_int_eq(epbs[0].hdr.s.setup[3], USB_DESC_DEVICE);

    ck_assert_int_eq(epbs[1].hdr.type, 'C');
    ck_assert_uint_eq(epbs[1].payload_len, sizeof(dd));
    ck_assert_mem_eq(epbs[1].payload, dd, sizeof(dd));

    /* GET_DESCRIPTOR(config): Submit then Complete carrying cd on ep0 IN. */
    ck_assert_int_eq(epbs[2].hdr.type, 'S');
    ck_assert_int_eq(epbs[2].hdr.s.setup[3], USB_DESC_CONFIG);

    ck_assert_int_eq(epbs[3].hdr.type, 'C');
    ck_assert_uint_eq(epbs[3].payload_len, sizeof(cd));
    ck_assert_mem_eq(epbs[3].payload, cd, sizeof(cd));

    remove(path);
}
END_TEST

START_TEST(test_record_conn_send_recv_writes_packets)
{
    const char *path = "/tmp/usbvcr_test_record_connio.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    usb_record_ctx_t ctx = { .bus = 0, .device = 1 };

    /* 1. Host -> device control SETUP (recv side). */
    usb_control_packet_header_t setup = {
        .endpoint = 0, .request = USB_REQ_GET_DESCRIPTOR,
        .requesttype = USB_DIR_IN, .status = 0,
        .value = (USB_DESC_DEVICE << 8), .index = 0, .length = 18,
    };
    usb_header_t hdr = { .type = RT_CONTROL_PACKET, .length = sizeof(setup), .id = 42 };
    record_conn_recv(&hdr, &setup, sizeof(setup), &ctx);

    /* 2. Device -> host control reply, 18-byte descriptor payload (send side). */
    uint8_t reply[sizeof(setup) + 18];
    memcpy(reply, &setup, sizeof(setup));
    memset(reply + sizeof(setup), 0xCC, 18);
    record_conn_send(RT_CONTROL_PACKET, 42, reply, sizeof(reply), &ctx);

    /* 3. Device -> host interrupt report, ep 0x81 (send side). */
    usb_interrupt_packet_header_t ih_in = { .endpoint = 0x01, .status = 0, .length = 4 };
    uint8_t int_in[sizeof(ih_in) + 4];
    memcpy(int_in, &ih_in, sizeof(ih_in));
    memset(int_in + sizeof(ih_in), 0x11, 4);
    record_conn_send(RT_INTERRUPT_PACKET, 7, int_in, sizeof(int_in), &ctx);

    /* 4. Host -> device interrupt OUT, ep 0x01 (recv side, e.g. keyboard LEDs). */
    usb_interrupt_packet_header_t ih_out = { .endpoint = 0x01, .status = 0, .length = 1 };
    uint8_t int_out[sizeof(ih_out) + 1];
    memcpy(int_out, &ih_out, sizeof(ih_out));
    int_out[sizeof(ih_out)] = 0x02;
    usb_header_t hdr_out = { .type = RT_INTERRUPT_PACKET, .length = sizeof(int_out), .id = 7 };
    record_conn_recv(&hdr_out, int_out, sizeof(int_out), &ctx);

    record_close();

    decoded_epb_t epbs[8];
    size_t n = read_epbs(path, epbs, 8);
    ck_assert_uint_eq(n, 4);

    ck_assert_int_eq(epbs[0].hdr.type, 'S');
    ck_assert_uint_eq(epbs[0].hdr.xfer_type, PCAP_XFER_CONTROL);
    ck_assert_int_eq(epbs[0].hdr.id, 42);

    ck_assert_int_eq(epbs[1].hdr.type, 'C');
    ck_assert_uint_eq(epbs[1].payload_len, 18);
    for (size_t i = 0; i < 18; i++)
        ck_assert_uint_eq(epbs[1].payload[i], 0xCC);

    ck_assert_int_eq(epbs[2].hdr.type, 'C');
    ck_assert_uint_eq(epbs[2].hdr.xfer_type, PCAP_XFER_INT);
    ck_assert_uint_eq(epbs[2].hdr.epnum, 0x81);
    ck_assert_uint_eq(epbs[2].payload_len, 4);

    ck_assert_int_eq(epbs[3].hdr.type, 'S');
    ck_assert_uint_eq(epbs[3].hdr.xfer_type, PCAP_XFER_INT);
    ck_assert_uint_eq(epbs[3].hdr.epnum, 0x01);
    ck_assert_uint_eq(epbs[3].payload_len, 1);
    ck_assert_uint_eq(epbs[3].payload[0], 0x02);

    remove(path);
}
END_TEST

START_TEST(test_record_skips_without_ctx)
{
    const char *path = "/tmp/usbvcr_test_record_noctx.pcapng";
    remove(path);
    ck_assert_int_eq(record_open(path), 0);

    uint8_t dd[18] = {0};
    record_device_announce(dd, sizeof(dd), dd, sizeof(dd), NULL);

    usb_control_packet_header_t setup = {
        .endpoint = 0, .request = USB_REQ_GET_DESCRIPTOR,
        .requesttype = USB_DIR_IN, .value = (USB_DESC_DEVICE << 8),
        .index = 0, .length = 18,
    };
    usb_header_t hdr = { .type = RT_CONTROL_PACKET, .length = sizeof(setup), .id = 1 };
    record_conn_recv(&hdr, &setup, sizeof(setup), NULL);
    record_conn_send(RT_CONTROL_PACKET, 1, &setup, sizeof(setup), NULL);

    record_close();

    decoded_epb_t epbs[4];
    size_t n = read_epbs(path, epbs, 4);
    ck_assert_uint_eq(n, 0);

    remove(path);
}
END_TEST

static Suite *record_suite(void)
{
    Suite *s = suite_create("record");
    TCase *tc = tcase_create("core");

    tcase_add_test(tc, test_record_device_announce_writes_four_packets);
    tcase_add_test(tc, test_record_conn_send_recv_writes_packets);
    tcase_add_test(tc, test_record_skips_without_ctx);

    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = record_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
