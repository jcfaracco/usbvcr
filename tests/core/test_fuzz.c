/* Unit tests for src/fuzz/fuzz.c */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <check.h>

#include "fuzz.h"
#include "bytes.h"

START_TEST(test_fuzz_rng_deterministic)
{
    fuzz_rng_t r1;
    fuzz_rng_t r2;

    fuzz_rng_seed(&r1, 123456789ULL);
    fuzz_rng_seed(&r2, 123456789ULL);

    for (int i = 0; i < 50; i++) {
        ck_assert_uint_eq(fuzz_rng_next(&r1), fuzz_rng_next(&r2));
        ck_assert_uint_eq(fuzz_rng_below(&r1, 100), fuzz_rng_below(&r2, 100));
    }
}
END_TEST

START_TEST(test_fuzz_single_config)
{
    /* Standard 34-byte HID config descriptor */
    const uint8_t orig_cfg[34] = {
        0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xA0,
        0x32, 0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01,
        0x01, 0x00, 0x09, 0x21, 0x11, 0x01, 0x00, 0x01,
        0x22, 0x3F, 0x00, 0x07, 0x05, 0x81, 0x03, 0x08,
        0x00, 0x0A,
    };
    uint8_t buf[128];
    memcpy(buf, orig_cfg, sizeof(orig_cfg));

    fuzz_rng_t r;
    fuzz_rng_seed(&r, 42);

    fuzz_cfg_t cfg = {
        .seed = 42,
        .prob = 1, /* always mutate */
        .fuzz_descriptors = 1,
    };

    size_t new_len = fuzz_mutate_descriptor(&r, &cfg, buf, sizeof(orig_cfg), sizeof(buf));
    ck_assert_int_gt((int)new_len, 0);
}
END_TEST

START_TEST(test_fuzz_multi_config_boundaries)
{
    /* Device Descriptor (18 bytes) with bNumConfigurations = 2 at offset 17 */
    const uint8_t dev_desc[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08,
        0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x02, /* bNumConfigurations = 2 */
    };

    /* Config 1 (wTotalLength = 25): 9 (cfg) + 9 (iface) + 7 (ep) */
    const uint8_t cfg1[25] = {
        0x09, 0x02, 0x19, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
        0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,
    };

    /* Config 2 (wTotalLength = 25): 9 (cfg) + 9 (iface) + 7 (ep) */
    const uint8_t cfg2[25] = {
        0x09, 0x02, 0x19, 0x00, 0x01, 0x02, 0x00, 0x80, 0x64,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x08, 0x06, 0x50, 0x00,
        0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,
    };

    uint8_t full_corpus[18 + 25 + 25];
    memcpy(full_corpus, dev_desc, 18);
    memcpy(full_corpus + 18, cfg1, 25);
    memcpy(full_corpus + 18 + 25, cfg2, 25);

    uint8_t buf[256];
    memcpy(buf, full_corpus, sizeof(full_corpus));

    fuzz_rng_t r;
    fuzz_rng_seed(&r, 9999);

    fuzz_cfg_t cfg = {
        .seed = 9999,
        .prob = 2,
        .fuzz_descriptors = 1,
    };

    size_t new_len = fuzz_mutate_descriptor(&r, &cfg, buf, sizeof(full_corpus), sizeof(buf));
    ck_assert_int_gt((int)new_len, 0);
}
END_TEST

START_TEST(test_fuzz_mutate_bytes)
{
    uint8_t payload[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    uint8_t copy[16];
    memcpy(copy, payload, sizeof(payload));

    fuzz_rng_t r;
    fuzz_rng_seed(&r, 1);

    fuzz_cfg_t cfg = {
        .seed = 1,
        .prob = 1,
    };

    size_t len = fuzz_mutate_bytes(&r, &cfg, copy, sizeof(copy), sizeof(copy));
    ck_assert_int_eq((int)len, 16);
    ck_assert_int_ne(memcmp(payload, copy, sizeof(payload)), 0);
}
END_TEST

static Suite *fuzz_suite(void)
{
    Suite *s = suite_create("fuzz");
    TCase *tc = tcase_create("core");

    tcase_add_test(tc, test_fuzz_rng_deterministic);
    tcase_add_test(tc, test_fuzz_single_config);
    tcase_add_test(tc, test_fuzz_multi_config_boundaries);
    tcase_add_test(tc, test_fuzz_mutate_bytes);

    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = fuzz_suite();
    SRunner *sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    int number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
