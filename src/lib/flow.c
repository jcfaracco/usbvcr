/*
 * flow.c – Behavior state machine.  STUB (README §9 build step 4).
 *
 * TODO:
 *   - parse states, match/actions rules, transition_state
 *   - bounded when: eval (field compare, current state, counters)
 *   - vars: block + {{ var }} interpolation
 *   - per-endpoint queue_endpoint_data draining
 */

#include "flow.h"

#include <stdlib.h>
#include <glib.h>

struct flow {
    char *state; /* current state name */
    /* TODO: states, rules, queues, vars, counters */
};

flow_t *flow_create(void)
{
    return g_new(flow_t, 1);
}

void flow_free(flow_t *flow)
{
    if (!flow)
        return;

    g_free(flow->state);
    g_free(flow);
}

const char *flow_current_state(flow_t *flow)
{
    return flow ? flow->state : NULL;
}

int flow_control(flow_t *flow, const uint8_t *setup,
                 uint8_t *out, uint16_t *len)
{
    (void)flow;
    (void)setup;
    (void)out;
    (void)len;
    return -1; /* no match */
}

int flow_ep_in(flow_t *flow, uint8_t ep, uint8_t *buf, size_t cap)
{
    (void)flow;
    (void)ep;
    (void)buf;
    (void)cap;
    return 0; /* NAK */
}

int flow_ep_out(flow_t *flow, uint8_t ep, const uint8_t *buf, size_t len)
{
    (void)flow;
    (void)ep;
    (void)buf;
    (void)len;
    return 0;
}

void flow_tick(flow_t *flow)
{
    (void)flow;
}
