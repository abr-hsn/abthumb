/* Bounds-checked readers over an in-memory (usually memory-mapped) byte span.
   Every read past the end sets r->err and returns 0: parsers check r->err once per
   structure instead of after every field. Never reads outside [base, base+size). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct abt_rd {
    const uint8_t* base;
    size_t size;
    size_t pos;
    int err;
} abt_rd;

static inline abt_rd abt_rd_make(const uint8_t* p, size_t n) {
    abt_rd r;
    r.base = p; r.size = n; r.pos = 0; r.err = 0;
    return r;
}
static inline size_t abt_rd_left(const abt_rd* r) { return r->pos <= r->size ? r->size - r->pos : 0; }
static inline int abt_rd_has(const abt_rd* r, size_t n) { return !r->err && n <= abt_rd_left(r); }
static inline const uint8_t* abt_rd_ptr(const abt_rd* r) { return r->base + r->pos; }

static inline void abt_rd_seek(abt_rd* r, size_t pos) {
    if (pos > r->size) { r->err = 1; r->pos = r->size; } else r->pos = pos;
}
static inline void abt_rd_skip(abt_rd* r, uint64_t n) {
    if (r->err || n > abt_rd_left(r)) { r->err = 1; r->pos = r->size; } else r->pos += (size_t)n;
}
/* Returns pointer to n bytes and advances, or NULL (and err) if not available. */
static inline const uint8_t* abt_rd_take(abt_rd* r, size_t n) {
    if (!abt_rd_has(r, n)) { r->err = 1; r->pos = r->size; return NULL; }
    const uint8_t* p = r->base + r->pos;
    r->pos += n;
    return p;
}

static inline uint8_t abt_rd_u8(abt_rd* r) {
    const uint8_t* p = abt_rd_take(r, 1);
    return p ? p[0] : 0;
}
static inline uint16_t abt_rd_u16be(abt_rd* r) {
    const uint8_t* p = abt_rd_take(r, 2);
    return p ? (uint16_t)((p[0] << 8) | p[1]) : 0;
}
static inline uint32_t abt_rd_u32be(abt_rd* r) {
    const uint8_t* p = abt_rd_take(r, 4);
    return p ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3] : 0;
}
static inline uint64_t abt_rd_u64be(abt_rd* r) {
    uint64_t hi = abt_rd_u32be(r);
    return (hi << 32) | abt_rd_u32be(r);
}
static inline uint16_t abt_rd_u16le(abt_rd* r) {
    const uint8_t* p = abt_rd_take(r, 2);
    return p ? (uint16_t)(p[0] | (p[1] << 8)) : 0;
}
static inline uint32_t abt_rd_u32le(abt_rd* r) {
    const uint8_t* p = abt_rd_take(r, 4);
    return p ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24) : 0;
}

/* Unaligned loads without bounds checks (caller guarantees range). */
static inline uint16_t abt_load_u16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t abt_load_u32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint32_t abt_load_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* memmem: first occurrence of needle in [hay, hay+hn), or NULL. */
const uint8_t* abt_memmem(const uint8_t* hay, size_t hn, const void* needle, size_t nn);
/* Last occurrence. */
const uint8_t* abt_memrmem(const uint8_t* hay, size_t hn, const void* needle, size_t nn);

#ifdef __cplusplus
}
#endif
