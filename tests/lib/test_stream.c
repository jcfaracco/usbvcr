/* Unit tests for MJPEG parser and stream provider (src/lib/stream/). */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <check.h>
#include <glib.h>

#include "stream/mjpeg_parser.h"
#include "stream/stream_provider.h"

START_TEST(test_mjpeg_parser_empty_or_null)
{
    ck_assert_ptr_null(mjpeg_stream_open(NULL));
    ck_assert_ptr_null(mjpeg_stream_open("/nonexistent/file.mjpeg"));
    ck_assert_ptr_null(mjpeg_stream_new_from_bytes(NULL, 0));

    size_t len = 99;
    ck_assert_ptr_null(mjpeg_stream_get_frame(NULL, 0, &len));
    ck_assert_int_eq(len, 0);

    mjpeg_stream_free(NULL);
}
END_TEST

START_TEST(test_mjpeg_parser_single_and_multi_frame)
{
    /* Construct 2 synthetic JPEG frames:
     * Frame 1: FF D8 01 02 03 FF D9 (7 bytes)
     * Frame 2: FF D8 AA BB CC DD EE FF D9 (9 bytes)
     */
    static const uint8_t data[] = {
        0xFF, 0xD8, 0x01, 0x02, 0x03, 0xFF, 0xD9,
        0xFF, 0xD8, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0xD9
    };

    mjpeg_stream_t *s = mjpeg_stream_new_from_bytes(data, sizeof(data));
    ck_assert_ptr_nonnull(s);
    ck_assert_int_eq(s->frame_count, 2);

    size_t f1_len = 0;
    const uint8_t *f1 = mjpeg_stream_get_frame(s, 0, &f1_len);
    ck_assert_ptr_nonnull(f1);
    ck_assert_int_eq(f1_len, 7);
    ck_assert_int_eq(f1[0], 0xFF);
    ck_assert_int_eq(f1[1], 0xD8);
    ck_assert_int_eq(f1[5], 0xFF);
    ck_assert_int_eq(f1[6], 0xD9);

    size_t f2_len = 0;
    const uint8_t *f2 = mjpeg_stream_get_frame(s, 1, &f2_len);
    ck_assert_ptr_nonnull(f2);
    ck_assert_int_eq(f2_len, 9);
    ck_assert_int_eq(f2[0], 0xFF);
    ck_assert_int_eq(f2[1], 0xD8);
    ck_assert_int_eq(f2[7], 0xFF);
    ck_assert_int_eq(f2[8], 0xD9);

    /* Out of bounds frame */
    size_t oob_len = 99;
    ck_assert_ptr_null(mjpeg_stream_get_frame(s, 2, &oob_len));
    ck_assert_int_eq(oob_len, 0);

    mjpeg_stream_free(s);
}
END_TEST

START_TEST(test_stream_provider_slicing_and_headers)
{
    /* 1 frame of 10 bytes: FF D8 01 02 03 04 05 06 FF D9 */
    static const uint8_t data[] = {
        0xFF, 0xD8, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xFF, 0xD9
    };

    mjpeg_stream_t *s = mjpeg_stream_new_from_bytes(data, sizeof(data));
    stream_provider_t *p = stream_provider_new_from_stream(s, FALSE, 0x81);
    ck_assert_ptr_nonnull(p);

    uint8_t pkt[8]; /* Capacity 8: 2 header bytes + up to 6 payload bytes */

    /* Packet 1: 6 payload bytes (offset 0..5), EOF = 0, FID = 0 */
    int n1 = stream_provider_produce_packet(p, 0x81, pkt, sizeof(pkt));
    ck_assert_int_eq(n1, 8);
    ck_assert_int_eq(pkt[0], 0x02); /* bHeaderLength */
    ck_assert_int_eq(pkt[1], 0x00); /* FID=0, EOF=0 */
    ck_assert_int_eq(pkt[2], 0xFF);
    ck_assert_int_eq(pkt[3], 0xD8);
    ck_assert_int_eq(pkt[7], 0x04);

    /* Wrong endpoint -> should return -1 */
    ck_assert_int_eq(stream_provider_produce_packet(p, 0x82, pkt, sizeof(pkt)), -1);

    /* Packet 2: remaining 4 payload bytes (offset 6..9), EOF = 1, FID = 0 */
    int n2 = stream_provider_produce_packet(p, 0x81, pkt, sizeof(pkt));
    ck_assert_int_eq(n2, 6);        /* 2 header + 4 payload */
    ck_assert_int_eq(pkt[0], 0x02);
    ck_assert_int_eq(pkt[1], 0x02); /* FID=0, EOF=1 (bit 1) */
    ck_assert_int_eq(pkt[2], 0x05);
    ck_assert_int_eq(pkt[5], 0xD9);

    /* No loop: stream should be inactive now */
    ck_assert_int_eq(stream_provider_produce_packet(p, 0x81, pkt, sizeof(pkt)), 0);

    stream_provider_free(p);
}
END_TEST

START_TEST(test_stream_provider_looping)
{
    /* 2 frames: 4 bytes each */
    static const uint8_t data[] = {
        0xFF, 0xD8, 0xFF, 0xD9,
        0xFF, 0xD8, 0xFF, 0xD9
    };

    mjpeg_stream_t *s = mjpeg_stream_new_from_bytes(data, sizeof(data));
    stream_provider_t *p = stream_provider_new_from_stream(s, TRUE, 0);
    ck_assert_ptr_nonnull(p);

    uint8_t pkt[16];

    /* Frame 1 (entire frame fits): FID=0, EOF=1 */
    int n1 = stream_provider_produce_packet(p, 0x81, pkt, sizeof(pkt));
    ck_assert_int_eq(n1, 6); /* 2 header + 4 data */
    ck_assert_int_eq(pkt[1], 0x02); /* FID=0, EOF=1 */

    /* Frame 2: FID should toggle to 1, EOF=1 */
    int n2 = stream_provider_produce_packet(p, 0x81, pkt, sizeof(pkt));
    ck_assert_int_eq(n2, 6);
    ck_assert_int_eq(pkt[1], 0x03); /* FID=1, EOF=1 */

    /* Loop wrap-around to Frame 1: FID should toggle to 0, EOF=1 */
    int n3 = stream_provider_produce_packet(p, 0x81, pkt, sizeof(pkt));
    ck_assert_int_eq(n3, 6);
    ck_assert_int_eq(pkt[1], 0x02); /* FID=0, EOF=1 */

    stream_provider_free(p);
}
END_TEST

static Suite *stream_suite(void)
{
    Suite *s = suite_create("stream");
    TCase *tc = tcase_create("core");

    tcase_add_test(tc, test_mjpeg_parser_empty_or_null);
    tcase_add_test(tc, test_mjpeg_parser_single_and_multi_frame);
    tcase_add_test(tc, test_stream_provider_slicing_and_headers);
    tcase_add_test(tc, test_stream_provider_looping);

    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(stream_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
