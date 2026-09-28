#include <stdlib.h>
#include <check.h>
#include <string.h>

#include "flow.h"

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "../data"
#endif

START_TEST(test_flow_load_null)
{
    ck_assert_ptr_null(flow_load(NULL));
}
END_TEST

START_TEST(test_flow_load_missing)
{
    ck_assert_ptr_null(flow_load("/nonexistent/flow.yaml"));
}
END_TEST

START_TEST(test_flow_load_example1)
{
    flow_t *f = flow_load(TEST_DATA_DIR "/example1/flows/hello_world_flow.yaml");
    ck_assert_ptr_nonnull(f);

    /* Single operational state: the flow starts here and stays here so the
     * descriptor and HID class rules remain live past enumeration (the host
     * re-fetches the report descriptor / sends SET_IDLE after binding). */
    const char *state = flow_current_state(f);
    ck_assert_ptr_nonnull(state);
    ck_assert_str_eq(state, "operational");

    /* SET_CONFIGURATION: ACKs and queues the HID report payloads. */
    uint8_t setup[8] = { 0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t out[1024];
    uint16_t len = 0;

    int rc = flow_control(f, setup, out, &len);
    ck_assert_int_eq(rc, 0); /* match handled */

    flow_tick(f);

    /* No transition away from operational. */
    state = flow_current_state(f);
    ck_assert_str_eq(state, "operational");

    /* Fetch endpoint 0x81 queue data */
    uint8_t ep_buf[8];
    rc = flow_ep_in(f, 0x81, ep_buf, sizeof(ep_buf));
    ck_assert_int_gt(rc, 0); /* Expecting data, 8 bytes probably */
    
    /* First hex payload is "02000B0000000000" (8 bytes) */
    ck_assert_int_eq(rc, 8);
    ck_assert_int_eq(ep_buf[0], 0x02);
    ck_assert_int_eq(ep_buf[2], 0x0B);

    flow_free(f);
}
END_TEST

START_TEST(test_flow_free_null)
{
    flow_free(NULL);
}
END_TEST

static Suite *flow_suite(void)
{
    Suite *s = suite_create("flow");

    TCase *tc_core = tcase_create("core");
    tcase_add_test(tc_core, test_flow_load_null);
    tcase_add_test(tc_core, test_flow_load_missing);
    tcase_add_test(tc_core, test_flow_load_example1);
    tcase_add_test(tc_core, test_flow_free_null);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(flow_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
