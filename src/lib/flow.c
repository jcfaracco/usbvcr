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

struct Flow {
    char *state; /* current state name */
    /* TODO: states, rules, queues, vars, counters */
};

Flow *flow_create(void)
{
    return calloc(1, sizeof(Flow));
}

void flow_free(Flow *flow)
{
    if (!flow)
        return;
    free(flow->state);
    free(flow);
}

const char *flow_current_state(Flow *flow)
{
    return flow ? flow->state : NULL;
}

int flow_control(Flow *flow, const uint8_t *setup,
                 uint8_t *out, uint16_t *len)
{
    (void)flow;
    (void)setup;
    (void)out;
    (void)len;
    return -1; /* no match */
}

int flow_ep_in(Flow *flow, uint8_t ep, uint8_t *buf, size_t cap)
{
    (void)flow;
    (void)ep;
    (void)buf;
    (void)cap;
    return 0; /* NAK */
}

int flow_ep_out(Flow *flow, uint8_t ep, const uint8_t *buf, size_t len)
{
    (void)flow;
    (void)ep;
    (void)buf;
    (void)len;
    return 0;
}

void flow_tick(Flow *flow)
{
    (void)flow;
}
