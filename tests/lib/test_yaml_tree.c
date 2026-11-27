/* Unit tests for src/lib/yaml/tree.c */

#include <stdlib.h>
#include <sys/stat.h>
#include <check.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "yaml/tree.h"
#include "codec.h"
#include "schema.h"
#include "utils.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "../data"
#endif

START_TEST(test_tree_load_null_and_basic)
{
    ck_assert_ptr_null(yaml_tree_load(NULL, NULL, NULL, NULL));
    ck_assert_ptr_null(yaml_tree_load("/nonexistent/file.yaml", NULL, NULL, NULL));

    g_autofree char *dir = g_dir_make_tmp("usbvcr_tree_basic_XXXXXX", NULL);
    g_autofree char *unreadable = g_build_filename(dir, "unreadable.yaml", NULL);
    ck_assert(g_file_set_contents(unreadable, "namespace: test\n", -1, NULL));
    chmod(unreadable, 0000);
    ck_assert_ptr_null(yaml_tree_load(unreadable, NULL, NULL, NULL));
    chmod(unreadable, 0600);

    /* Invalid YAML syntax */
    g_autofree char *syntax_err = g_build_filename(dir, "syntax_err.yaml", NULL);
    ck_assert(g_file_set_contents(syntax_err, "key: [unclosed\n", -1, NULL));
    ck_assert_ptr_null(yaml_tree_load(syntax_err, NULL, NULL, NULL));

    /* Basic valid YAML with namespace */
    g_autofree char *basic_yaml = g_build_filename(dir, "basic.yaml", NULL);
    const char *content = "namespace: \"test_ns\"\n"
                          "device_descriptor:\n"
                          "  bLength: 18\n"
                          "  bDescriptorType: 1\n"
                          "  bcdUSB: 0x0200\n"
                          "  bDeviceClass: 0\n"
                          "  bMaxPacketSize0: 64\n"
                          "  idVendor: 0x1234\n"
                          "  idProduct: 0x5678\n"
                          "  bcdDevice: 0x0100\n"
                          "  bNumConfigurations: 1\n";
    ck_assert(g_file_set_contents(basic_yaml, content, -1, NULL));

    char *ns = NULL;
    usb_node_t *root = yaml_tree_load(basic_yaml, NULL, NULL, &ns);
    ck_assert_ptr_nonnull(root);
    ck_assert_ptr_nonnull(ns);
    ck_assert_str_eq(ns, "test_ns");
    g_free(ns);
    usb_node_free(root);

    g_remove(unreadable);
    g_remove(syntax_err);
    g_remove(basic_yaml);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_tree_raw_hex_block)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_tree_hex_XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "raw_hex.yaml", NULL);

    const char *yaml = "bos_descriptor:\n"
                       "  hex: \"05 0f 16 00 02 07 10 02\"\n"
                       "invalid_hex:\n"
                       "  hex: \"01 ZZ 02\"\n"
                       "odd_hex:\n"
                       "  hex: \"01 2\"\n"
                       "empty_hex:\n"
                       "  hex: \"\"\n";
    ck_assert(g_file_set_contents(path, yaml, -1, NULL));

    usb_node_t *root = yaml_tree_load(path, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);

    /* Check bos_descriptor array */
    usb_prop_t *p_bos = NULL;
    for (GList *l = root->props; l; l = l->next)
    {
        usb_prop_t *p = l->data;
        if (strcmp(p->name, "bos_descriptor") == 0)
        {
            p_bos = p;
            break;
        }
    }
    ck_assert_ptr_nonnull(p_bos);
    ck_assert_int_eq(p_bos->type, USB_PROP_ARRAY);
    ck_assert_int_eq(p_bos->size, 8);

    usb_node_free(root);
    g_remove(path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_tree_codec_block)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_tree_codec_XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "codec_test.yaml", NULL);

    const char *yaml = "report_descriptor:\n"
                       "  codec: \"unknown_codec\"\n"
                       "  items:\n"
                       "    - \"not_a_mapping\"\n"
                       "    - usage_page: \"Generic Desktop\"\n"
                       "valid_codec_desc:\n"
                       "  codec: \"hid_report\"\n"
                       "  items:\n"
                       "    - \"not_a_mapping\"\n"
                       "    - usage_page: \"Generic Desktop\"\n"
                       "    - usage: \"Keyboard\"\n"
                       "empty_codec_desc:\n"
                       "  codec: \"hid_report\"\n"
                       "  items: []\n";
    ck_assert(g_file_set_contents(path, yaml, -1, NULL));

    GHashTable *codecs
        = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)codec_free);
    char *datadir = usbvcr_find_data_file("codecs", "hid_report.yaml");
    if (datadir)
    {
        codec_t *c = codec_load(datadir);
        if (c)
        {
            g_hash_table_insert(codecs, g_strdup(codec_name(c)), c);
        }
        g_free(datadir);
    }

    usb_node_t *root = yaml_tree_load(path, NULL, codecs, NULL);
    ck_assert_ptr_nonnull(root);

    usb_node_free(root);
    g_hash_table_destroy(codecs);
    g_remove(path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_tree_scalars_and_schemas)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_tree_scalars_XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "scalars.yaml", NULL);

    const char *yaml = "schema: \"unknown_schema\"\n"
                       "template: \"unknown_template\"\n"
                       "valid_node:\n"
                       "  schema: \"base.standard\"\n"
                       "  bLength: null\n"
                       "  bDescriptorType: ~\n"
                       "  bcdUSB: 0x0200\n"
                       "  bMaxPacketSize0: 64\n"
                       "  comment_field: \"non-numeric string\"\n";
    ck_assert(g_file_set_contents(path, yaml, -1, NULL));

    schema_t *schemas = schema_new();
    usb_node_t *base_node = usb_node_create();
    usb_node_t *std_node = usb_node_create();
    usb_node_set_prop(std_node, create_prop_u8("bLength", 18));
    usb_node_set_prop(base_node, create_prop_node("standard", std_node));
    schema_register(schemas, base_node, "base");
    usb_node_free(base_node);

    usb_node_t *root = yaml_tree_load(path, schemas, NULL, NULL);
    ck_assert_ptr_nonnull(root);

    schema_free(schemas);
    usb_node_free(root);
    g_remove(path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_tree_strings_and_sequences)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_tree_seq_XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "strings_seq.yaml", NULL);

    const char *yaml = "strings:\n"
                       "  1: \"Manufacturer\"\n"
                       "  2: \"Product\"\n"
                       "  ? [non_scalar]: \"val\"\n"
                       "scalar_seq:\n"
                       "  - 10\n"
                       "  - 20\n"
                       "map_seq:\n"
                       "  - bInterfaceNumber: 0\n"
                       "  - bInterfaceNumber: 1\n";
    ck_assert(g_file_set_contents(path, yaml, -1, NULL));

    usb_node_t *root = yaml_tree_load(path, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);

    usb_node_free(root);
    g_remove(path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_tree_max_depth_and_non_scalar_keys)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_tree_deep_XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "deep.yaml", NULL);

    /* Construct YAML with 35 levels of nesting */
    GString *s = g_string_new(NULL);
    for (int i = 0; i < 35; i++)
    {
        for (int sp = 0; sp < i * 2; sp++)
        {
            g_string_append_c(s, ' ');
        }
        g_string_append_printf(s, "level%d:\n", i);
    }
    for (int sp = 0; sp < 35 * 2; sp++)
    {
        g_string_append_c(s, ' ');
    }
    g_string_append(s, "leaf: 1\n");
    g_string_append(s, "? [non_scalar_key]: { val: 1 }\n");

    ck_assert(g_file_set_contents(path, s->str, -1, NULL));
    g_string_free(s, TRUE);

    usb_node_t *root = yaml_tree_load(path, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);
    usb_node_free(root);

    g_remove(path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_tree_oversized_file)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_tree_oversize_XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "large.yaml", NULL);

    /* Create file slightly over 1 MiB (YAML_MAX_FILE_BYTES) */
    size_t sz = (1024 * 1024) + 128;
    char *buf = g_malloc0(sz);
    memset(buf, ' ', sz);
    memcpy(buf, "a: 1\n", 5);
    ck_assert(g_file_set_contents(path, buf, sz, NULL));
    g_free(buf);

    ck_assert_ptr_null(yaml_tree_load(path, NULL, NULL, NULL));

    g_remove(path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_tree_extends_variants_and_cycles)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_tree_ext_XXXXXX", NULL);
    g_autofree char *devs_dir = g_build_filename(dir, "devices", NULL);
    g_mkdir(devs_dir, 0700);

    /* 1. Base device */
    g_autofree char *base_path = g_build_filename(dir, "base_dev.yaml", NULL);
    const char *base_yaml = "device_descriptor:\n"
                            "  bLength: 18\n"
                            "  idVendor: 0x1111\n";
    ck_assert(g_file_set_contents(base_path, base_yaml, -1, NULL));

    /* 2. Device extending base relative to dir */
    g_autofree char *child_path = g_build_filename(dir, "child.yaml", NULL);
    const char *child_yaml = "extends: \"base_dev.yaml\"\n"
                             "device_descriptor:\n"
                             "  idProduct: 0x2222\n";
    ck_assert(g_file_set_contents(child_path, child_yaml, -1, NULL));

    usb_node_t *root = yaml_tree_load(child_path, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);
    usb_node_free(root);

    /* 3. Device extending absolute path */
    g_autofree char *child_abs_path = g_build_filename(dir, "child_abs.yaml", NULL);
    g_autofree char *child_abs_yaml = g_strdup_printf("extends: \"%s\"\n"
                                                      "device_descriptor:\n"
                                                      "  idProduct: 0x3333\n",
                                                      base_path);
    ck_assert(g_file_set_contents(child_abs_path, child_abs_yaml, -1, NULL));

    root = yaml_tree_load(child_abs_path, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);
    usb_node_free(root);

    /* 4. Device extending bare ID under devices/ subdirectory */
    g_autofree char *sub_dev = g_build_filename(devs_dir, "my_sub_dev.yaml", NULL);
    ck_assert(g_file_set_contents(sub_dev, base_yaml, -1, NULL));

    g_autofree char *child_sub = g_build_filename(dir, "child_sub.yaml", NULL);
    const char *child_sub_yaml = "extends: \"my_sub_dev\"\n"
                                 "device_descriptor:\n"
                                 "  idProduct: 0x4444\n";
    ck_assert(g_file_set_contents(child_sub, child_sub_yaml, -1, NULL));

    root = yaml_tree_load(child_sub, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);
    usb_node_free(root);

    /* 5. Device extending bare ID in same dir */
    g_autofree char *child_bare = g_build_filename(dir, "child_bare.yaml", NULL);
    const char *child_bare_yaml = "extends: \"base_dev\"\n"
                                  "device_descriptor:\n"
                                  "  idProduct: 0x5555\n";
    ck_assert(g_file_set_contents(child_bare, child_bare_yaml, -1, NULL));

    root = yaml_tree_load(child_bare, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);
    usb_node_free(root);

    /* 6. Extends nonexistent / unresolvable reference */
    g_autofree char *child_nonexist = g_build_filename(dir, "child_nonexist.yaml", NULL);
    const char *child_nonexist_yaml = "extends: \"nonexistent_base_xyz\"\n"
                                      "device_descriptor:\n"
                                      "  idProduct: 0x6666\n";
    ck_assert(g_file_set_contents(child_nonexist, child_nonexist_yaml, -1, NULL));

    root = yaml_tree_load(child_nonexist, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);
    usb_node_free(root);

    /* 7. Extends broken base device (syntax error) */
    g_autofree char *broken_base = g_build_filename(dir, "broken_base.yaml", NULL);
    ck_assert(g_file_set_contents(broken_base, "bad: [unclosed\n", -1, NULL));

    g_autofree char *child_broken = g_build_filename(dir, "child_broken.yaml", NULL);
    const char *child_broken_yaml = "extends: \"broken_base.yaml\"\n"
                                    "device_descriptor:\n"
                                    "  idProduct: 0x7777\n";
    ck_assert(g_file_set_contents(child_broken, child_broken_yaml, -1, NULL));

    root = yaml_tree_load(child_broken, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);
    usb_node_free(root);

    /* 8. Circular extends: cycle_a extends cycle_b, cycle_b extends cycle_a */
    g_autofree char *cycle_a = g_build_filename(dir, "cycle_a.yaml", NULL);
    g_autofree char *cycle_b = g_build_filename(dir, "cycle_b.yaml", NULL);
    const char *cycle_a_yaml = "extends: \"cycle_b.yaml\"\n"
                               "device_descriptor:\n"
                               "  idVendor: 0xAAAA\n";
    const char *cycle_b_yaml = "extends: \"cycle_a.yaml\"\n"
                               "device_descriptor:\n"
                               "  idVendor: 0xBBBB\n";
    ck_assert(g_file_set_contents(cycle_a, cycle_a_yaml, -1, NULL));
    ck_assert(g_file_set_contents(cycle_b, cycle_b_yaml, -1, NULL));

    root = yaml_tree_load(cycle_a, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);
    usb_node_free(root);

    /* 9. Self extends */
    g_autofree char *self_ext = g_build_filename(dir, "self_ext.yaml", NULL);
    const char *self_ext_yaml = "extends: \"self_ext.yaml\"\n"
                                "device_descriptor:\n"
                                "  idVendor: 0xCCCC\n";
    ck_assert(g_file_set_contents(self_ext, self_ext_yaml, -1, NULL));

    root = yaml_tree_load(self_ext, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);
    usb_node_free(root);

    /* 10. Extends datadir with devices/ prefix */
    g_autofree char *child_datadir = g_build_filename(dir, "child_datadir.yaml", NULL);
    const char *child_datadir_yaml = "extends: \"devices/keyboard.yaml\"\n"
                                     "device_descriptor:\n"
                                     "  idProduct: 0x8888\n";
    ck_assert(g_file_set_contents(child_datadir, child_datadir_yaml, -1, NULL));
    root = yaml_tree_load(child_datadir, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(root);
    usb_node_free(root);

    g_remove(child_datadir);

    g_remove(base_path);
    g_remove(child_path);
    g_remove(child_abs_path);
    g_remove(sub_dev);
    g_remove(child_sub);
    g_remove(child_bare);
    g_remove(child_nonexist);
    g_remove(broken_base);
    g_remove(child_broken);
    g_remove(cycle_a);
    g_remove(cycle_b);
    g_remove(self_ext);
    g_rmdir(devs_dir);
    g_rmdir(dir);
}
END_TEST

static Suite *
yaml_tree_suite(void)
{
    Suite *s = suite_create("yaml_tree");
    TCase *tc = tcase_create("tree");

    tcase_add_test(tc, test_tree_load_null_and_basic);
    tcase_add_test(tc, test_tree_raw_hex_block);
    tcase_add_test(tc, test_tree_codec_block);
    tcase_add_test(tc, test_tree_scalars_and_schemas);
    tcase_add_test(tc, test_tree_strings_and_sequences);
    tcase_add_test(tc, test_tree_max_depth_and_non_scalar_keys);
    tcase_add_test(tc, test_tree_oversized_file);
    tcase_add_test(tc, test_tree_extends_variants_and_cycles);

    suite_add_tcase(s, tc);
    return s;
}

int
main(void)
{
    SRunner *sr = srunner_create(yaml_tree_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
