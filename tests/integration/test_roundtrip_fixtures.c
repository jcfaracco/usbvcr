/*
 * test_roundtrip_fixtures.c - Generic data-driven conformance test.
 *
 * Dynamically discovers test fixture pairs in tests/data/roundtrip/:
 *   - YAML: <fixture>/main.yaml or <fixture>/<fixture>.yaml
 *   - PCAP: <fixture>.pcap, <fixture>.pcapng, <fixture>/capture.pcap[ng], or <fixture>/<fixture>.pcap[ng]
 *
 * For every discovered fixture, it instantiates both yaml_backend (declarative
 * FSM) and pcap_backend (empirical capture), asserting byte-for-byte parity on:
 *   - Device descriptor
 *   - Configuration descriptor
 *   - Report descriptor (if present)
 *   - SET_CONFIGURATION control transition
 *   - Endpoint IN packet sequences
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include <check.h>

#include "pcap_backend.h"
#include "yaml_backend.h"
#include "protocol.h"
#include "descriptor.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

typedef struct {
    char *name;
    char *yaml_path;
    char *pcap_path;
} fixture_t;

static void fixture_free(gpointer data)
{
    fixture_t *f = (fixture_t *)data;
    if (f == NULL)
        return;
    g_free(f->name);
    g_free(f->yaml_path);
    g_free(f->pcap_path);
    g_free(f);
}

static gint fixture_compare(gconstpointer a, gconstpointer b)
{
    const fixture_t *fa = (const fixture_t *)a;
    const fixture_t *fb = (const fixture_t *)b;
    return g_strcmp0(fa->name, fb->name);
}

static GList *discover_fixtures(const char *base_dir)
{
    GList *fixtures = NULL;
    GDir *dir = g_dir_open(base_dir, 0, NULL);

    if (dir == NULL)
        return NULL;

    const char *entry;
    while ((entry = g_dir_read_name(dir)) != NULL)
    {
        char *sub_dir = g_build_filename(base_dir, entry, NULL);
        if (g_file_test(sub_dir, G_FILE_TEST_IS_DIR))
        {
            char *yaml_path = NULL;
            char *pcap_path = NULL;

            /* Check YAML candidates */
            char *main_yaml = g_build_filename(sub_dir, "main.yaml", NULL);
            char *named_yaml = g_strdup_printf("%s/%s.yaml", sub_dir, entry);
            if (g_file_test(main_yaml, G_FILE_TEST_IS_REGULAR))
                yaml_path = g_strdup(main_yaml);
            else if (g_file_test(named_yaml, G_FILE_TEST_IS_REGULAR))
                yaml_path = g_strdup(named_yaml);
            g_free(main_yaml);
            g_free(named_yaml);

            /* Check PCAP candidates (both adjacent and inside the directory) */
            char *adj_pcap = g_strdup_printf("%s/%s.pcap", base_dir, entry);
            char *adj_pcapng = g_strdup_printf("%s/%s.pcapng", base_dir, entry);
            char *in_pcap = g_strdup_printf("%s/%s.pcap", sub_dir, entry);
            char *in_pcapng = g_strdup_printf("%s/%s.pcapng", sub_dir, entry);
            char *in_capture = g_build_filename(sub_dir, "capture.pcap", NULL);
            char *in_captureng = g_build_filename(sub_dir, "capture.pcapng", NULL);

            if (g_file_test(adj_pcapng, G_FILE_TEST_IS_REGULAR))
                pcap_path = g_strdup(adj_pcapng);
            else if (g_file_test(adj_pcap, G_FILE_TEST_IS_REGULAR))
                pcap_path = g_strdup(adj_pcap);
            else if (g_file_test(in_pcapng, G_FILE_TEST_IS_REGULAR))
                pcap_path = g_strdup(in_pcapng);
            else if (g_file_test(in_pcap, G_FILE_TEST_IS_REGULAR))
                pcap_path = g_strdup(in_pcap);
            else if (g_file_test(in_captureng, G_FILE_TEST_IS_REGULAR))
                pcap_path = g_strdup(in_captureng);
            else if (g_file_test(in_capture, G_FILE_TEST_IS_REGULAR))
                pcap_path = g_strdup(in_capture);

            g_free(adj_pcap);
            g_free(adj_pcapng);
            g_free(in_pcap);
            g_free(in_pcapng);
            g_free(in_capture);
            g_free(in_captureng);

            if (yaml_path != NULL && pcap_path != NULL)
            {
                fixture_t *f = g_new0(fixture_t, 1);
                f->name = g_strdup(entry);
                f->yaml_path = yaml_path;
                f->pcap_path = pcap_path;
                fixtures = g_list_prepend(fixtures, f);
            }
            else
            {
                g_free(yaml_path);
                g_free(pcap_path);
            }
        }
        g_free(sub_dir);
    }
    g_dir_close(dir);

    return g_list_sort(fixtures, fixture_compare);
}

static void run_fixture_conformance(const fixture_t *f)
{
    printf("[fixture] Running conformance test for: %s\n", f->name);
    printf("          YAML: %s\n", f->yaml_path);
    printf("          PCAP: %s\n", f->pcap_path);

    usb_device_t *y_dev = yaml_backend_create(f->yaml_path);
    usb_device_t *p_dev = pcap_backend_create(f->pcap_path);

    ck_assert_msg(y_dev != NULL, "Fixture %s: yaml_backend_create failed on %s", f->name, f->yaml_path);
    ck_assert_msg(p_dev != NULL, "Fixture %s: pcap_backend_create failed on %s", f->name, f->pcap_path);

    /* 1. Device Descriptor Parity */
    const uint8_t *y_desc = NULL, *p_desc = NULL;
    uint16_t y_len = 0, p_len = 0;
    int y_rc = y_dev->ops->get_descriptor(y_dev, USB_DESC_DEVICE, 0, 0, &y_desc, &y_len);
    int p_rc = p_dev->ops->get_descriptor(p_dev, USB_DESC_DEVICE, 0, 0, &p_desc, &p_len);
    ck_assert_msg(y_rc == 0, "Fixture %s: YAML get_descriptor(DEVICE) returned %d", f->name, y_rc);
    ck_assert_msg(p_rc == 0, "Fixture %s: PCAP get_descriptor(DEVICE) returned %d", f->name, p_rc);
    ck_assert_msg(y_len == p_len, "Fixture %s: Device descriptor length mismatch: yaml=%u pcap=%u", f->name, y_len, p_len);
    ck_assert_msg(memcmp(y_desc, p_desc, y_len) == 0, "Fixture %s: Device descriptor bytes mismatch", f->name);

    /* 2. Configuration Descriptor Parity */
    y_desc = NULL; p_desc = NULL;
    y_len = 0; p_len = 0;
    y_rc = y_dev->ops->get_descriptor(y_dev, USB_DESC_CONFIG, 0, 0, &y_desc, &y_len);
    p_rc = p_dev->ops->get_descriptor(p_dev, USB_DESC_CONFIG, 0, 0, &p_desc, &p_len);
    ck_assert_msg(y_rc == 0, "Fixture %s: YAML get_descriptor(CONFIG) returned %d", f->name, y_rc);
    ck_assert_msg(p_rc == 0, "Fixture %s: PCAP get_descriptor(CONFIG) returned %d", f->name, p_rc);
    ck_assert_msg(y_len == p_len, "Fixture %s: Config descriptor length mismatch: yaml=%u pcap=%u", f->name, y_len, p_len);
    ck_assert_msg(memcmp(y_desc, p_desc, y_len) == 0, "Fixture %s: Config descriptor bytes mismatch", f->name);

    /* 3. Class Report Descriptor Parity via Control Request (if present) */
    usb_control_packet_header_t report_setup = {
        .endpoint = 0x00,
        .requesttype = 0x81,
        .request = 0x06,
        .status = 0,
        .value = 0x2200, /* 0x22 = HID Report */
        .index = 0,
        .length = 256
    };
    uint8_t y_rep_data[256], p_rep_data[256];
    uint16_t y_rep_len = 0, p_rep_len = 0;
    y_rc = y_dev->ops->control(y_dev, &report_setup, y_rep_data, &y_rep_len);
    p_rc = p_dev->ops->control(p_dev, &report_setup, p_rep_data, &p_rep_len);
    if (y_rc == 0 || p_rc == 0)
    {
        ck_assert_msg(y_rc == p_rc, "Fixture %s: Report descriptor presence mismatch: yaml_rc=%d pcap_rc=%d", f->name, y_rc, p_rc);
        ck_assert_msg(y_rep_len == p_rep_len, "Fixture %s: Report descriptor length mismatch: yaml=%u pcap=%u", f->name, y_rep_len, p_rep_len);
        ck_assert_msg(memcmp(y_rep_data, p_rep_data, y_rep_len) == 0, "Fixture %s: Report descriptor bytes mismatch", f->name);
    }

    /* 4. SET_CONFIGURATION(1) Transition */
    usb_control_packet_header_t set_cfg = {
        .endpoint = 0x00,
        .requesttype = 0x00,
        .request = 0x09,
        .status = 0,
        .value = 1,
        .index = 0,
        .length = 0
    };
    uint8_t ctrl_data[64];
    uint16_t ctrl_len = 0;
    y_rc = y_dev->ops->control(y_dev, &set_cfg, ctrl_data, &ctrl_len);
    p_rc = p_dev->ops->control(p_dev, &set_cfg, ctrl_data, &ctrl_len);
    ck_assert_msg(y_rc == p_rc, "Fixture %s: SET_CONFIGURATION status mismatch: yaml_rc=%d pcap_rc=%d", f->name, y_rc, p_rc);

    /* 5. Endpoint IN Stream Parity across active endpoints */
    for (uint8_t ep = 0x81; ep <= 0x85; ep++)
    {
        uint8_t y_buf[512], p_buf[512];
        size_t packet_idx = 0;
        while (1)
        {
            int ny = y_dev->ops->ep_in(y_dev, ep, y_buf, sizeof(y_buf));
            int np = p_dev->ops->ep_in(p_dev, ep, p_buf, sizeof(p_buf));

            if (ny <= 0 && np <= 0)
                break;

            ck_assert_msg(ny == np,
                          "Fixture %s EP 0x%02x pkt %zu len mismatch: yaml=%d pcap=%d",
                          f->name, ep, packet_idx, ny, np);
            ck_assert_msg(memcmp(y_buf, p_buf, (size_t)ny) == 0,
                          "Fixture %s EP 0x%02x pkt %zu content mismatch",
                          f->name, ep, packet_idx);
            packet_idx++;
        }
    }

    yaml_backend_destroy(y_dev);
    pcap_backend_destroy(p_dev);
}

START_TEST(test_discovered_roundtrip_fixtures)
{
    const char *base_dir = TEST_DATA_DIR "/roundtrip";
    GList *fixtures = discover_fixtures(base_dir);

    ck_assert_msg(fixtures != NULL, "No test fixtures discovered in %s", base_dir);

    for (GList *l = fixtures; l != NULL; l = l->next)
    {
        const fixture_t *f = (const fixture_t *)l->data;
        run_fixture_conformance(f);
    }

    g_list_free_full(fixtures, fixture_free);
}
END_TEST

static Suite *roundtrip_fixtures_suite(void)
{
    Suite *s = suite_create("roundtrip_fixtures");
    TCase *tc = tcase_create("conformance");

    tcase_add_test(tc, test_discovered_roundtrip_fixtures);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(roundtrip_fixtures_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
