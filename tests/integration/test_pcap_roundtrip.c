/*
 * Integration test: pcap/pcapng module round-trip.
 *
 * pcap_writer_* and pcap_trace_load()/pcap_trace_get() are the write and
 * read halves of the same usbmon framing (src/lib/pcap/pcap.c). This test
 * writes a batch of representative transfers (control SETUP/COMPLETE with a
 * SETUP packet, interrupt IN/OUT, bulk with and without a payload) through
 * the writer, reads them back through the reader, and asserts every field
 * survives byte-for-byte - in both supported containers (classic pcap and
 * pcapng, selected by file extension).
 *
 * This only exercises the pcap module itself (see tests/meson.build: this
 * binary links pcap.c alone). The recording round-trip through
 * pcap_backend itself - record a session, replay it, and assert the
 * backend reproduces the exact payload bytes - lives in
 * tests/integration/test_pcap_backend.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <check.h>

#include "pcap/pcap.h"

#define N_VECTORS 6

typedef struct {
    pcap_packet_t pkt;
    uint8_t       data_buf[64];
} vector_t;

/* Build the fixed set of transfers shared by every round-trip variant. */
static size_t build_vectors(vector_t *v)
{
    size_t n = 0;
    uint8_t get_device_desc_setup[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00 };

    /* 0: control SETUP, ep0 IN, no payload yet. */
    memset(&v[n], 0, sizeof(v[n]));
    v[n].pkt.ts_usec     = 1696500000123456ULL;
    v[n].pkt.urb_id      = 42;
    v[n].pkt.bus         = 2;
    v[n].pkt.device      = 5;
    v[n].pkt.endpoint    = 0x80;
    v[n].pkt.dir         = PCAP_DIR_IN;
    v[n].pkt.xfer        = PCAP_XFER_CONTROL;
    v[n].pkt.event_type  = 'S';
    v[n].pkt.setup_valid = 1;
    memcpy(v[n].pkt.setup, get_device_desc_setup, 8);
    n++;

    /* 1: control COMPLETE, same URB, carrying an 18-byte device descriptor. */
    memset(&v[n], 0, sizeof(v[n]));
    v[n].pkt.ts_usec     = 1696500000124000ULL;
    v[n].pkt.urb_id      = 42;
    v[n].pkt.bus         = 2;
    v[n].pkt.device      = 5;
    v[n].pkt.endpoint    = 0x80;
    v[n].pkt.dir         = PCAP_DIR_IN;
    v[n].pkt.xfer        = PCAP_XFER_CONTROL;
    v[n].pkt.event_type  = 'C';
    v[n].pkt.setup_valid = 1;
    memcpy(v[n].pkt.setup, get_device_desc_setup, 8);
    for (int i = 0; i < 18; i++)
        v[n].data_buf[i] = (uint8_t)(0xA0 + i);
    v[n].pkt.data     = v[n].data_buf;
    v[n].pkt.data_len = 18;
    n++;

    /* 2: interrupt IN complete, ep 0x81, 4-byte HID report. */
    memset(&v[n], 0, sizeof(v[n]));
    v[n].pkt.ts_usec    = 1696500000200000ULL;
    v[n].pkt.urb_id     = 7;
    v[n].pkt.bus        = 2;
    v[n].pkt.device     = 5;
    v[n].pkt.endpoint   = 0x81;
    v[n].pkt.dir        = PCAP_DIR_IN;
    v[n].pkt.xfer       = PCAP_XFER_INT;
    v[n].pkt.event_type = 'C';
    memcpy(v[n].data_buf, "\x00\x04\x00\x00", 4);
    v[n].pkt.data     = v[n].data_buf;
    v[n].pkt.data_len = 4;
    n++;

    /* 3: interrupt OUT submit, ep 0x01, 1-byte LED state (no setup, no data
     * past the single byte). */
    memset(&v[n], 0, sizeof(v[n]));
    v[n].pkt.ts_usec    = 1696500000300000ULL;
    v[n].pkt.urb_id     = 8;
    v[n].pkt.bus        = 2;
    v[n].pkt.device     = 5;
    v[n].pkt.endpoint   = 0x01;
    v[n].pkt.dir        = PCAP_DIR_OUT;
    v[n].pkt.xfer       = PCAP_XFER_INT;
    v[n].pkt.event_type = 'S';
    v[n].data_buf[0]    = 0x02;
    v[n].pkt.data       = v[n].data_buf;
    v[n].pkt.data_len   = 1;
    n++;

    /* 4: bulk OUT submit, ep 0x02, 32-byte payload. */
    memset(&v[n], 0, sizeof(v[n]));
    v[n].pkt.ts_usec    = 1696500000400000ULL;
    v[n].pkt.urb_id     = 9;
    v[n].pkt.bus        = 3;
    v[n].pkt.device     = 1;
    v[n].pkt.endpoint   = 0x02;
    v[n].pkt.dir        = PCAP_DIR_OUT;
    v[n].pkt.xfer       = PCAP_XFER_BULK;
    v[n].pkt.event_type = 'S';
    for (int i = 0; i < 32; i++)
        v[n].data_buf[i] = (uint8_t)i;
    v[n].pkt.data     = v[n].data_buf;
    v[n].pkt.data_len = 32;
    n++;

    /* 5: bulk IN complete, ep 0x82, zero-length (e.g. a short/ZLP transfer). */
    memset(&v[n], 0, sizeof(v[n]));
    v[n].pkt.ts_usec    = 1696500000500000ULL;
    v[n].pkt.urb_id     = 10;
    v[n].pkt.bus        = 3;
    v[n].pkt.device     = 1;
    v[n].pkt.endpoint   = 0x82;
    v[n].pkt.dir        = PCAP_DIR_IN;
    v[n].pkt.xfer       = PCAP_XFER_BULK;
    v[n].pkt.event_type = 'C';
    n++;

    return n;
}

static void write_trace(const char *path, const vector_t *v, size_t n)
{
    remove(path);
    pcap_writer_t *w = pcap_writer_open(path);
    ck_assert_ptr_nonnull(w);
    for (size_t i = 0; i < n; i++)
        pcap_writer_add(w, &v[i].pkt);
    pcap_writer_close(w);
}

static void assert_roundtrip(const char *path)
{
    vector_t vectors[N_VECTORS];
    size_t n = build_vectors(vectors);

    write_trace(path, vectors, n);

    pcap_trace_t *t = pcap_trace_load(path);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(pcap_trace_count(t), n);

    for (size_t i = 0; i < n; i++) {
        const pcap_packet_t *got = pcap_trace_get(t, i);
        const pcap_packet_t *want = &vectors[i].pkt;

        ck_assert_ptr_nonnull(got);
        ck_assert_uint_eq(got->ts_usec, want->ts_usec);
        ck_assert_uint_eq(got->urb_id, want->urb_id);
        ck_assert_uint_eq(got->bus, want->bus);
        ck_assert_uint_eq(got->device, want->device);
        ck_assert_uint_eq(got->endpoint, want->endpoint);
        ck_assert_int_eq(got->dir, want->dir);
        ck_assert_int_eq(got->xfer, want->xfer);
        ck_assert_int_eq(got->event_type, want->event_type);
        ck_assert_int_eq(got->setup_valid, want->setup_valid);
        if (want->setup_valid)
            ck_assert_mem_eq(got->setup, want->setup, sizeof(got->setup));

        ck_assert_uint_eq(got->data_len, want->data_len);
        if (want->data_len > 0) {
            ck_assert_ptr_nonnull(got->data);
            ck_assert_mem_eq(got->data, want->data, want->data_len);
        }
    }

    /* Out of range lookups fail cleanly. */
    ck_assert_ptr_null(pcap_trace_get(t, n));

    pcap_trace_free(t);
    remove(path);
}

START_TEST(test_pcap_roundtrip_pcapng)
{
    assert_roundtrip("/tmp/usbvcr_roundtrip.pcapng");
}
END_TEST

START_TEST(test_pcap_roundtrip_legacy)
{
    assert_roundtrip("/tmp/usbvcr_roundtrip.pcap");
}
END_TEST

START_TEST(test_pcap_roundtrip_empty_trace)
{
    const char *path = "/tmp/usbvcr_roundtrip_empty.pcapng";
    remove(path);

    pcap_writer_t *w = pcap_writer_open(path);
    ck_assert_ptr_nonnull(w);
    pcap_writer_close(w);

    pcap_trace_t *t = pcap_trace_load(path);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(pcap_trace_count(t), 0);
    ck_assert_ptr_null(pcap_trace_get(t, 0));

    pcap_trace_free(t);
    remove(path);
}
END_TEST

START_TEST(test_pcap_trace_load_missing_file)
{
    pcap_trace_t *t = pcap_trace_load("/tmp/usbvcr_roundtrip_does_not_exist.pcapng");
    ck_assert_ptr_null(t);
}
END_TEST

static Suite *roundtrip_suite(void)
{
    Suite *s = suite_create("pcap_roundtrip");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, test_pcap_roundtrip_pcapng);
    tcase_add_test(tc, test_pcap_roundtrip_legacy);
    tcase_add_test(tc, test_pcap_roundtrip_empty_trace);
    tcase_add_test(tc, test_pcap_trace_load_missing_file);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = roundtrip_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
