/* Unit tests for generic table-driven encoder (src/lib/codec.c). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <check.h>
#include <glib.h>
#include <unistd.h>
#include <yaml.h>

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

/* Real libc/libyaml function for linker wrapping */
int __real_yaml_parser_initialize(yaml_parser_t *parser);

static int s_mock_parser_init_fail = 0;

int __wrap_yaml_parser_initialize(yaml_parser_t *parser)
{
    if (s_mock_parser_init_fail)
    {
        return 0;
    }
    return __real_yaml_parser_initialize(parser);
}

/* Expose static functions for unit testing */
#define static
#include "../../src/lib/codec.c"
#undef static

static char *write_temp_yaml(const char *content)
{
    g_autofree char *tmpl = g_strdup_printf("%s/usbvcr_codec_XXXXXX.yaml", g_get_tmp_dir());
    int fd = g_mkstemp(tmpl);
    ck_assert_int_ge(fd, 0);
    if (content != NULL && strlen(content) > 0)
    {
        ck_assert_int_eq(write(fd, content, strlen(content)), (ssize_t)strlen(content));
    }
    close(fd);
    return g_steal_pointer(&tmpl);
}

START_TEST(test_codec_load_valid)
{
    const char *path = TEST_DATA_DIR "/example1/codecs/hid_report.yaml";
    codec_t *c = codec_load(path);
    ck_assert_ptr_nonnull(c);
    ck_assert_str_eq(codec_name(c), "hid_report");

    codec_free(c);

    /* NULL safety checks */
    codec_free(NULL);
    ck_assert_ptr_null(codec_name(NULL));
}
END_TEST

START_TEST(test_codec_load_file_errors)
{
    /* 1. File does not exist -> lines 127-128 */
    codec_t *c = codec_load("/tmp/nonexistent_codec_table_12345.yaml");
    ck_assert_ptr_null(c);

    /* 2. File size exceeds CODEC_MAX_FILE_BYTES (1 MiB) -> lines 135-138 */
    g_autofree char *large_path = g_strdup_printf("%s/large_codec_XXXXXX.yaml", g_get_tmp_dir());
    int fd = g_mkstemp(large_path);
    ck_assert_int_ge(fd, 0);
    ck_assert_int_ge(lseek(fd, (off_t)CODEC_MAX_FILE_BYTES + 10, SEEK_SET), 0);
    ck_assert_int_eq(write(fd, "x", 1), 1);
    close(fd);

    c = codec_load(large_path);
    ck_assert_ptr_null(c);
    unlink(large_path);

    /* 3. Parser initialization failure -> lines 145-146 */
    s_mock_parser_init_fail = 1;
    c = codec_load(TEST_DATA_DIR "/example1/codecs/hid_report.yaml");
    ck_assert_ptr_null(c);
    s_mock_parser_init_fail = 0;

    /* 4. YAML syntax error -> lines 152-158 */
    g_autofree char *bad_yaml = write_temp_yaml("codec: [unclosed_list\n");
    c = codec_load(bad_yaml);
    ck_assert_ptr_null(c);
    unlink(bad_yaml);

    /* 5. Top-level node is not a mapping -> line 165 */
    g_autofree char *seq_yaml = write_temp_yaml("- item1\n- item2\n");
    c = codec_load(seq_yaml);
    ck_assert_ptr_null(c);
    unlink(seq_yaml);
}
END_TEST

START_TEST(test_codec_load_yaml_structure_branches)
{
    /* Tests parse_uint_map, parse_token, and parse_nested_maps non-mapping/non-scalar branches:
     * - item_types: not a mapping (line 52)
     * - item_types contains non-scalar key/value (line 59)
     * - tokens contains non-scalar key (line 188)
     * - token has unknown/missing type (line 77)
     * - symbols is not a mapping (line 112)
     * - symbols contains non-scalar key (line 118)
     * - flagsets is not a mapping (line 112)
     */
    const char *weird_yaml =
        "codec: \"weird\"\n"
        "item_types: { [1, 2]: 3, main: [4, 5], global: 1 }\n"
        "tokens:\n"
        "  [1, 2]: { type: global, tag: 0x1 }\n"
        "  tok_unknown: { type: unknown_type, tag: 0x2 }\n"
        "  tok_no_type: { tag: 0x3, width: 2 }\n"
        "  tok_auto: { type: global, tag: 0x4, width: auto, signed: true }\n"
        "  tok_signed_one: { type: global, signed: \"1\" }\n"
        "  tok_flag: { type: global, tag: 0x5, width: 1, flagset: fs1 }\n"
        "symbols: { [1, 2]: { a: 1 }, tok_unknown: { sym1: 10 } }\n"
        "flagsets: { fs1: { FlagA: 1, FlagB: 2 } }\n";

    g_autofree char *path = write_temp_yaml(weird_yaml);
    codec_t *c = codec_load(path);
    ck_assert_ptr_nonnull(c);
    ck_assert_str_eq(codec_name(c), "weird");
    codec_free(c);
    unlink(path);

    /* Test table with missing codec: field */
    const char *no_name_yaml = "tokens: { a: { tag: 1 } }\n";
    path = write_temp_yaml(no_name_yaml);
    c = codec_load(path);
    ck_assert_ptr_nonnull(c);
    ck_assert_str_eq(codec_name(c), "");
    codec_free(c);
    unlink(path);

    /* Test symbols/flagsets as non-mapping scalars */
    const char *scalar_sections_yaml =
        "codec: \"scalars\"\n"
        "item_types: \"not_a_map\"\n"
        "tokens: \"not_a_map\"\n"
        "symbols: \"not_a_map\"\n"
        "flagsets: \"not_a_map\"\n";

    path = write_temp_yaml(scalar_sections_yaml);
    c = codec_load(path);
    ck_assert_ptr_nonnull(c);
    codec_free(c);
    unlink(path);
}
END_TEST

START_TEST(test_codec_encode_full_roundtrip)
{
    const char *path = TEST_DATA_DIR "/example1/codecs/hid_report.yaml";
    codec_t *c = codec_load(path);
    ck_assert_ptr_nonnull(c);

    GList *items = NULL;
    codec_item_t it_up = { .key = "usage_page", .value = "Generic Desktop" };
    codec_item_t it_u = { .key = "usage", .value = "Keyboard" };
    codec_item_t it_coll = { .key = "collection", .value = "Application" };
    codec_item_t it_umin = { .key = "usage_minimum", .value = "224" };
    codec_item_t it_umax = { .key = "usage_maximum", .value = "231" };
    codec_item_t it_lmin = { .key = "logical_minimum", .value = "0" };
    codec_item_t it_lmax = { .key = "logical_maximum", .value = "1" };
    codec_item_t it_rsz = { .key = "report_size", .value = "1" };
    codec_item_t it_rcnt = { .key = "report_count", .value = "8" };
    codec_item_t it_inp = { .key = "input", .value = "Data,Var,Abs" };
    codec_item_t it_end = { .key = "end_collection", .value = NULL };

    items = g_list_append(items, &it_up);
    items = g_list_append(items, &it_u);
    items = g_list_append(items, &it_coll);
    items = g_list_append(items, &it_umin);
    items = g_list_append(items, &it_umax);
    items = g_list_append(items, &it_lmin);
    items = g_list_append(items, &it_lmax);
    items = g_list_append(items, &it_rsz);
    items = g_list_append(items, &it_rcnt);
    items = g_list_append(items, &it_inp);
    items = g_list_append(items, &it_end);

    GByteArray *out = g_byte_array_new();
    int ret = codec_encode(c, items, out);
    ck_assert_int_eq(ret, 0);
    ck_assert_int_gt(out->len, 0);

    /* End collection is prefix only: tag=0xC, type=0 (main), bSize=0 -> 0xC0 */
    ck_assert_uint_eq(out->data[out->len - 1], 0xC0);

    g_byte_array_free(out, TRUE);
    g_list_free(items);
    codec_free(c);
}
END_TEST

START_TEST(test_codec_encode_widths_and_autos)
{
    const char *widths_yaml =
        "codec: \"widths\"\n"
        "item_types: { global: 1 }\n"
        "tokens:\n"
        "  tok_w0:     { type: global, tag: 0x0, width: 0 }\n"
        "  tok_w1:     { type: global, tag: 0x1, width: 1 }\n"
        "  tok_w2:     { type: global, tag: 0x2, width: 2 }\n"
        "  tok_w3:     { type: global, tag: 0x3, width: 3 }\n"
        "  tok_w4:     { type: global, tag: 0x4, width: 4 }\n"
        "  tok_auto_u: { type: global, tag: 0x5, width: auto, signed: false }\n"
        "  tok_auto_s: { type: global, tag: 0x6, width: auto, signed: true }\n";

    g_autofree char *path = write_temp_yaml(widths_yaml);
    codec_t *c = codec_load(path);
    ck_assert_ptr_nonnull(c);

    GList *items = NULL;
    codec_item_t it0 = { .key = "tok_w0", .value = "" };
    codec_item_t it1 = { .key = "tok_w1", .value = "42" };
    codec_item_t it2 = { .key = "tok_w2", .value = "1000" };
    codec_item_t it3 = { .key = "tok_w3", .value = "100" }; /* hits line 238 (default) */
    codec_item_t it4 = { .key = "tok_w4", .value = "100000" };

    /* auto_bytes_u: 1 byte (<=0xFF), 2 bytes (<=0xFFFF), 4 bytes (>0xFFFF) -> lines 246-247 */
    codec_item_t it_u1 = { .key = "tok_auto_u", .value = "200" };
    codec_item_t it_u2 = { .key = "tok_auto_u", .value = "4096" };
    codec_item_t it_u4 = { .key = "tok_auto_u", .value = "1048576" };

    /* auto_bytes_s: 1 byte, 2 bytes, 4 bytes -> lines 254-255 */
    codec_item_t it_s1 = { .key = "tok_auto_s", .value = "-50" };
    codec_item_t it_s2 = { .key = "tok_auto_s", .value = "-500" };
    codec_item_t it_s4 = { .key = "tok_auto_s", .value = "-50000" };

    items = g_list_append(items, &it0);
    items = g_list_append(items, &it1);
    items = g_list_append(items, &it2);
    items = g_list_append(items, &it3);
    items = g_list_append(items, &it4);
    items = g_list_append(items, &it_u1);
    items = g_list_append(items, &it_u2);
    items = g_list_append(items, &it_u4);
    items = g_list_append(items, &it_s1);
    items = g_list_append(items, &it_s2);
    items = g_list_append(items, &it_s4);

    GByteArray *out = g_byte_array_new();
    int ret = codec_encode(c, items, out);
    ck_assert_int_eq(ret, 0);

    g_byte_array_free(out, TRUE);
    g_list_free(items);
    codec_free(c);
    unlink(path);
}
END_TEST

START_TEST(test_codec_encode_flagset_branches)
{
    const char *fs_yaml =
        "codec: \"flagset_test\"\n"
        "item_types: { main: 0 }\n"
        "tokens:\n"
        "  tok_valid_fs: { type: main, tag: 0x1, width: 1, flagset: io }\n"
        "  tok_bad_fs:   { type: main, tag: 0x2, width: 1, flagset: nonexistent_fs }\n"
        "flagsets:\n"
        "  io: { Data: 0, Const: 1, Var: 2, Rel: 4 }\n";

    g_autofree char *path = write_temp_yaml(fs_yaml);
    codec_t *c = codec_load(path);
    ck_assert_ptr_nonnull(c);

    GByteArray *out = g_byte_array_new();

    /* 1. Token references unknown flagset -> lines 265, 267 */
    codec_item_t bad_fs_item = { .key = "tok_bad_fs", .value = "Data" };
    GList *bad_fs_list = g_list_append(NULL, &bad_fs_item);
    ck_assert_int_eq(codec_encode(c, bad_fs_list, out), -1);
    g_list_free(bad_fs_list);

    /* 2. Flagset with empty comma-delimited element -> line 274 */
    codec_item_t empty_elem_item = { .key = "tok_valid_fs", .value = "Data,,Var" };
    GList *empty_elem_list = g_list_append(NULL, &empty_elem_item);
    ck_assert_int_eq(codec_encode(c, empty_elem_list, out), 0);
    g_list_free(empty_elem_list);

    /* 3. Flagset with unknown flag name -> lines 276-278 */
    codec_item_t unknown_flag_item = { .key = "tok_valid_fs", .value = "Data,InvalidFlag" };
    GList *unknown_flag_list = g_list_append(NULL, &unknown_flag_item);
    ck_assert_int_eq(codec_encode(c, unknown_flag_list, out), -1);
    g_list_free(unknown_flag_list);

    g_byte_array_free(out, TRUE);
    codec_free(c);
    unlink(path);
}
END_TEST

START_TEST(test_codec_encode_symbol_and_token_errors)
{
    const char *path = TEST_DATA_DIR "/example1/codecs/hid_report.yaml";
    codec_t *c = codec_load(path);
    ck_assert_ptr_nonnull(c);

    GByteArray *out = g_byte_array_new();

    /* 1. NULL codec or NULL out -> line 330 */
    ck_assert_int_eq(codec_encode(NULL, NULL, out), -1);
    ck_assert_int_eq(codec_encode(c, NULL, NULL), -1);

    /* 2. Unknown token key -> lines 339-340 */
    codec_item_t bad_tok_item = { .key = "unknown_token_key", .value = "1" };
    GList *bad_tok_list = g_list_append(NULL, &bad_tok_item);
    ck_assert_int_eq(codec_encode(c, bad_tok_list, out), -1);
    g_list_free(bad_tok_list);

    /* 3. Unknown symbolic value for token -> lines 318-319, 354 */
    codec_item_t bad_sym_item = { .key = "usage_page", .value = "Nonexistent Page" };
    GList *bad_sym_list = g_list_append(NULL, &bad_sym_item);
    ck_assert_int_eq(codec_encode(c, bad_sym_list, out), -1);
    g_list_free(bad_sym_list);

    /* 4. Item with NULL key or NULL item in list -> lines 334-335 */
    codec_item_t null_key_item = { .key = NULL, .value = "1" };
    GList *null_items = NULL;
    null_items = g_list_append(null_items, NULL);
    null_items = g_list_append(null_items, &null_key_item);
    ck_assert_int_eq(codec_encode(c, null_items, out), 0);
    g_list_free(null_items);

    g_byte_array_free(out, TRUE);
    codec_free(c);
}
END_TEST

START_TEST(test_codec_internal_unit_checks)
{
    /* 1. codec_token_free NULL check -> line 101 */
    codec_token_free(NULL);

    /* 2. size_code cases -> lines 234, 236-238 */
    ck_assert_int_eq(size_code(0), 0);
    ck_assert_int_eq(size_code(1), 1);
    ck_assert_int_eq(size_code(2), 2);
    ck_assert_int_eq(size_code(4), 3);
    ck_assert_int_eq(size_code(3), 1);
    ck_assert_int_eq(size_code(5), 1);

    /* 3. auto_bytes_u thresholds -> lines 246-247 */
    ck_assert_int_eq(auto_bytes_u(0xFF), 1);
    ck_assert_int_eq(auto_bytes_u(0x100), 2);
    ck_assert_int_eq(auto_bytes_u(0xFFFF), 2);
    ck_assert_int_eq(auto_bytes_u(0x10000), 4);

    /* 4. auto_bytes_s thresholds -> lines 254-255 */
    ck_assert_int_eq(auto_bytes_s(-128), 1);
    ck_assert_int_eq(auto_bytes_s(127), 1);
    ck_assert_int_eq(auto_bytes_s(-129), 2);
    ck_assert_int_eq(auto_bytes_s(128), 2);
    ck_assert_int_eq(auto_bytes_s(-32768), 2);
    ck_assert_int_eq(auto_bytes_s(32767), 2);
    ck_assert_int_eq(auto_bytes_s(-32769), 4);
    ck_assert_int_eq(auto_bytes_s(32768), 4);

    /* 5. parse_uint_map and parse_nested_maps NULL node safety -> lines 52, 112 */
    GHashTable *h = parse_uint_map(NULL, NULL);
    ck_assert_ptr_nonnull(h);
    ck_assert_uint_eq(g_hash_table_size(h), 0);
    g_hash_table_destroy(h);

    GHashTable *dst = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_destroy);
    parse_nested_maps(NULL, NULL, dst);
    ck_assert_uint_eq(g_hash_table_size(dst), 0);
    g_hash_table_destroy(dst);
}
END_TEST

static Suite *codec_suite(void)
{
    Suite *s = suite_create("codec");
    TCase *tc = tcase_create("core");

    tcase_add_test(tc, test_codec_load_valid);
    tcase_add_test(tc, test_codec_load_file_errors);
    tcase_add_test(tc, test_codec_load_yaml_structure_branches);
    tcase_add_test(tc, test_codec_encode_full_roundtrip);
    tcase_add_test(tc, test_codec_encode_widths_and_autos);
    tcase_add_test(tc, test_codec_encode_flagset_branches);
    tcase_add_test(tc, test_codec_encode_symbol_and_token_errors);
    tcase_add_test(tc, test_codec_internal_unit_checks);

    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = codec_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
