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
#include <glib.h>

typedef struct flow flow_t;

/* Generic endpoint provider operations (decouples protocol providers like
 * storage BOT/SCSI and MJPEG video streaming from the core flow engine). */
typedef struct flow_endpoint_ops {
    /* Called when IN packet is requested on ep (0 for NAK, > 0 bytes written, -1 unhandled) */
    int  (*produce_in)(void *ctx, uint8_t ep, uint8_t *buf, size_t cap);

    /* Called when OUT packet is received on ep (0 handled, -1 unhandled) */
    int  (*consume_out)(void *ctx, uint8_t ep, const uint8_t *buf, size_t len, GQueue *in_queue);

    /* Called periodically on flow_tick() */
    void (*tick)(void *ctx);

    /* Called when provider is unbound or flow is destroyed (optional) */
    void (*free)(void *ctx);
} flow_endpoint_ops_t;

flow_t *flow_create(void);
flow_t *flow_load(const char *path);
flow_t *flow_load_with_dir(const char *path, const char *base_dir);
void  flow_free(flow_t *flow);

/* Bind an endpoint to a generic provider */
int  flow_bind_endpoint(flow_t *flow, uint8_t ep, const flow_endpoint_ops_t *ops, void *ctx);

/* Unbind a previously bound endpoint provider */
void flow_unbind_endpoint(flow_t *flow, uint8_t ep);

/* Current state name of default machine (NULL if none). */
const char *flow_current_state(flow_t *flow);

/* Current state name of a specific orthogonal machine (NULL if not found). */
const char *flow_machine_state(flow_t *flow, const char *machine_name);

/* Run match rules for an incoming control setup; produce a response into
 * `out` (set *len). Returns 0 = responded, -1 = no match (engine stalls). */
int flow_control(flow_t *flow, const uint8_t *setup,
                 uint8_t *out, uint16_t *len);

/* Drain the current state's queue for endpoint `ep` into `buf` (<= cap).
 * Returns byte count (0 = NAK), -1 on error. */
int flow_ep_in(flow_t *flow, uint8_t ep, uint8_t *buf, size_t cap);

/* Feed OUT data to the flow (e.g. keyboard LED report). */
int flow_ep_out(flow_t *flow, uint8_t ep, const uint8_t *buf, size_t len);

/* Explicitly transition flow to a named state (synchronous).
 * If target is "machine.state", transitions that machine.
 * Otherwise searches default machine first, then other machines.
 * Returns 0 on success, -1 if state unknown. */
int flow_transition_to(flow_t *flow, const char *new_state);

/* Explicitly transition a specific orthogonal machine to a new state (synchronous).
 * Returns 0 on success, -1 if machine or state not found. */
int flow_transition_machine_to(flow_t *flow, const char *machine_name, const char *new_state);

/* Run on_enter actions and advance transitions. */
void flow_tick(flow_t *flow);

#endif /* FLOW_H */
