/*
 * Integration test: pcap/pcapng module round-trip.
 *
 * pcap_writer_* and pcap_trace_load()/pcap_trace_get() are the write and
 * read halves of the same usbmon framing (src/lib/pcap/pcap.c). This test
 * exercises the round-trip encoding and decoding, error branches, and edge cases.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <check.h>
#include <glib.h>

#include "pcap/pcap.h"

#define N_VECTORS 6

static FILE *s_mock_target_fp = NULL;
static int s_mock_track_fopen = 0;
static int s_mock_fwrite_fail_countdown = -1;
static int s_mock_fflush_fail = 0;
static int s_mock_fseek_fail = 0;

static inline FILE *mock_fopen(const char *path, const char *mode)
{
    FILE *fp = fopen(path, mode);
    if (s_mock_track_fopen)
    {
        s_mock_target_fp = fp;
    }
    return fp;
}
#define fopen mock_fopen

static inline size_t mock_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (s_mock_target_fp != NULL && stream == s_mock_target_fp)
    {
        if (s_mock_fwrite_fail_countdown >= 0)
        {
            if (s_mock_fwrite_fail_countdown == 0)
            {
                errno = EIO;
                return 0;
            }
            s_mock_fwrite_fail_countdown--;
        }
    }
    return fwrite(ptr, size, nmemb, stream);
}
#define fwrite mock_fwrite

static inline int mock_fflush(FILE *stream)
{
    if (s_mock_target_fp != NULL && stream == s_mock_target_fp)
    {
        if (s_mock_fflush_fail)
        {
            errno = EIO;
            return EOF;
        }
    }
    return fflush(stream);
}
#define fflush mock_fflush

static inline int mock_fseek(FILE *stream, long offset, int whence)
{
    if (s_mock_target_fp != NULL && stream == s_mock_target_fp)
    {
        if (s_mock_fseek_fail)
        {
            errno = ESPIPE;
            return -1;
        }
    }
    return fseek(stream, offset, whence);
}
#define fseek mock_fseek

#include "pcap/pcap.c"

#undef fopen
#undef fwrite
#undef fflush
#undef fseek

typedef struct {
    pcap_packet_t pkt;
    uint8_t       data_buf[64];
} vector_t;

static void write_raw_file(const char *path, const void *buf, size_t len)
{
    FILE *fp = fopen(path, "wb");
    ck_assert_ptr_nonnull(fp);
    if (len > 0 && buf != NULL)
    {
        ck_assert_uint_eq(fwrite(buf, 1, len, fp), len);
    }
    fclose(fp);
}

static void init_valid_pcapng_headers(uint8_t *buf, size_t *out_len, uint16_t link_type)
{
    pcapng_shb_t shb = {
        .block_type = 0x0A0D0D0A,
        .block_len = sizeof(pcapng_shb_t) + sizeof(uint32_t),
        .byte_order_magic = 0x1A2B3C4D,
        .major_version = 1,
        .minor_version = 0,
        .section_len = -1
    };
    uint32_t shb_len = shb.block_len;
    pcapng_idb_t idb = {
        .block_type = 0x00000001,
        .block_len = sizeof(pcapng_idb_t) + sizeof(uint32_t),
        .link_type = link_type,
        .reserved = 0,
        .snaplen = 65535
    };
    uint32_t idb_len = idb.block_len;

    size_t off = 0;
    memcpy(buf + off, &shb, sizeof(shb));
    off += sizeof(shb);
    memcpy(buf + off, &shb_len, sizeof(shb_len));
    off += sizeof(shb_len);
    memcpy(buf + off, &idb, sizeof(idb));
    off += sizeof(idb);
    memcpy(buf + off, &idb_len, sizeof(idb_len));
    off += sizeof(idb_len);
    *out_len = off;
}

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
    {
        v[n].data_buf[i] = (uint8_t)(0xA0 + i);
    }
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
    {
        v[n].data_buf[i] = (uint8_t)i;
    }
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
    {
        pcap_writer_add(w, &v[i].pkt);
    }
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

    for (size_t i = 0; i < n; i++)
    {
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
        {
            ck_assert_mem_eq(got->setup, want->setup, sizeof(got->setup));
        }

        ck_assert_uint_eq(got->data_len, want->data_len);
        if (want->data_len > 0)
        {
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
    ck_assert_ptr_null(pcap_trace_load(NULL));
    pcap_trace_t *t = pcap_trace_load("/tmp/usbvcr_roundtrip_does_not_exist.pcapng");
    ck_assert_ptr_null(t);
}
END_TEST

/* ---- New Error & Boundary Tests ---- */

START_TEST(test_pcap_load_legacy_errors)
{
    const char *path = "/tmp/usbvcr_legacy_err.pcap";
    pcap_header_t gh = {
        .magic_number = 0xa1b2c3d4,
        .version_major = 2,
        .version_minor = 4,
        .snaplen = 65535,
        .network = 220
    };
    pcap_rec_header_t rh = { 0 };
    usbmon_header_t uh = { 0 };
    pcap_trace_t *t;

    /* 1. Truncated global header (< 24 bytes) -> lines 124-125 */
    write_raw_file(path, "short_hdr", 9);
    t = pcap_trace_load(path);
    ck_assert_ptr_null(t);

    /* 2. Unrecognized magic number -> lines 128-129 */
    pcap_header_t bad_gh = gh;
    bad_gh.magic_number = 0xdeadbeef;
    write_raw_file(path, &bad_gh, sizeof(bad_gh));
    t = pcap_trace_load(path);
    ck_assert_ptr_null(t);

    /* 3. Record smaller than usbmon header (rh.incl_len < 64) -> lines 99-100, 137 */
    rh.incl_len = 10;
    FILE *fp = fopen(path, "wb");
    ck_assert_ptr_nonnull(fp);
    fwrite(&gh, sizeof(gh), 1, fp);
    fwrite(&rh, sizeof(rh), 1, fp);
    fclose(fp);
    t = pcap_trace_load(path);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(pcap_trace_count(t), 0);
    pcap_trace_free(t);

    /* 4. Truncated usbmon header in record -> line 104 */
    rh.incl_len = sizeof(usbmon_header_t);
    fp = fopen(path, "wb");
    ck_assert_ptr_nonnull(fp);
    fwrite(&gh, sizeof(gh), 1, fp);
    fwrite(&rh, sizeof(rh), 1, fp);
    fwrite(&uh, 20, 1, fp); /* truncated usbmon header */
    fclose(fp);
    t = pcap_trace_load(path);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(pcap_trace_count(t), 0);
    pcap_trace_free(t);

    /* 5. Truncated payload in record -> line 110 */
    rh.incl_len = sizeof(usbmon_header_t) + 16;
    fp = fopen(path, "wb");
    ck_assert_ptr_nonnull(fp);
    fwrite(&gh, sizeof(gh), 1, fp);
    fwrite(&rh, sizeof(rh), 1, fp);
    fwrite(&uh, sizeof(uh), 1, fp);
    fwrite("short", 5, 1, fp); /* payload is only 5 bytes instead of 16 */
    fclose(fp);
    t = pcap_trace_load(path);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(pcap_trace_count(t), 0);
    pcap_trace_free(t);

    remove(path);
}
END_TEST

START_TEST(test_pcapng_load_errors)
{
    const char *path = "/tmp/usbvcr_ng_err.pcapng";
    uint8_t hdr_buf[256];
    size_t hdr_len = 0;
    pcap_trace_t *t;
    FILE *fp;

    /* 1. Missing or invalid SHB -> lines 149-150 */
    write_raw_file(path, "bad_shb", 7);
    t = pcap_trace_load(path);
    ck_assert_ptr_null(t);

    /* 2. Truncated SHB trailer -> lines 153-154 */
    pcapng_shb_t shb = {
        .block_type = 0x0A0D0D0A,
        .block_len = sizeof(pcapng_shb_t) + sizeof(uint32_t),
        .byte_order_magic = 0x1A2B3C4D,
        .major_version = 1,
        .minor_version = 0,
        .section_len = -1
    };
    write_raw_file(path, &shb, sizeof(shb));
    t = pcap_trace_load(path);
    ck_assert_ptr_null(t);

    /* 3. Missing/invalid IDB -> lines 159-160 */
    uint32_t shb_len = shb.block_len;
    fp = fopen(path, "wb");
    ck_assert_ptr_nonnull(fp);
    fwrite(&shb, sizeof(shb), 1, fp);
    fwrite(&shb_len, sizeof(shb_len), 1, fp);
    uint32_t bad_idb[2] = { 0x99999999, 32 };
    fwrite(bad_idb, sizeof(bad_idb), 1, fp);
    fclose(fp);
    t = pcap_trace_load(path);
    ck_assert_ptr_null(t);

    /* 4. Truncated IDB trailer -> lines 163-164 */
    pcapng_idb_t idb = {
        .block_type = 0x00000001,
        .block_len = sizeof(pcapng_idb_t) + sizeof(uint32_t),
        .link_type = 220,
        .reserved = 0,
        .snaplen = 65535
    };
    fp = fopen(path, "wb");
    ck_assert_ptr_nonnull(fp);
    fwrite(&shb, sizeof(shb), 1, fp);
    fwrite(&shb_len, sizeof(shb_len), 1, fp);
    fwrite(&idb, sizeof(idb), 1, fp);
    fclose(fp);
    t = pcap_trace_load(path);
    ck_assert_ptr_null(t);

    /* 5. Unsupported link type (e.g. 1 instead of 189/220) -> lines 167-168 */
    init_valid_pcapng_headers(hdr_buf, &hdr_len, 1);
    write_raw_file(path, hdr_buf, hdr_len);
    t = pcap_trace_load(path);
    ck_assert_ptr_null(t);

    /* Initialize valid SHB + IDB (link_type 220) */
    init_valid_pcapng_headers(hdr_buf, &hdr_len, 220);

    /* 6. Unexpected pcapng block type after IDB -> lines 176-177 */
    fp = fopen(path, "wb");
    ck_assert_ptr_nonnull(fp);
    fwrite(hdr_buf, hdr_len, 1, fp);
    pcapng_epb_t bad_epb = { 0 };
    bad_epb.block_type = 0x00000005; /* unexpected block type (not 0x06) */
    bad_epb.block_len = sizeof(bad_epb);
    fwrite(&bad_epb, sizeof(bad_epb), 1, fp);
    fclose(fp);
    t = pcap_trace_load(path);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(pcap_trace_count(t), 0);
    pcap_trace_free(t);

    /* 7. EPB with cap_packet_len < sizeof(usbmon_header_t) -> line 181 */
    pcapng_epb_t epb = {
        .block_type = 0x00000006,
        .block_len = sizeof(pcapng_epb_t) + sizeof(uint32_t) + 32,
        .cap_packet_len = 10,
        .orig_packet_len = 10
    };
    fp = fopen(path, "wb");
    ck_assert_ptr_nonnull(fp);
    fwrite(hdr_buf, hdr_len, 1, fp);
    fwrite(&epb, sizeof(epb), 1, fp);
    fclose(fp);
    t = pcap_trace_load(path);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(pcap_trace_count(t), 0);
    pcap_trace_free(t);

    /* 8. EPB with padding fseek failure -> line 185 */
    epb.cap_packet_len = sizeof(usbmon_header_t) + 1; /* unpadded = 65 -> padding needed = 3 */
    epb.block_len = sizeof(pcapng_epb_t) + 65 + 3 + sizeof(uint32_t);
    usbmon_header_t uh = { 0 };
    fp = fopen(path, "wb");
    ck_assert_ptr_nonnull(fp);
    fwrite(hdr_buf, hdr_len, 1, fp);
    fwrite(&epb, sizeof(epb), 1, fp);
    fwrite(&uh, sizeof(uh), 1, fp);
    fwrite("x", 1, 1, fp);
    fwrite("\0\0\0", 3, 1, fp);
    uint32_t epb_trailer = epb.block_len;
    fwrite(&epb_trailer, sizeof(epb_trailer), 1, fp);
    fclose(fp);

    s_mock_track_fopen = 1;
    s_mock_fseek_fail = 1;
    t = pcap_trace_load(path);
    s_mock_fseek_fail = 0;
    s_mock_track_fopen = 0;
    s_mock_target_fp = NULL;
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(pcap_trace_count(t), 1);
    pcap_trace_free(t);

    /* 9. EPB with malformed trailer -> lines 188-189 */
    fp = fopen(path, "wb");
    ck_assert_ptr_nonnull(fp);
    fwrite(hdr_buf, hdr_len, 1, fp);
    fwrite(&epb, sizeof(epb), 1, fp);
    fwrite(&uh, sizeof(uh), 1, fp);
    fwrite("x", 1, 1, fp);
    fwrite("\0\0\0", 3, 1, fp); /* 3 bytes padding */
    uint32_t bad_trailer = 0x12345678; /* trailer != epb.block_len */
    fwrite(&bad_trailer, sizeof(bad_trailer), 1, fp);
    fclose(fp);
    t = pcap_trace_load(path);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(pcap_trace_count(t), 1);
    pcap_trace_free(t);

    remove(path);
}
END_TEST

START_TEST(test_pcap_format_and_helpers)
{
    /* pcap_trace_free NULL safety and trace helper NULL checks */
    pcap_trace_free(NULL);
    ck_assert_uint_eq(pcap_trace_count(NULL), 0);
    ck_assert_ptr_null(pcap_trace_get(NULL, 0));

    pcap_trace_t *dummy_t = g_new0(pcap_trace_t, 1);
    ck_assert_uint_eq(pcap_trace_count(dummy_t), 0);
    ck_assert_ptr_null(pcap_trace_get(dummy_t, 0));
    pcap_trace_free(dummy_t);

    /* pcap_format_get edge cases -> lines 239, 246 */
    ck_assert_int_eq(pcap_format_get(NULL), FORMAT_LEGACY_PCAP);
    ck_assert_int_eq(pcap_format_get(""), FORMAT_LEGACY_PCAP);
    ck_assert_int_eq(pcap_format_get("short"), FORMAT_LEGACY_PCAP);
    ck_assert_int_eq(pcap_format_get("capture.pcap"), FORMAT_LEGACY_PCAP);
    ck_assert_int_eq(pcap_format_get("capture.pcapng"), FORMAT_PCAPNG);

    /* Test writer packets with event_type 'E', event_type 0, and control wLength */
    const char *path = "/tmp/usbvcr_helpers.pcap";
    remove(path);
    pcap_writer_t *w = pcap_writer_open(path);
    ck_assert_ptr_nonnull(w);

    pcap_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.ts_usec = 1000;
    pkt.urb_id = 1;

    /* Writer with NULL fp */
    pcap_writer_t *dummy_w = g_new0(pcap_writer_t, 1);
    ck_assert_int_eq(pcap_writer_add(dummy_w, &pkt), -1);
    pcap_writer_close(dummy_w);
    pkt.dir = PCAP_DIR_IN;
    pkt.event_type = 'E'; /* line 281: 'E' */
    ck_assert_int_eq(pcap_writer_add(w, &pkt), 0);

    /* Event type 0 with PCAP_DIR_IN -> line 283: 'C' */
    pkt.event_type = 0;
    pkt.dir = PCAP_DIR_IN;
    ck_assert_int_eq(pcap_writer_add(w, &pkt), 0);

    /* Event type 0 with PCAP_DIR_OUT -> line 283: 'S' */
    pkt.dir = PCAP_DIR_OUT;
    ck_assert_int_eq(pcap_writer_add(w, &pkt), 0);

    /* Control Submit with wLength > data_len -> lines 308-310 */
    pkt.xfer = PCAP_XFER_CONTROL;
    pkt.setup_valid = 1;
    pkt.event_type = 'S';
    pkt.setup[6] = 50; /* wLength = 50 */
    pkt.setup[7] = 0;
    pkt.data_len = 8;
    uint8_t dummy_data[8] = { 0 };
    pkt.data = dummy_data;
    ck_assert_int_eq(pcap_writer_add(w, &pkt), 0);

    pcap_writer_close(w);
    remove(path);
}
END_TEST

START_TEST(test_pcap_writer_open_failures)
{
    pcap_writer_t *w;

    /* NULL path -> lines 388-390 */
    ck_assert_ptr_null(pcap_writer_open(NULL));

    /* 1. Invalid path (directory does not exist) -> lines 391-396 */
    w = pcap_writer_open("/nonexistent_dir_12345/test.pcap");
    ck_assert_ptr_null(w);

    /* 2. Legacy header fwrite failure -> lines 340-341, 405-409 */
    s_mock_track_fopen = 1;
    s_mock_fwrite_fail_countdown = 0;
    w = pcap_writer_open("/tmp/fail_legacy.pcap");
    ck_assert_ptr_null(w);
    s_mock_fwrite_fail_countdown = -1;
    s_mock_track_fopen = 0;
    s_mock_target_fp = NULL;
    remove("/tmp/fail_legacy.pcap");

    /* 3. PCAPNG SHB fwrite failure (first or second fwrite) -> lines 360-361, 405-409 */
    s_mock_track_fopen = 1;
    s_mock_fwrite_fail_countdown = 0;
    w = pcap_writer_open("/tmp/fail_shb.pcapng");
    ck_assert_ptr_null(w);
    s_mock_fwrite_fail_countdown = 1;
    w = pcap_writer_open("/tmp/fail_shb.pcapng");
    ck_assert_ptr_null(w);
    s_mock_fwrite_fail_countdown = -1;
    s_mock_track_fopen = 0;
    s_mock_target_fp = NULL;
    remove("/tmp/fail_shb.pcapng");

    /* 4. PCAPNG IDB fwrite failure -> lines 376-377, 405-409 */
    s_mock_track_fopen = 1;
    s_mock_fwrite_fail_countdown = 2;
    w = pcap_writer_open("/tmp/fail_idb.pcapng");
    ck_assert_ptr_null(w);
    s_mock_fwrite_fail_countdown = 3;
    w = pcap_writer_open("/tmp/fail_idb.pcapng");
    ck_assert_ptr_null(w);
    s_mock_fwrite_fail_countdown = -1;
    s_mock_track_fopen = 0;
    s_mock_target_fp = NULL;
    remove("/tmp/fail_idb.pcapng");

    /* 5. Header fflush failure -> lines 404-409 */
    s_mock_track_fopen = 1;
    s_mock_fflush_fail = 1;
    w = pcap_writer_open("/tmp/fail_fflush.pcap");
    ck_assert_ptr_null(w);
    s_mock_fflush_fail = 0;
    s_mock_track_fopen = 0;
    s_mock_target_fp = NULL;
    remove("/tmp/fail_fflush.pcap");
}
END_TEST

START_TEST(test_pcap_writer_add_and_close_errors)
{
    /* pcap_writer_close NULL safety -> line 541 */
    pcap_writer_close(NULL);

    pcap_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.data_len = 1;
    uint8_t byte = 0xAA;
    pkt.data = &byte;

    /* pcap_writer_add NULL parameter checks -> line 513 */
    ck_assert_int_eq(pcap_writer_add(NULL, &pkt), -1);

    const char *pcap_path = "/tmp/usbvcr_writer_err.pcap";
    remove(pcap_path);
    pcap_writer_t *w_legacy = pcap_writer_open(pcap_path);
    ck_assert_ptr_nonnull(w_legacy);

    ck_assert_int_eq(pcap_writer_add(w_legacy, NULL), -1);

    /* Test invalid format -> line 527 */
    w_legacy->format = (pcap_format_t)999;
    ck_assert_int_eq(pcap_writer_add(w_legacy, &pkt), -1);
    w_legacy->format = FORMAT_LEGACY_PCAP;

    /* Legacy write failures:
     * - record_hdr (countdown = 0) -> lines 430-431
     * - usb_hdr (countdown = 1) -> lines 436-437
     * - payload (countdown = 2) -> lines 441-442
     */
    s_mock_target_fp = w_legacy->fp;
    s_mock_fwrite_fail_countdown = 0;
    ck_assert_int_eq(pcap_writer_add(w_legacy, &pkt), -1);

    s_mock_fwrite_fail_countdown = 1;
    ck_assert_int_eq(pcap_writer_add(w_legacy, &pkt), -1);

    s_mock_fwrite_fail_countdown = 2;
    ck_assert_int_eq(pcap_writer_add(w_legacy, &pkt), -1);

    /* Legacy fflush failure -> lines 531-532 */
    s_mock_fwrite_fail_countdown = -1;
    s_mock_fflush_fail = 1;
    ck_assert_int_eq(pcap_writer_add(w_legacy, &pkt), -1);
    s_mock_fflush_fail = 0;
    s_mock_target_fp = NULL;

    pcap_writer_close(w_legacy);
    remove(pcap_path);

    /* PCAPNG write failures:
     * - epb (countdown = 0) -> lines 473-474
     * - usb_hdr (countdown = 1) -> lines 479-480
     * - payload (countdown = 2) -> lines 485-486
     * - padding (countdown = 3) -> lines 263, 492-493
     * - trailing length (countdown = 4) -> lines 498-499
     */
    const char *ng_path = "/tmp/usbvcr_writer_err.pcapng";
    remove(ng_path);
    pcap_writer_t *w_ng = pcap_writer_open(ng_path);
    ck_assert_ptr_nonnull(w_ng);

    s_mock_target_fp = w_ng->fp;
    s_mock_fwrite_fail_countdown = 0;
    ck_assert_int_eq(pcap_writer_add(w_ng, &pkt), -1);

    s_mock_fwrite_fail_countdown = 1;
    ck_assert_int_eq(pcap_writer_add(w_ng, &pkt), -1);

    s_mock_fwrite_fail_countdown = 2;
    ck_assert_int_eq(pcap_writer_add(w_ng, &pkt), -1);

    s_mock_fwrite_fail_countdown = 3;
    ck_assert_int_eq(pcap_writer_add(w_ng, &pkt), -1);

    s_mock_fwrite_fail_countdown = 4;
    ck_assert_int_eq(pcap_writer_add(w_ng, &pkt), -1);

    s_mock_fwrite_fail_countdown = -1;
    s_mock_target_fp = NULL;
    pcap_writer_close(w_ng);
    remove(ng_path);
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
    tcase_add_test(tc, test_pcap_load_legacy_errors);
    tcase_add_test(tc, test_pcapng_load_errors);
    tcase_add_test(tc, test_pcap_format_and_helpers);
    tcase_add_test(tc, test_pcap_writer_open_failures);
    tcase_add_test(tc, test_pcap_writer_add_and_close_errors);

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
