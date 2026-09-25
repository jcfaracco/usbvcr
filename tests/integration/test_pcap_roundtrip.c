/*
 * Integration test: the pcap round-trip (golden test).
 *
 * The strongest end-to-end check we have is that a device behaves identically
 * whether emulated from YAML or replayed from a capture of that emulation:
 *
 *     YAML device  --emulate-->  usb_core  --record-->  capture.pcapng
 *     capture.pcapng  --pcap_backend-->  usb_core  --replay-->  transfers'
 *     assert  transfers == transfers'
 *
 * Concretely:
 *   1. yaml_backend_create(example1), drive it through usb_core, and have
 *      usb_core write every transfer to a pcapng via the pcap writer.
 *   2. pcap_backend_create(capture), drive it through the same engine.
 *   3. Compare the two transfer streams byte-for-byte (descriptors, control
 *      replies, endpoint IN data). Equal == round-trip is lossless.
 *
 * Blocked on: pcap reader/writer (src/pcap/pcap.c) and usb_core record hook.
 * Until those land this test only pins the module contract so the wiring and
 * build stay honest; the full comparison is TODO.
 */

#include <stdlib.h>
#include <check.h>

#include "pcap/pcap.h"

/* Contract pin: writer/reader link and behave as stubs today (return NULL).
 * When pcap.c is implemented, replace this with the real record->replay
 * comparison described above. */
START_TEST(test_pcap_roundtrip_pending)
{
    pcap_writer_t *w = pcap_writer_open("/tmp/usbvcr_roundtrip.pcapng");
    ck_assert_msg(w == NULL, "pcap writer implemented: enable the real round-trip test");

    pcap_trace_t *t = pcap_trace_load("/tmp/usbvcr_roundtrip.pcapng");
    ck_assert_msg(t == NULL, "pcap reader implemented: enable the real round-trip test");
}
END_TEST

static Suite *roundtrip_suite(void)
{
    Suite *s = suite_create("pcap_roundtrip");
    TCase *tc = tcase_create("core");
    tcase_add_test(tc, test_pcap_roundtrip_pending);
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
