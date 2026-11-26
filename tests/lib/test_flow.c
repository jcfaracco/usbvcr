#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <string.h>
#include <glib.h>

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

START_TEST(test_flow_stream_from_file_state_machine)
{
    flow_t *f = flow_load(TEST_DATA_DIR "/stream/streaming_flow.yaml");
    ck_assert_ptr_nonnull(f);

    /* 1. Initial state is 'idle' */
    ck_assert_str_eq(flow_current_state(f), "idle");

    /* EP 0x81 should NAK in idle */
    uint8_t buf[64];
    int n = flow_ep_in(f, 0x81, buf, sizeof(buf));
    ck_assert_int_eq(n, 0);

    /* 2. SET_INTERFACE altsetting=1 -> transition to 'streaming' */
    uint8_t setup_start[8] = { 0x01, 0x0B, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00 };
    uint8_t out[128];
    uint16_t out_len = 0;
    int rc = flow_control(f, setup_start, out, &out_len);
    ck_assert_int_eq(rc, 0);

    /* Synchronous transition: state is immediately 'streaming' without waiting for flow_tick */
    ck_assert_str_eq(flow_current_state(f), "streaming");

    flow_tick(f);
    ck_assert_str_eq(flow_current_state(f), "streaming");

    /* 3. EP 0x81 should now produce stream packet with UVC header */
    n = flow_ep_in(f, 0x81, buf, sizeof(buf));
    ck_assert_int_eq(n, 8); /* 2 byte header + 6 byte frame */
    ck_assert_int_eq(buf[0], 0x02); /* UVC header len */
    ck_assert_int_eq(buf[1], 0x02); /* FID=0, EOF=1 */
    ck_assert_int_eq(buf[2], 0xFF); /* SOI */
    ck_assert_int_eq(buf[3], 0xD8);
    ck_assert_int_eq(buf[6], 0xFF); /* EOI */
    ck_assert_int_eq(buf[7], 0xD9);

    /* 4. SET_INTERFACE altsetting=0 -> transition back to 'idle' */
    uint8_t setup_stop[8] = { 0x01, 0x0B, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
    rc = flow_control(f, setup_stop, out, &out_len);
    ck_assert_int_eq(rc, 0);

    /* Synchronous transition: state is immediately 'idle' without waiting for flow_tick */
    ck_assert_str_eq(flow_current_state(f), "idle");

    flow_tick(f);
    ck_assert_str_eq(flow_current_state(f), "idle");

    /* EP 0x81 should NAK again */
    n = flow_ep_in(f, 0x81, buf, sizeof(buf));
    ck_assert_int_eq(n, 0);

    flow_free(f);
}
END_TEST

START_TEST(test_flow_sync_transition_on_packet)
{
    flow_t *f = flow_load(TEST_DATA_DIR "/flow_transition/transition_flow.yaml");
    ck_assert_ptr_nonnull(f);

    /* Initial state */
    ck_assert_str_eq(flow_current_state(f), "init");

    /* EP 0x81 has nothing queued yet */
    uint8_t buf[64];
    int n = flow_ep_in(f, 0x81, buf, sizeof(buf));
    ck_assert_int_eq(n, 0);

    /* Send request 0x40/0x01: transitions to 'armed' and queues '01020304' in on_enter */
    uint8_t setup_arm[8] = { 0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t out[128];
    uint16_t out_len = 0;
    int rc = flow_control(f, setup_arm, out, &out_len);
    ck_assert_int_eq(rc, 0);

    /* State transition must be synchronous: immediately 'armed' without flow_tick() */
    ck_assert_str_eq(flow_current_state(f), "armed");

    /* on_enter actions must have executed synchronously: endpoint queue is immediately available */
    n = flow_ep_in(f, 0x81, buf, sizeof(buf));
    ck_assert_int_eq(n, 4);
    ck_assert_int_eq(buf[0], 0x01);
    ck_assert_int_eq(buf[1], 0x02);
    ck_assert_int_eq(buf[2], 0x03);
    ck_assert_int_eq(buf[3], 0x04);

    /* Send request 0x40/0x02: transitions to 'disarmed' */
    uint8_t setup_disarm[8] = { 0x40, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    rc = flow_control(f, setup_disarm, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_str_eq(flow_current_state(f), "disarmed");

    flow_free(f);
}
END_TEST

START_TEST(test_flow_transition_to_api)
{
    flow_t *f = flow_load(TEST_DATA_DIR "/flow_transition/transition_flow.yaml");
    ck_assert_ptr_nonnull(f);

    /* Invalid parameters */
    ck_assert_int_eq(flow_transition_to(NULL, "armed"), -1);
    ck_assert_int_eq(flow_transition_to(f, NULL), -1);
    ck_assert_int_eq(flow_transition_to(f, "nonexistent"), -1);

    /* Valid transition */
    ck_assert_int_eq(flow_transition_to(f, "armed"), 0);
    ck_assert_str_eq(flow_current_state(f), "armed");

    /* on_enter queued 4 bytes */
    uint8_t buf[16];
    int n = flow_ep_in(f, 0x81, buf, sizeof(buf));
    ck_assert_int_eq(n, 4);

    /* Transitioning to the same state is a no-op */
    ck_assert_int_eq(flow_transition_to(f, "armed"), 0);
    ck_assert_str_eq(flow_current_state(f), "armed");

    flow_free(f);
}
END_TEST

START_TEST(test_flow_rules_inheritance)
{
    flow_t *f = flow_load(TEST_DATA_DIR "/flow_rules/rules_inheritance.yaml");
    ck_assert_ptr_nonnull(f);

    uint8_t out[128];
    uint16_t out_len = 0;
    uint8_t buf[16];
    int rc;

    /* Setup packets */
    uint8_t setup_common[8]   = { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00 };
    uint8_t setup_rule_a[8]   = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x04, 0x00 }; /* 0x0100 */
    uint8_t setup_rule_b[8]   = { 0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0x04, 0x00 }; /* 0x0200 */
    uint8_t setup_rule_c[8]   = { 0x80, 0x06, 0x00, 0x03, 0x00, 0x00, 0x04, 0x00 }; /* 0x0300 */
    uint8_t setup_rule_d[8]   = { 0x80, 0x06, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00 }; /* 0x0400 */
    uint8_t setup_to_derived[8] = { 0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t setup_to_standalone[8] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    /* 1. Initial state is 'base' */
    ck_assert_str_eq(flow_current_state(f), "base");

    /* Common rule works in 'base' */
    rc = flow_control(f, setup_common, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(out_len, 2);
    ck_assert_int_eq(out[0], 0x01);
    ck_assert_int_eq(out[1], 0x00);

    /* Rule A works in 'base' */
    rc = flow_control(f, setup_rule_a, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(out_len, 4);
    ck_assert_int_eq(out[0], 0x12);
    ck_assert_int_eq(out[3], 0x02);

    /* Rule B returns base version */
    rc = flow_control(f, setup_rule_b, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(out_len, 4);
    ck_assert_int_eq(out[2], 0x20);

    /* Rule C is NOT in 'base' */
    rc = flow_control(f, setup_rule_c, out, &out_len);
    ck_assert_int_eq(rc, -1);

    /* Common ep_out works in 'base' */
    uint8_t out_pkt[1] = { 0x00 };
    flow_ep_out(f, 0x02, out_pkt, sizeof(out_pkt));
    int n = flow_ep_in(f, 0x82, buf, sizeof(buf));
    ck_assert_int_eq(n, 2);
    ck_assert_int_eq(buf[0], 0xAA);
    ck_assert_int_eq(buf[1], 0xBB);

    /* 2. Transition to 'derived' (extends 'base') */
    rc = flow_control(f, setup_to_derived, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_str_eq(flow_current_state(f), "derived");

    /* Common rule works in 'derived' */
    rc = flow_control(f, setup_common, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(out_len, 2);

    /* Rule A is inherited from 'base' via extends */
    rc = flow_control(f, setup_rule_a, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(out_len, 4);
    ck_assert_int_eq(out[0], 0x12);

    /* Rule B is overridden by 'derived' */
    rc = flow_control(f, setup_rule_b, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(out_len, 4);
    ck_assert_int_eq(out[2], 0xFF);
    ck_assert_int_eq(out[3], 0xFF);

    /* Rule C is defined in 'derived' */
    rc = flow_control(f, setup_rule_c, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(out_len, 4);
    ck_assert_int_eq(out[0], 0x04);

    /* 3. Transition to 'standalone' (does NOT extend 'base') */
    rc = flow_control(f, setup_to_standalone, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_str_eq(flow_current_state(f), "standalone");

    /* Common rule works in 'standalone' */
    rc = flow_control(f, setup_common, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(out_len, 2);

    /* Rule D is defined in 'standalone' */
    rc = flow_control(f, setup_rule_d, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(out_len, 4);
    ck_assert_int_eq(out[0], 0x08);

    /* Rules A, B, C are NOT present in 'standalone' */
    rc = flow_control(f, setup_rule_a, out, &out_len);
    ck_assert_int_eq(rc, -1);
    rc = flow_control(f, setup_rule_b, out, &out_len);
    ck_assert_int_eq(rc, -1);
    rc = flow_control(f, setup_rule_c, out, &out_len);
    ck_assert_int_eq(rc, -1);

    /* Common ep_out also works in 'standalone' */
    flow_ep_out(f, 0x02, out_pkt, sizeof(out_pkt));
    n = flow_ep_in(f, 0x82, buf, sizeof(buf));
    ck_assert_int_eq(n, 2);
    ck_assert_int_eq(buf[0], 0xAA);
    ck_assert_int_eq(buf[1], 0xBB);

    flow_free(f);
}
END_TEST

START_TEST(test_flow_orthogonal_state_machines)
{
    flow_t *f = flow_load(TEST_DATA_DIR "/flow_orthogonal/orthogonal_flow.yaml");
    ck_assert_ptr_nonnull(f);

    uint8_t out[128];
    uint16_t out_len = 0;
    uint8_t buf[16];
    int rc;

    /* 1. Initial states of orthogonal machines */
    ck_assert_str_eq(flow_current_state(f), "configured");
    ck_assert_str_eq(flow_machine_state(f, "video_control"), "configured");
    ck_assert_str_eq(flow_machine_state(f, "video_streaming"), "idle");
    ck_assert_str_eq(flow_machine_state(f, "audio_streaming"), "muted");
    ck_assert_ptr_null(flow_machine_state(f, "nonexistent"));

    /* Common rule works */
    uint8_t setup_common[8] = { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00 };
    rc = flow_control(f, setup_common, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(out_len, 2);
    ck_assert_int_eq(out[0], 0x01);
    ck_assert_int_eq(out[1], 0x00);

    /* Video control rule works */
    uint8_t setup_vc[8] = { 0xA1, 0x81, 0x00, 0x01, 0x00, 0x00, 0x02, 0x00 };
    rc = flow_control(f, setup_vc, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(out_len, 2);
    ck_assert_int_eq(out[0], 0x11);
    ck_assert_int_eq(out[1], 0x22);

    /* Neither streaming machine should have queued packets yet */
    ck_assert_int_eq(flow_ep_in(f, 0x81, buf, sizeof(buf)), 0);
    ck_assert_int_eq(flow_ep_in(f, 0x82, buf, sizeof(buf)), 0);

    /* 2. Transition video_streaming to 'streaming' via SET_INTERFACE alt=1 on iface 1 */
    uint8_t setup_vs_start[8] = { 0x01, 0x0B, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00 };
    rc = flow_control(f, setup_vs_start, out, &out_len);
    ck_assert_int_eq(rc, 0);

    /* video_streaming transitioned synchronously, but audio_streaming stayed muted */
    ck_assert_str_eq(flow_machine_state(f, "video_streaming"), "streaming");
    ck_assert_str_eq(flow_machine_state(f, "audio_streaming"), "muted");
    ck_assert_str_eq(flow_machine_state(f, "video_control"), "configured");

    /* Video streaming on_enter queued DEADBEEF */
    int n = flow_ep_in(f, 0x81, buf, sizeof(buf));
    ck_assert_int_eq(n, 4);
    ck_assert_int_eq(buf[0], 0xDE);
    ck_assert_int_eq(buf[1], 0xAD);
    ck_assert_int_eq(buf[2], 0xBE);
    ck_assert_int_eq(buf[3], 0xEF);

    /* Audio streaming queue is still empty */
    ck_assert_int_eq(flow_ep_in(f, 0x82, buf, sizeof(buf)), 0);

    /* 3. Transition audio_streaming to 'unmuted' via SET_CUR on iface 2 */
    uint8_t setup_as_unmute[8] = { 0x21, 0x01, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00 };
    rc = flow_control(f, setup_as_unmute, out, &out_len);
    ck_assert_int_eq(rc, 0);

    /* audio_streaming is now unmuted, while video_streaming remains streaming */
    ck_assert_str_eq(flow_machine_state(f, "audio_streaming"), "unmuted");
    ck_assert_str_eq(flow_machine_state(f, "video_streaming"), "streaming");

    /* Audio streaming on_enter queued CAFEBABE */
    n = flow_ep_in(f, 0x82, buf, sizeof(buf));
    ck_assert_int_eq(n, 4);
    ck_assert_int_eq(buf[0], 0xCA);
    ck_assert_int_eq(buf[1], 0xFE);
    ck_assert_int_eq(buf[2], 0xBA);
    ck_assert_int_eq(buf[3], 0xBE);

    /* 4. Transition audio_streaming back to muted via dot-notation rule */
    uint8_t setup_as_mute[8] = { 0x21, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };
    rc = flow_control(f, setup_as_mute, out, &out_len);
    ck_assert_int_eq(rc, 0);
    ck_assert_str_eq(flow_machine_state(f, "audio_streaming"), "muted");
    ck_assert_str_eq(flow_machine_state(f, "video_streaming"), "streaming");

    /* 5. Direct API transitions */
    ck_assert_int_eq(flow_transition_machine_to(f, "video_streaming", "idle"), 0);
    ck_assert_str_eq(flow_machine_state(f, "video_streaming"), "idle");

    ck_assert_int_eq(flow_transition_machine_to(f, "nonexistent", "idle"), -1);
    ck_assert_int_eq(flow_transition_machine_to(f, "video_streaming", "nonexistent"), -1);

    /* Transition via dot-notation in flow_transition_to */
    ck_assert_int_eq(flow_transition_to(f, "audio_streaming.unmuted"), 0);
    ck_assert_str_eq(flow_machine_state(f, "audio_streaming"), "unmuted");

    flow_free(f);
}
END_TEST

typedef struct {
    int produce_called;
    int consume_called;
    int tick_called;
    int free_called;
    uint8_t last_consumed_byte;
} mock_provider_ctx_t;

static int mock_produce_in(void *ctx, uint8_t ep, uint8_t *buf, size_t cap)
{
    (void)ep;
    mock_provider_ctx_t *m = ctx;
    m->produce_called++;
    if (cap < 4) return -1;
    buf[0] = 0xAA;
    buf[1] = 0xBB;
    buf[2] = 0xCC;
    buf[3] = 0xDD;
    return 4;
}

static int mock_consume_out(void *ctx, uint8_t ep, const uint8_t *buf, size_t len, GQueue *in_queue)
{
    (void)ep;
    (void)in_queue;
    mock_provider_ctx_t *m = ctx;
    m->consume_called++;
    if (len > 0)
        m->last_consumed_byte = buf[0];
    return 0;
}

static void mock_tick(void *ctx)
{
    mock_provider_ctx_t *m = ctx;
    m->tick_called++;
}

static void mock_free(void *ctx)
{
    mock_provider_ctx_t *m = ctx;
    m->free_called++;
}

static const flow_endpoint_ops_t mock_ops = {
    .produce_in = mock_produce_in,
    .consume_out = mock_consume_out,
    .tick = mock_tick,
    .free = mock_free,
};

START_TEST(test_flow_endpoint_provider_binding)
{
    flow_t *f = flow_create();
    mock_provider_ctx_t ctx1 = {0};
    mock_provider_ctx_t ctx2 = {0};
    uint8_t buf[64];
    uint8_t out_pkt[2] = { 0x42, 0x43 };
    int n;

    /* Invalid args */
    ck_assert_int_eq(flow_bind_endpoint(NULL, 0x82, &mock_ops, &ctx1), -1);
    ck_assert_int_eq(flow_bind_endpoint(f, 0x82, NULL, &ctx1), -1);

    /* Bind ctx1 to ep 0x82 */
    ck_assert_int_eq(flow_bind_endpoint(f, 0x82, &mock_ops, &ctx1), 0);

    /* Produce on ep 0x82 */
    n = flow_ep_in(f, 0x82, buf, sizeof(buf));
    ck_assert_int_eq(n, 4);
    ck_assert_int_eq(buf[0], 0xAA);
    ck_assert_int_eq(ctx1.produce_called, 1);

    /* Consume on ep 0x82 */
    ck_assert_int_eq(flow_ep_out(f, 0x82, out_pkt, sizeof(out_pkt)), 0);
    ck_assert_int_eq(ctx1.consume_called, 1);
    ck_assert_int_eq(ctx1.last_consumed_byte, 0x42);

    /* Tick calls provider tick */
    flow_tick(f);
    ck_assert_int_eq(ctx1.tick_called, 1);

    /* Unbind ep 0x82 -> free is called */
    flow_unbind_endpoint(f, 0x82);
    ck_assert_int_eq(ctx1.free_called, 1);

    /* Subsequent in on 0x82 returns 0 (no queue, no provider) */
    n = flow_ep_in(f, 0x82, buf, sizeof(buf));
    ck_assert_int_eq(n, 0);

    /* Wildcard binding on ep 0 */
    ck_assert_int_eq(flow_bind_endpoint(f, 0, &mock_ops, &ctx2), 0);
    n = flow_ep_in(f, 0x85, buf, sizeof(buf));
    ck_assert_int_eq(n, 4);
    ck_assert_int_eq(ctx2.produce_called, 1);

    /* Freeing flow destroys wildcard binding and calls free */
    flow_free(f);
    ck_assert_int_eq(ctx2.free_called, 1);
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
    tcase_add_test(tc_core, test_flow_stream_from_file_state_machine);
    tcase_add_test(tc_core, test_flow_sync_transition_on_packet);
    tcase_add_test(tc_core, test_flow_transition_to_api);
    tcase_add_test(tc_core, test_flow_rules_inheritance);
    tcase_add_test(tc_core, test_flow_orthogonal_state_machines);
    tcase_add_test(tc_core, test_flow_endpoint_provider_binding);
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
