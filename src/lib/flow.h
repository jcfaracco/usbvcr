/*
 * flow.h – Declarative behavior model (README §3.3, §5).
 *
 * A flow is a state machine loaded from flows/\*.yaml: states with
 * `match`/`actions` rules, `transition_state`, bounded `when:` guards, and
 * per-endpoint data queues. No arbitrary expression eval — deterministic
 * for fuzzing.
 */

#ifndef FLOW_H
#define FLOW_H

#include <stdint.h>
#include <stddef.h>

typedef struct flow flow_t;

flow_t *flow_create(void);
flow_t *flow_load(const char *path);
void  flow_free(flow_t *flow);

/* Current state name (NULL if none). */
const char *flow_current_state(flow_t *flow);

/* Run match rules for an incoming control setup; produce a response into
 * `out` (set *len). Returns 0 = responded, -1 = no match (engine stalls). */
int flow_control(flow_t *flow, const uint8_t *setup,
                 uint8_t *out, uint16_t *len);

/* Drain the current state's queue for endpoint `ep` into `buf` (<= cap).
 * Returns byte count (0 = NAK), -1 on error. */
int flow_ep_in(flow_t *flow, uint8_t ep, uint8_t *buf, size_t cap);

/* Feed OUT data to the flow (e.g. keyboard LED report). */
int flow_ep_out(flow_t *flow, uint8_t ep, const uint8_t *buf, size_t len);

/* Run on_enter actions and advance transitions. */
void flow_tick(flow_t *flow);

#endif /* FLOW_H */
