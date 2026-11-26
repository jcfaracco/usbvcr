#include <string.h>

#include "fuzz.h"
#include "protocol.h"
#include "bytes.h"

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

/* Mutate a single descriptor at buf[off] of length blen */
static void mutate_single_descriptor(fuzz_rng_t *r, uint8_t *buf, size_t off,
                                     uint8_t blen, size_t len)
{
    switch (fuzz_rng_below(r, 6)) {
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
    case 4:  /* zero a 16-bit field */
        if (off + 3 < len) {
            buf[off + 2] = 0x00;
            buf[off + 3] = 0x00;
        }
        break;
    case 5:  /* if device descriptor (0x01), corrupt bNumConfigurations at offset 17 */
        if (buf[off + 1] == 0x01 && off + 17 < len) {
            buf[off + 17] = (uint8_t)(fuzz_rng_below(r, 4) + 1);
        }
        break;
    }
}

/* ------------------------------------------------------------------ *
 * Structure-aware descriptor mutation                                 *
 * ------------------------------------------------------------------ */

size_t fuzz_mutate_descriptor(fuzz_rng_t *r, const fuzz_cfg_t *cfg,
                              uint8_t *buf, size_t len, size_t cap)
{
    if (!cfg->fuzz_descriptors || len < 2)
        return len;

    /* Walk descriptor tree, respecting wTotalLength config boundaries */
    size_t off = 0;
    while (off + 2 <= len) {
        uint8_t blen = buf[off];
        uint8_t dtype = buf[off + 1];

        if (blen == 0)
            break;

        /* If this is a configuration descriptor (type 2), respect wTotalLength boundary */
        if (dtype == 0x02 && off + 4 <= len) {
            uint16_t wTotalLength = get_u16le(&buf[off + 2]);
            size_t config_end = off + wTotalLength;

            if (roll(r, cfg->prob)) {
                if (fuzz_rng_below(r, 5) == 4) {
                    /* Truncate whole buffer here */
                    return off + 1 < cap ? off + 1 : cap;
                }
                mutate_single_descriptor(r, buf, off, blen, len);
            }

            /* Walk subordinate descriptors within this configuration */
            size_t sub_off = off + blen;
            size_t limit = (wTotalLength >= blen && config_end <= len) ? config_end : len;

            while (sub_off + 2 <= limit) {
                uint8_t sub_blen = buf[sub_off];
                if (sub_blen == 0)
                    break;

                if (roll(r, cfg->prob)) {
                    if (fuzz_rng_below(r, 5) == 4) {
                        return sub_off + 1 < cap ? sub_off + 1 : cap;
                    }
                    mutate_single_descriptor(r, buf, sub_off, sub_blen, len);
                }
                sub_off += sub_blen;
            }

            /* Advance to the next configuration boundary */
            if (wTotalLength >= blen && config_end <= len) {
                off = config_end;
            } else {
                off += blen;
            }
            continue;
        }

        if (roll(r, cfg->prob)) {
            if (fuzz_rng_below(r, 5) == 4) {
                /* Truncate the whole buffer here */
                return off + 1 < cap ? off + 1 : cap;
            }
            mutate_single_descriptor(r, buf, off, blen, len);
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
