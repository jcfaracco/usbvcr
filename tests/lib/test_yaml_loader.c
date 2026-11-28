/* Unit tests for src/lib/yaml/loader.c.
 *
 * yaml_scope_from_path() is fully implemented and tested here.
 * yaml_parse_from_file() / yaml_device_free() are stubs today — the few
 * assertions below just pin their current contract. Extend as the loader
 * grows (imports, templates, null autofill, flow load).
 */

#include <stdlib.h>
#include <sys/stat.h>
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
    ck_assert_int_eq(yaml_scope_from_path("templates/kbd_base.yaml"), YAML_SCOPE_TEMPLATES);
    ck_assert_int_eq(yaml_scope_from_path("schemas/kbd_base.yaml"), YAML_SCOPE_TEMPLATES);
    ck_assert_int_eq(yaml_scope_from_path("devices/kbd.yaml"), YAML_SCOPE_DEVICES);
    ck_assert_int_eq(yaml_scope_from_path("flows/enum.yaml"), YAML_SCOPE_FLOWS);
    ck_assert_int_eq(yaml_scope_from_path("tests/cve.yaml"), YAML_SCOPE_TESTS);
}
END_TEST

/* Only the immediate parent directory decides the scope. */
START_TEST(test_scope_deep_path)
{
    ck_assert_int_eq(yaml_scope_from_path("tests/data/example2/flows/x.yaml"), YAML_SCOPE_FLOWS);
    ck_assert_int_eq(yaml_scope_from_path("/abs/proj/tests/cve.yaml"), YAML_SCOPE_TESTS);
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
    ck_assert_int_eq(yaml_scope_from_path("random/x.yaml"), YAML_SCOPE_UNKNOWN);
}
END_TEST

/* Matching is exact: substrings / different case do not match. */
START_TEST(test_scope_no_fuzzy_match)
{
    ck_assert_int_eq(yaml_scope_from_path("mydevices/x.yaml"), YAML_SCOPE_UNKNOWN);
    ck_assert_int_eq(yaml_scope_from_path("Devices/x.yaml"), YAML_SCOPE_UNKNOWN);
    ck_assert_int_eq(yaml_scope_from_path("device/x.yaml"), YAML_SCOPE_UNKNOWN);
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
    yaml_device_t *dev = yaml_parse_from_file(TEST_DATA_DIR "/example1/example1.yaml");
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_nonnull(dev->tree);
    ck_assert_ptr_nonnull(dev->flows);
    ck_assert_ptr_nonnull(dev->flow);
    ck_assert_ptr_nonnull(yaml_device_find_flow(dev, "hello_world_flow"));
    ck_assert_ptr_nonnull(yaml_device_find_flow(dev, "flows/hello_world_flow.yaml"));
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
    yaml_device_t *dev = yaml_parse_from_file(TEST_DATA_DIR "/example1/example1.yaml");
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
    for (GList *l = dev->tree->props; l; l = l->next)
    {
        usb_prop_t *p = l->data;
        if (strcmp(p->name, "device_descriptor") == 0)
        {
            p_desc = p;
            break;
        }
    }
    ck_assert_ptr_nonnull(p_desc);
    ck_assert_int_eq(p_desc->type, USB_PROP_NODE);

    uint16_t vid = 0, pid = 0;
    for (GList *l = p_desc->val.node->props; l; l = l->next)
    {
        usb_prop_t *p = l->data;
        if (strcmp(p->name, "idVendor") == 0)
            vid = p->val.u16;
        if (strcmp(p->name, "idProduct") == 0)
            pid = p->val.u16;
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

START_TEST(test_auto_wire_flow_from_datadir)
{
    /* A manifest without flow: specified should automatically wire a fallback/matching
     * flow from dev->flows (e.g. enumeration_flow from datadir). */
    yaml_device_t *dev = yaml_parse_from_file(TEST_DATA_DIR "/no_flow/no_flow.yaml");
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_nonnull(dev->tree);
    ck_assert_ptr_nonnull(dev->flows);
    ck_assert_ptr_nonnull(dev->flow);
    ck_assert_ptr_nonnull(yaml_device_find_flow(dev, "enumeration_flow"));

    yaml_dev_entry_t *entry = dev->devices->data;
    ck_assert_ptr_eq(entry->flow, dev->flow);

    yaml_device_free(dev);
}
END_TEST

START_TEST(test_auto_wire_local_flows_dir)
{
    /* A project with a local flows/ directory scans all flows into dev->flows
     * and wires them to devices referencing or relying on them. */
    yaml_device_t *dev = yaml_parse_from_file(TEST_DATA_DIR "/autoflow/autoflow.yaml");
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_nonnull(dev->flows);
    ck_assert_ptr_nonnull(yaml_device_find_flow(dev, "custom_flow"));
    ck_assert_ptr_nonnull(dev->flow);

    yaml_dev_entry_t *entry = dev->devices->data;
    ck_assert_ptr_eq(entry->flow, dev->flow);

    yaml_device_free(dev);
}
END_TEST

START_TEST(test_find_flow_and_entry_null_checks)
{
    ck_assert_ptr_null(yaml_device_find_flow(NULL, "kbd"));

    yaml_device_t dev;
    memset(&dev, 0, sizeof(dev));
    ck_assert_ptr_null(yaml_device_find_flow(&dev, NULL));
    ck_assert_ptr_null(yaml_device_find_flow(&dev, "kbd"));

    yaml_device_t *d = yaml_device_create();
    ck_assert_ptr_nonnull(d);
    ck_assert_ptr_null(yaml_device_find_flow(d, NULL));
    ck_assert_ptr_null(yaml_device_find_flow(d, "nonexistent"));
    yaml_device_free(d);
}
END_TEST

START_TEST(test_loader_yaml_syntax_and_version_errors)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_loader_err_XXXXXX", NULL);
    g_autofree char *proj_syntax_err = g_build_filename(dir, "syntax_err.yaml", NULL);
    g_autofree char *proj_ver_err = g_build_filename(dir, "ver_err.yaml", NULL);
    g_autofree char *proj_unreadable = g_build_filename(dir, "unreadable.yaml", NULL);
    g_autofree char *proj_missing_dev = g_build_filename(dir, "missing_dev.yaml", NULL);
    g_autofree char *proj_bad_dev_entry = g_build_filename(dir, "bad_dev_entry.yaml", NULL);

    /* 1. Invalid YAML syntax -> parser load failure */
    ck_assert(g_file_set_contents(proj_syntax_err, "key: [unclosed\n", -1, NULL));
    ck_assert_ptr_null(yaml_parse_from_file(proj_syntax_err));

    /* 2. Version mismatch */
    const char *ver_yaml = "version: \"99.0\"\n"
                           "name: \"ver-test\"\n";
    ck_assert(g_file_set_contents(proj_ver_err, ver_yaml, -1, NULL));
    ck_assert_ptr_null(yaml_parse_from_file(proj_ver_err));

    /* 3. Unreadable file (fopen failure) */
    ck_assert(g_file_set_contents(proj_unreadable, "version: \"1.0\"\n", -1, NULL));
    chmod(proj_unreadable, 0000);
    ck_assert_ptr_null(yaml_parse_from_file(proj_unreadable));
    chmod(proj_unreadable, 0600);

    /* 4. Missing device reference */
    const char *missing_dev_yaml = "version: \"1.0\"\n"
                                   "name: \"missing-dev-test\"\n"
                                   "devices:\n"
                                   "  - device: \"nonexistent_dev_xyz\"\n";
    ck_assert(g_file_set_contents(proj_missing_dev, missing_dev_yaml, -1, NULL));
    ck_assert_ptr_null(yaml_parse_from_file(proj_missing_dev));

    /* 5. Manifest entry without device: and non-mapping entry in devices sequence */
    const char *bad_dev_entry_yaml = "version: \"1.0\"\n"
                                     "name: \"bad-entry-test\"\n"
                                     "devices:\n"
                                     "  - \"not_a_mapping\"\n"
                                     "  - flow: \"only_flow_no_device\"\n";
    ck_assert(g_file_set_contents(proj_bad_dev_entry, bad_dev_entry_yaml, -1, NULL));
    yaml_device_t *dev = yaml_parse_from_file(proj_bad_dev_entry);
    ck_assert_ptr_nonnull(dev);
    yaml_device_free(dev);

    g_remove(proj_syntax_err);
    g_remove(proj_ver_err);
    g_remove(proj_unreadable);
    g_remove(proj_missing_dev);
    g_remove(proj_bad_dev_entry);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_loader_boolean_flags_and_speed)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_loader_flags_XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "flags.yaml", NULL);

    const char *flags_yaml = "version: \"1.0\"\n"
                             "name: \"flags-test\"\n"
                             "connection:\n"
                             "  debug: \"1\"\n"
                             "  speed: \"super\"\n"
                             "loop: \"1\"\n";
    ck_assert(g_file_set_contents(path, flags_yaml, -1, NULL));

    yaml_device_t *dev = yaml_parse_from_file(path);
    ck_assert_ptr_nonnull(dev);
    ck_assert_int_eq(dev->debug, TRUE);
    ck_assert_int_eq(dev->speed, 3); /* USB_VCR_SPEED_SUPER */
    ck_assert_int_eq(dev->loop, TRUE);

    yaml_device_free(dev);
    g_remove(path);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_loader_asset_resolution_paths_and_yml)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_loader_paths_XXXXXX", NULL);
    g_autofree char *devs_dir = g_build_filename(dir, "devices", NULL);
    g_autofree char *flows_dir = g_build_filename(dir, "flows", NULL);
    g_mkdir(devs_dir, 0700);
    g_mkdir(flows_dir, 0700);

    /* Device with .yml suffix */
    g_autofree char *dev_yml = g_build_filename(devs_dir, "my_kbd.yml", NULL);
    const char *kbd_yaml = "device_descriptor:\n"
                           "  bLength: 18\n"
                           "  bDescriptorType: 1\n"
                           "  bcdUSB: 0x0200\n"
                           "  bDeviceClass: 0\n"
                           "  bDeviceSubClass: 0\n"
                           "  bDeviceProtocol: 0\n"
                           "  bMaxPacketSize0: 64\n"
                           "  idVendor: 0x1234\n"
                           "  idProduct: 0x5678\n"
                           "  bcdDevice: 0x0100\n"
                           "  iManufacturer: 0\n"
                           "  iProduct: 0\n"
                           "  iSerialNumber: 0\n"
                           "  bNumConfigurations: 1\n";
    ck_assert(g_file_set_contents(dev_yml, kbd_yaml, -1, NULL));

    /* Flow with .yml suffix */
    g_autofree char *flow_yml = g_build_filename(flows_dir, "my_flow.yml", NULL);
    const char *flow_content = "states:\n"
                               "  - name: \"active\"\n";
    ck_assert(g_file_set_contents(flow_yml, flow_content, -1, NULL));

    /* Manifest referencing .yml files and absolute dev_yml path */
    g_autofree char *proj_path = g_build_filename(dir, "project.yaml", NULL);
    g_autofree char *proj_content = g_strdup_printf("version: \"1.0\"\n"
                                                    "name: \"yml-test\"\n"
                                                    "devices:\n"
                                                    "  - device: \"%s\"\n"
                                                    "    flow: \"flows/my_flow.yml\"\n"
                                                    "    speed: \"high\"\n"
                                                    "    loop: \"1\"\n",
                                                    dev_yml);
    ck_assert(g_file_set_contents(proj_path, proj_content, -1, NULL));

    yaml_device_t *dev = yaml_parse_from_file(proj_path);
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_nonnull(dev->tree);
    ck_assert_ptr_nonnull(dev->flow);
    ck_assert_int_eq(dev->speed, 2); /* USB_VCR_SPEED_HIGH */
    ck_assert_int_eq(dev->loop, TRUE);
    ck_assert_ptr_nonnull(yaml_device_find_flow(dev, "my_flow.yml"));
    ck_assert_ptr_nonnull(yaml_device_find_flow(dev, "my_flow.yaml"));

    yaml_device_free(dev);

    /* Test resolution with sub/ prefix (devices/keyboard.yaml in datadir) */
    const char *proj_sub_ref = "version: \"1.0\"\n"
                               "name: \"sub-ref-test\"\n"
                               "devices:\n"
                               "  - device: \"devices/keyboard.yaml\"\n";
    ck_assert(g_file_set_contents(proj_path, proj_sub_ref, -1, NULL));
    dev = yaml_parse_from_file(proj_path);
    ck_assert_ptr_nonnull(dev);
    yaml_device_free(dev);

    g_remove(dev_yml);
    g_remove(flow_yml);
    g_remove(proj_path);
    g_rmdir(devs_dir);
    g_rmdir(flows_dir);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_loader_schema_and_flow_parse_errors)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_loader_fails_XXXXXX", NULL);
    g_autofree char *templates_dir = g_build_filename(dir, "templates", NULL);
    g_autofree char *schemas_dir = g_build_filename(dir, "schemas", NULL);
    g_autofree char *flows_dir = g_build_filename(dir, "flows", NULL);
    g_autofree char *codecs_dir = g_build_filename(dir, "codecs", NULL);
    g_autofree char *tests_dir = g_build_filename(dir, "tests", NULL);
    g_autofree char *devs_dir = g_build_filename(dir, "devices", NULL);
    g_mkdir(templates_dir, 0700);
    g_mkdir(schemas_dir, 0700);
    g_mkdir(flows_dir, 0700);
    g_mkdir(codecs_dir, 0700);
    g_mkdir(tests_dir, 0700);
    g_mkdir(devs_dir, 0700);

    g_autofree char *proj_path = g_build_filename(dir, "project.yaml", NULL);
    ck_assert(g_file_set_contents(proj_path, "version: \"1.0\"\nname: \"t\"\n", -1, NULL));

    /* 1. Broken template in templates/ */
    g_autofree char *bad_template = g_build_filename(templates_dir, "broken.yaml", NULL);
    ck_assert(g_file_set_contents(bad_template, "bad: [unclosed\n", -1, NULL));
    ck_assert_ptr_null(yaml_parse_from_file(proj_path));
    g_remove(bad_template);

    /* 1b. Broken schema in schemas/ (legacy scope) */
    g_autofree char *bad_schema = g_build_filename(schemas_dir, "broken.yaml", NULL);
    ck_assert(g_file_set_contents(bad_schema, "bad: [unclosed\n", -1, NULL));
    ck_assert_ptr_null(yaml_parse_from_file(proj_path));
    g_remove(bad_schema);

    /* 2. Broken flow in flows/ */
    g_autofree char *bad_flow = g_build_filename(flows_dir, "broken.yaml", NULL);
    ck_assert(g_file_set_contents(bad_flow, "bad: [unclosed\n", -1, NULL));
    ck_assert_ptr_null(yaml_parse_from_file(proj_path));
    g_remove(bad_flow);

    /* 3. Broken codec in codecs/ */
    g_autofree char *bad_codec = g_build_filename(codecs_dir, "broken.yaml", NULL);
    ck_assert(g_file_set_contents(bad_codec, "bad: [unclosed\n", -1, NULL));
    ck_assert_ptr_null(yaml_parse_from_file(proj_path));
    g_remove(bad_codec);

    /* 4. Valid test in tests/ (hits parse_tests) */
    g_autofree char *test_yaml = g_build_filename(tests_dir, "dummy_test.yaml", NULL);
    ck_assert(g_file_set_contents(test_yaml, "test: dummy\n", -1, NULL));
    yaml_device_t *dev = yaml_parse_from_file(proj_path);
    ck_assert_ptr_nonnull(dev);
    yaml_device_free(dev);
    g_remove(test_yaml);

    /* 5. Broken device in devices/ */
    g_autofree char *bad_dev = g_build_filename(devs_dir, "broken.yaml", NULL);
    ck_assert(g_file_set_contents(bad_dev, "bad: [unclosed\n", -1, NULL));
    const char *proj_with_bad_dev = "version: \"1.0\"\n"
                                    "name: \"t\"\n"
                                    "devices:\n"
                                    "  - device: \"devices/broken.yaml\"\n";
    ck_assert(g_file_set_contents(proj_path, proj_with_bad_dev, -1, NULL));
    ck_assert_ptr_null(yaml_parse_from_file(proj_path));

    g_remove(bad_dev);
    g_remove(proj_path);
    g_rmdir(templates_dir);
    g_rmdir(schemas_dir);
    g_rmdir(flows_dir);
    g_rmdir(codecs_dir);
    g_rmdir(tests_dir);
    g_rmdir(devs_dir);
    g_rmdir(dir);
}
END_TEST

START_TEST(test_loader_single_flow_and_device_flow_fallback)
{
    g_autofree char *dir = g_dir_make_tmp("usbvcr_flow_fallback_XXXXXX", NULL);
    g_autofree char *devs_dir = g_build_filename(dir, "devices", NULL);
    g_autofree char *flows_dir = g_build_filename(dir, "flows", NULL);
    g_mkdir(devs_dir, 0700);
    g_mkdir(flows_dir, 0700);

    /* Valid device */
    g_autofree char *dev_path = g_build_filename(devs_dir, "mydev.yaml", NULL);
    const char *dev_yaml = "device_descriptor:\n"
                           "  bLength: 18\n"
                           "  bDescriptorType: 1\n"
                           "  bcdUSB: 0x0200\n"
                           "  bDeviceClass: 0\n"
                           "  bDeviceSubClass: 0\n"
                           "  bDeviceProtocol: 0\n"
                           "  bMaxPacketSize0: 64\n"
                           "  idVendor: 0x1234\n"
                           "  idProduct: 0x5678\n"
                           "  bcdDevice: 0x0100\n"
                           "  iManufacturer: 0\n"
                           "  iProduct: 0\n"
                           "  iSerialNumber: 0\n"
                           "  bNumConfigurations: 1\n";
    ck_assert(g_file_set_contents(dev_path, dev_yaml, -1, NULL));

    /* 1. Single flow whose name does NOT match device name -> hits single flow fallback */
    g_autofree char *flow_path = g_build_filename(flows_dir, "arbitrary_single_flow.yaml", NULL);
    const char *flow_yaml = "version: \"1.0\"\n"
                            "states:\n"
                            "  - name: \"running\"\n";
    ck_assert(g_file_set_contents(flow_path, flow_yaml, -1, NULL));

    g_autofree char *proj_path = g_build_filename(dir, "project.yaml", NULL);
    const char *proj_yaml = "version: \"1.0\"\n"
                            "name: \"fallback-test\"\n"
                            "devices:\n"
                            "  - device: \"mydev\"\n";
    ck_assert(g_file_set_contents(proj_path, proj_yaml, -1, NULL));

    yaml_device_t *dev = yaml_parse_from_file(proj_path);
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_nonnull(dev->flow);
    yaml_device_free(dev);

    /* 2. Add second flow named mydev_flow.yaml -> size > 1, hits <device>_flow matching */
    g_autofree char *dev_flow_path = g_build_filename(flows_dir, "mydev_flow.yaml", NULL);
    ck_assert(g_file_set_contents(dev_flow_path, flow_yaml, -1, NULL));

    dev = yaml_parse_from_file(proj_path);
    ck_assert_ptr_nonnull(dev);
    ck_assert_ptr_nonnull(dev->flow);
    yaml_device_free(dev);

    g_remove(dev_path);
    g_remove(flow_path);
    g_remove(dev_flow_path);
    g_remove(proj_path);
    g_rmdir(devs_dir);
    g_rmdir(flows_dir);
    g_rmdir(dir);
}
END_TEST

static Suite *
yaml_loader_suite(void)
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
    tcase_add_test(tc_load, test_auto_wire_flow_from_datadir);
    tcase_add_test(tc_load, test_auto_wire_local_flows_dir);
    tcase_add_test(tc_load, test_find_flow_and_entry_null_checks);
    tcase_add_test(tc_load, test_loader_yaml_syntax_and_version_errors);
    tcase_add_test(tc_load, test_loader_boolean_flags_and_speed);
    tcase_add_test(tc_load, test_loader_asset_resolution_paths_and_yml);
    tcase_add_test(tc_load, test_loader_schema_and_flow_parse_errors);
    tcase_add_test(tc_load, test_loader_single_flow_and_device_flow_fallback);
    suite_add_tcase(s, tc_load);

    return s;
}

int
main(void)
{
    SRunner *sr = srunner_create(yaml_loader_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
