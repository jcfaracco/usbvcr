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

/*
 * Big-endian wire (de)serialization helpers (e.g. SCSI CDBs, network framing).
 */
static inline uint16_t get_u16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline void put_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline uint32_t get_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           ((uint32_t)p[3]);
}

static inline void put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline uint64_t get_u64be(const uint8_t *p)
{
    return ((uint64_t)get_u32be(p) << 32) | (uint64_t)get_u32be(p + 4);
}

static inline void put_u64be(uint8_t *p, uint64_t v)
{
    put_u32be(p, (uint32_t)(v >> 32));
    put_u32be(p + 4, (uint32_t)v);
}

#endif /* BYTES_H */
