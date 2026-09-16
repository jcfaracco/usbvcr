#include <string.h>

#include "fuzz.h"
#include "protocol.h"

/* ------------------------------------------------------------------ *
 * PRNG: splitmix64 — tiny, fast, fully determined by the seed.        *
 * ------------------------------------------------------------------ */

void fuzz_rng_seed(fuzz_rng_t *r, uint64_t seed)
{
    r->state = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

uint64_t fuzz_rng_next(fuzz_rng_t *r)
{
    uint64_t z = (r->state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

uint32_t fuzz_rng_below(fuzz_rng_t *r, uint32_t n)
{
    return n ? (uint32_t)(fuzz_rng_next(r) % n) : 0;
}

/* 1-in-`prob` chance (prob<=1 => always). */
static int roll(fuzz_rng_t *r, uint32_t prob)
{
    if (prob <= 1)
        return 1;
    return fuzz_rng_below(r, prob) == 0;
}

/* ------------------------------------------------------------------ *
 * Structure-aware descriptor mutation                                 *
 * ------------------------------------------------------------------ */

size_t fuzz_mutate_descriptor(fuzz_rng_t *r, const fuzz_cfg_t *cfg,
                              uint8_t *buf, size_t len, size_t cap)
{
    if (!cfg->fuzz_descriptors || len < 2)
        return len;

    /* Walk each descriptor (bLength, bDescriptorType, ...). */
    size_t off = 0;
    while (off + 2 <= len) {
        uint8_t blen = buf[off];
        if (blen == 0)
            break;

        if (roll(r, cfg->prob)) {
            switch (fuzz_rng_below(r, 5)) {
            case 0:  /* corrupt bLength (classic over/under-read) */
                buf[off] = (uint8_t)fuzz_rng_next(r);
                break;
            case 1:  /* corrupt bDescriptorType */
                buf[off + 1] = (uint8_t)fuzz_rng_next(r);
                break;
            case 2:  /* flip a byte inside the descriptor body */
                if (blen > 2) {
                    size_t p = off + 2 + fuzz_rng_below(r, blen - 2);
                    if (p < len)
                        buf[p] ^= (uint8_t)(1u << fuzz_rng_below(r, 8));
                }
                break;
            case 3:  /* maximise a 16-bit field (wTotalLength / wMaxPacketSize) */
                if (off + 3 < len) {
                    buf[off + 2] = 0xff;
                    buf[off + 3] = 0xff;
                }
                break;
            case 4:  /* truncate the whole buffer here */
                return off + 1 < cap ? off + 1 : cap;
            }
        }

        off += blen;
    }

    /* Occasionally extend past the real end with garbage (extra descriptors). */
    if (roll(r, cfg->prob) && len < cap) {
        size_t extra = fuzz_rng_below(r, (uint32_t)(cap - len));
        for (size_t i = 0; i < extra; i++)
            buf[len + i] = (uint8_t)fuzz_rng_next(r);
        len += extra;
    }

    return len;
}

/* ------------------------------------------------------------------ *
 * Byte-level mutation                                                 *
 * ------------------------------------------------------------------ */

size_t fuzz_mutate_bytes(fuzz_rng_t *r, const fuzz_cfg_t *cfg,
                         uint8_t *buf, size_t len, size_t cap)
{
    (void)cap;
    if (len == 0)
        return len;

    for (size_t i = 0; i < len; i++)
        if (roll(r, cfg->prob))
            buf[i] = (uint8_t)fuzz_rng_next(r);

    return len;
}
