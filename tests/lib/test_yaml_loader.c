/* Unit tests for src/lib/yaml/loader.c.
 *
 * yaml_scope_from_path() is fully implemented and tested here.
 * yaml_parse_from_file() / yaml_device_free() are stubs today — the few
 * assertions below just pin their current contract. Extend as the loader
 * grows (imports, templates, null autofill, flow load).
 */

#include <stdlib.h>
#include <check.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "yaml/loader.h"
#include "utils.h"

/* meson passes -DTEST_DATA_DIR="<srcdir>/data"; fall back for ad-hoc builds. */
#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "../data"
#endif

/* ── yaml_scope_from_path: each recognized directory maps to its scope ── */

START_TEST(test_scope_each_dir)
{
    ck_assert_int_eq(yaml_scope_from_path("schemas/kbd_base.yaml"), YAML_SCOPE_SCHEMAS);
    ck_assert_int_eq(yaml_scope_from_path("devices/kbd.yaml"), YAML_SCOPE_DEVICES);
    ck_assert_int_eq(yaml_scope_from_path("flows/enum.yaml"),  YAML_SCOPE_FLOWS);
    ck_assert_int_eq(yaml_scope_from_path("tests/cve.yaml"),   YAML_SCOPE_TESTS);
}
END_TEST

/* Only the immediate parent directory decides the scope. */
START_TEST(test_scope_deep_path)
{
    ck_assert_int_eq(yaml_scope_from_path("tests/data/example2/flows/x.yaml"),
                     YAML_SCOPE_FLOWS);
    ck_assert_int_eq(yaml_scope_from_path("/abs/proj/tests/cve.yaml"),
                     YAML_SCOPE_TESTS);
}
END_TEST

/* A file with no directory component has no scope. */
START_TEST(test_scope_no_dir)
{
    ck_assert_int_eq(yaml_scope_from_path("bare.yaml"), YAML_SCOPE_UNKNOWN);
}
END_TEST

/* Unrecognized directories (incl. payloads/) are unknown scopes. */
START_TEST(test_scope_unknown_dir)
{
    ck_assert_int_eq(yaml_scope_from_path("payloads/blob.bin"), YAML_SCOPE_UNKNOWN);
    ck_assert_int_eq(yaml_scope_from_path("random/x.yaml"),     YAML_SCOPE_UNKNOWN);
}
END_TEST

/* Matching is exact: substrings / different case do not match. */
START_TEST(test_scope_no_fuzzy_match)
{
    ck_assert_int_eq(yaml_scope_from_path("mydevices/x.yaml"), YAML_SCOPE_UNKNOWN);
    ck_assert_int_eq(yaml_scope_from_path("Devices/x.yaml"),   YAML_SCOPE_UNKNOWN);
    ck_assert_int_eq(yaml_scope_from_path("device/x.yaml"),    YAML_SCOPE_UNKNOWN);
}
END_TEST

/* NULL path is guarded. */
START_TEST(test_scope_null)
{
    ck_assert_int_eq(yaml_scope_from_path(NULL), YAML_SCOPE_UNKNOWN);
}
END_TEST

/* ── loader stubs: pin current contract (update when implemented) ── */

START_TEST(test_parse_from_file_null_dir)
{
    /* NULL project dir -> NULL. */
    ck_assert_ptr_null(yaml_parse_from_file(NULL));
}
END_TEST

START_TEST(test_parse_from_file_missing_dir)
{
    /* A missing project file is a hard error: yaml_parse_from_file returns
     * NULL (the manifest is mandatory, README §3). */
    yaml_device_t *dev = yaml_parse_from_file("/nonexistent/project");
    ck_assert_ptr_null(dev);
}
END_TEST

START_TEST(test_device_free_null)
{
    /* Freeing NULL must be a safe no-op. */
    yaml_device_free(NULL);
}
END_TEST

START_TEST(test_parse_from_file_example1)
{
    /* Load example 1 of data samples (the project manifest file). Path is
     * absolute (TEST_DATA_DIR from meson) so it resolves regardless of the
     * test's working directory. */
    yaml_device_t *dev =
        yaml_parse_from_file(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_nonnull(dev->tree);
    ck_assert_str_eq(dev->name, "Hello World Injector Keyboard");
    yaml_device_free(dev);
}
END_TEST

START_TEST(test_parse_from_file_no_name)
{
    /* `name:` is optional — a project without one loads fine, dev->name
     * stays NULL (main.c's --local path falls back to "usbvcr"). */
    g_autofree char *dir = g_dir_make_tmp("usbvcr_noname_XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "project.yaml", NULL);
    ck_assert(g_file_set_contents(path, "version: \"1.0\"\n", -1, NULL));

    yaml_device_t *dev = yaml_parse_from_file(path);
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_null(dev->name);
    yaml_device_free(dev);

    g_remove(path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_parse_from_file_no_connection)
{
    /* `connection:` is optional — a project without one loads fine, with
     * every field left NULL (main.c falls back to CLI flags / defaults). */
    yaml_device_t *dev =
        yaml_parse_from_file(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_null(dev->engine);
    ck_assert_ptr_null(dev->udc);
    ck_assert_ptr_null(dev->host);
    ck_assert_ptr_null(dev->port);
    yaml_device_free(dev);
}
END_TEST

START_TEST(test_parse_from_file_connection)
{
    /* A `connection:` block pins the run config in the manifest, so
     * `usbvcr --yaml <file>` alone is enough to run (README §3). */
    g_autofree char *dir = g_dir_make_tmp("usbvcr_connection_XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "project.yaml", NULL);
    ck_assert(g_file_set_contents(path,
        "version: \"1.0\"\n"
        "name: \"conn-test\"\n"
        "connection:\n"
        "  engine: local\n"
        "  udc: dummy_udc.0\n"
        "  host: 127.0.0.1\n"
        "  port: \"1337\"\n",
        -1, NULL));

    yaml_device_t *dev = yaml_parse_from_file(path);
    ck_assert_ptr_nonnull(dev);
    ck_assert_str_eq(dev->engine, "local");
    ck_assert_str_eq(dev->udc, "dummy_udc.0");
    ck_assert_str_eq(dev->host, "127.0.0.1");
    ck_assert_str_eq(dev->port, "1337");
    yaml_device_free(dev);

    g_remove(path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_bare_id_resolution)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr-test-bare-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "project.yaml", NULL);
    ck_assert(g_file_set_contents(path,
        "version: \"1.0\"\n"
        "name: \"bare-id-test\"\n"
        "devices:\n"
        "  - device: keyboard\n"
        "    flow: enumeration_flow\n",
        -1, NULL));

    yaml_device_t *dev = yaml_parse_from_file(path);
    ck_assert_ptr_nonnull(dev);
    ck_assert_str_eq(dev->name, "bare-id-test");
    ck_assert_int_eq(g_list_length(dev->devices), 1);
    ck_assert_ptr_nonnull(dev->tree);
    yaml_device_free(dev);

    g_remove(path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_extends_device)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr-test-extends-XXXXXX", NULL);
    g_autofree char *dev_path = g_build_filename(dir, "custom_kbd.yaml", NULL);
    g_autofree char *proj_path = g_build_filename(dir, "project.yaml", NULL);

    ck_assert(g_file_set_contents(dev_path,
        "extends: keyboard\n"
        "device_descriptor:\n"
        "  idVendor: 0x1234\n"
        "  idProduct: 0x5678\n",
        -1, NULL));

    ck_assert(g_file_set_contents(proj_path,
        "version: \"1.0\"\n"
        "name: \"extends-test\"\n"
        "devices:\n"
        "  - device: \"custom_kbd.yaml\"\n"
        "    flow: enumeration_flow\n",
        -1, NULL));

    yaml_device_t *dev = yaml_parse_from_file(proj_path);
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_nonnull(dev->tree);

    /* Verify overridden vendor and product IDs */
    usb_prop_t *p_desc = NULL;
    for (GList *l = dev->tree->props; l; l = l->next) {
        usb_prop_t *p = l->data;
        if (strcmp(p->name, "device_descriptor") == 0) {
            p_desc = p;
            break;
        }
    }
    ck_assert_ptr_nonnull(p_desc);
    ck_assert_int_eq(p_desc->type, USB_PROP_NODE);

    uint16_t vid = 0, pid = 0;
    for (GList *l = p_desc->val.node->props; l; l = l->next) {
        usb_prop_t *p = l->data;
        if (strcmp(p->name, "idVendor") == 0) vid = p->val.u16;
        if (strcmp(p->name, "idProduct") == 0) pid = p->val.u16;
    }
    ck_assert_int_eq(vid, 0x1234);
    ck_assert_int_eq(pid, 0x5678);

    yaml_device_free(dev);

    g_remove(dev_path);
    g_remove(proj_path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_parse_speed)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr-test-speed-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "project.yaml", NULL);

    ck_assert(g_file_set_contents(path,
        "version: \"1.0\"\n"
        "name: \"speed-test\"\n"
        "connection:\n"
        "  speed: high\n"
        "devices:\n"
        "  - device: keyboard\n"
        "    speed: full\n",
        -1, NULL));

    yaml_device_t *dev = yaml_parse_from_file(path);
    ck_assert_ptr_nonnull(dev);
    ck_assert_int_eq(dev->speed, USB_VCR_SPEED_FULL);

    yaml_dev_entry_t *entry = dev->devices->data;
    ck_assert_int_eq(entry->speed, USB_VCR_SPEED_FULL);

    yaml_device_free(dev);
    g_remove(path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_parse_file_and_loop)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr-test-file-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "project.yaml", NULL);
    g_autofree char *fake_mjpeg = g_build_filename(dir, "video.mjpeg", NULL);

    ck_assert(g_file_set_contents(fake_mjpeg, "fake", 4, NULL));
    ck_assert(g_file_set_contents(path,
        "version: \"1.0\"\n"
        "name: \"file-test\"\n"
        "devices:\n"
        "  - device: keyboard\n"
        "    file: video.mjpeg\n"
        "    loop: true\n",
        -1, NULL));

    yaml_device_t *dev = yaml_parse_from_file(path);
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_nonnull(dev->file);
    ck_assert(g_str_has_suffix(dev->file, "video.mjpeg"));
    ck_assert_int_eq(dev->loop, TRUE);

    yaml_device_free(dev);
    g_remove(fake_mjpeg);
    g_remove(path);
    g_rmdir(dir);
}
END_TEST

static Suite *yaml_loader_suite(void)
{
    Suite *s = suite_create("yaml_loader");

    TCase *tc_scope = tcase_create("scope_from_dir");
    tcase_add_test(tc_scope, test_scope_each_dir);
    tcase_add_test(tc_scope, test_scope_deep_path);
    tcase_add_test(tc_scope, test_scope_no_dir);
    tcase_add_test(tc_scope, test_scope_unknown_dir);
    tcase_add_test(tc_scope, test_scope_no_fuzzy_match);
    tcase_add_test(tc_scope, test_scope_null);
    suite_add_tcase(s, tc_scope);

    TCase *tc_load = tcase_create("loader");
    tcase_add_test(tc_load, test_parse_from_file_null_dir);
    tcase_add_test(tc_load, test_parse_from_file_missing_dir);
    tcase_add_test(tc_load, test_device_free_null);
    tcase_add_test(tc_load, test_parse_from_file_example1);
    tcase_add_test(tc_load, test_parse_from_file_no_name);
    tcase_add_test(tc_load, test_parse_from_file_no_connection);
    tcase_add_test(tc_load, test_parse_from_file_connection);
    tcase_add_test(tc_load, test_bare_id_resolution);
    tcase_add_test(tc_load, test_extends_device);
    tcase_add_test(tc_load, test_parse_speed);
    tcase_add_test(tc_load, test_parse_file_and_loop);
    suite_add_tcase(s, tc_load);

    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(yaml_loader_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
