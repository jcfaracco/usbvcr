#ifndef BYTES_H
#define BYTES_H

#include <stdint.h>
#include <string.h>

/*
 * Little-endian wire (de)serialization helpers.
 *
 * memcpy underneath is deliberate: casting a byte pointer to a wider type is
 * undefined behaviour (strict aliasing + unaligned access), whereas memcpy is
 * well-defined and lowers to the same load/store. The width lives in the
 * function name, so call sites carry no magic byte counts.
 *
 * Host is assumed little-endian (see protocol.h). A big-endian port only needs
 * a byte swap added here, not at every call site.
 */

/* Generate a get_/put_ pair for one little-endian scalar type. */
#define BYTES_DEFINE_LE(suffix, type)                                        \
    static inline type get_##suffix(const uint8_t *p)                        \
    { type v; memcpy(&v, p, sizeof v); return v; }                          \
    static inline void put_##suffix(uint8_t *p, type v)                      \
    { memcpy(p, &v, sizeof v); }

BYTES_DEFINE_LE(u16le, uint16_t)
BYTES_DEFINE_LE(u32le, uint32_t)
BYTES_DEFINE_LE(i32le, int32_t)
BYTES_DEFINE_LE(i64le, int64_t)

#undef BYTES_DEFINE_LE

#endif /* BYTES_H */
