#ifndef USBREDIR_CLIENT_FUZZ_H
#define USBREDIR_CLIENT_FUZZ_H

#include <stddef.h>
#include <stdint.h>

/*
 * Deterministic mutation primitives. Everything is driven by a single 64-bit
 * seed so any finding is reproducible: same seed -> same byte stream.
 */

typedef struct {
    uint64_t state;
} fuzz_rng_t;

void     fuzz_rng_seed(fuzz_rng_t *r, uint64_t seed);
uint64_t fuzz_rng_next(fuzz_rng_t *r);
uint32_t fuzz_rng_below(fuzz_rng_t *r, uint32_t n);   /* [0, n) */

/* Fuzzing configuration. `prob` is a 1-in-N chance applied per mutable item. */
typedef struct {
    uint64_t seed;
    uint32_t prob;          /* e.g. 4 => ~25% of items mutated */
    int      fuzz_descriptors;
    int      fuzz_ep_in;
    int      fuzz_control;
} fuzz_cfg_t;

/*
 * Structure-aware descriptor mutation. Walks the descriptor chain in `buf`
 * (length `len`, capacity `cap`) and randomly corrupts length fields, counts
 * and types, or truncates/extends. Returns the (possibly changed) length.
 */
size_t fuzz_mutate_descriptor(fuzz_rng_t *r, const fuzz_cfg_t *cfg,
                              uint8_t *buf, size_t len, size_t cap);

/* Dumb byte-level mutation of an arbitrary payload (endpoint data, etc.). */
size_t fuzz_mutate_bytes(fuzz_rng_t *r, const fuzz_cfg_t *cfg,
                         uint8_t *buf, size_t len, size_t cap);

#endif
