/* gmp/c/reader.h - bounds-checked forward cursors over caller supplied buffers (gmsg::ByteReader / ByteWriter).
 *
 *     gmsg_reader r = gmsg_reader_init(data, size);
 *     uint16_t v;
 *     gmsg_error e = gmsg_read_u16(&r, GMSG_ENDIAN_BIG, &v);   // GMSG_ERR_TRUNCATED if fewer than 2 bytes remain
 *
 *     gmsg_writer w = gmsg_writer_init(out, sizeof out);
 *     e = gmsg_write_u16(&w, GMSG_ENDIAN_BIG, v);             // GMSG_ERR_BUFFER_FULL if it does not fit
 *
 * Typed functions exist for u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 and bool. On error nothing is consumed or
 * written and the output parameter is left unchanged. No allocation.
 */
#ifndef GMSG_C_READER_H
#define GMSG_C_READER_H

#include "gmp/c/config.h"
#include "gmp/c/endian.h"
#include "gmp/c/error.h"
#include "gmp/c/mem.h"

GMSG_C_BEGIN

typedef struct gmsg_reader {
    const uint8_t* data;
    size_t size;
    size_t pos;
} gmsg_reader;

typedef struct gmsg_writer {
    uint8_t* data;
    size_t size;
    size_t pos;
} gmsg_writer;

/* ---- reader ------------------------------------------------------------------------------------- */

GMSG_INLINE gmsg_reader gmsg_reader_init(const uint8_t* data, size_t size) {
    gmsg_reader r;
    r.data = data;
    r.size = size;
    r.pos = 0;
    return r;
}

GMSG_INLINE size_t gmsg_reader_remaining(const gmsg_reader* r) { return r->size - r->pos; }
GMSG_INLINE const uint8_t* gmsg_reader_rest(const gmsg_reader* r) { return r->data + r->pos; }

/* Read an unsigned integer of n bytes (1..8). */
GMSG_INLINE gmsg_error gmsg_read_uint(gmsg_reader* r, size_t n, gmsg_endian e, uint64_t* out) {
    if (gmsg_reader_remaining(r) < n) return GMSG_ERR_TRUNCATED;
    *out = gmsg_load_uint(r->data + r->pos, n, e);
    r->pos += n;
    return GMSG_OK;
}

GMSG_INLINE gmsg_error gmsg_read_bool(gmsg_reader* r, bool* out) {
    if (gmsg_reader_remaining(r) < 1) return GMSG_ERR_TRUNCATED;
    *out = gmsg_load_bool(r->data + r->pos);
    r->pos += 1;
    return GMSG_OK;
}

/* Borrow the next n bytes (no copy): *out points into the reader's buffer. */
GMSG_INLINE gmsg_error gmsg_read_bytes(gmsg_reader* r, size_t n, const uint8_t** out) {
    if (gmsg_reader_remaining(r) < n) return GMSG_ERR_TRUNCATED;
    *out = r->data + r->pos;
    r->pos += n;
    return GMSG_OK;
}

/* Copy the next n bytes into dst. */
GMSG_INLINE gmsg_error gmsg_read_copy(gmsg_reader* r, void* dst, size_t n) {
    if (gmsg_reader_remaining(r) < n) return GMSG_ERR_TRUNCATED;
    gmsg_memcpy(dst, r->data + r->pos, n);
    r->pos += n;
    return GMSG_OK;
}

/* Borrow the bytes up to (not including) `terminator`; the terminator is consumed. */
GMSG_INLINE gmsg_error gmsg_read_until(gmsg_reader* r, uint8_t terminator, const uint8_t** out, size_t* len) {
    size_t i;
    for (i = r->pos; i < r->size; ++i) {
        if (r->data[i] == terminator) {
            *out = r->data + r->pos;
            *len = i - r->pos;
            r->pos = i + 1;
            return GMSG_OK;
        }
    }
    return GMSG_ERR_TRUNCATED;
}

GMSG_INLINE gmsg_error gmsg_read_skip(gmsg_reader* r, size_t n) {
    if (gmsg_reader_remaining(r) < n) return GMSG_ERR_TRUNCATED;
    r->pos += n;
    return GMSG_OK;
}

/* ---- writer ------------------------------------------------------------------------------------- */

GMSG_INLINE gmsg_writer gmsg_writer_init(uint8_t* data, size_t size) {
    gmsg_writer w;
    w.data = data;
    w.size = size;
    w.pos = 0;
    return w;
}

GMSG_INLINE size_t gmsg_writer_remaining(const gmsg_writer* w) { return w->size - w->pos; }

GMSG_INLINE gmsg_error gmsg_write_uint(gmsg_writer* w, size_t n, gmsg_endian e, uint64_t v) {
    if (gmsg_writer_remaining(w) < n) return GMSG_ERR_BUFFER_FULL;
    gmsg_store_uint(w->data + w->pos, n, v, e);
    w->pos += n;
    return GMSG_OK;
}

GMSG_INLINE gmsg_error gmsg_write_bool(gmsg_writer* w, bool v) {
    if (gmsg_writer_remaining(w) < 1) return GMSG_ERR_BUFFER_FULL;
    gmsg_store_bool(w->data + w->pos, v);
    w->pos += 1;
    return GMSG_OK;
}

GMSG_INLINE gmsg_error gmsg_write_bytes(gmsg_writer* w, const void* src, size_t n) {
    if (gmsg_writer_remaining(w) < n) return GMSG_ERR_BUFFER_FULL;
    gmsg_memcpy(w->data + w->pos, src, n);
    w->pos += n;
    return GMSG_OK;
}

GMSG_INLINE gmsg_error gmsg_write_fill(gmsg_writer* w, uint8_t value, size_t n) {
    if (gmsg_writer_remaining(w) < n) return GMSG_ERR_BUFFER_FULL;
    gmsg_memset(w->data + w->pos, value, n);
    w->pos += n;
    return GMSG_OK;
}

/* Reserve n zeroed bytes and return their offset in *at (for back-patching length fields). */
GMSG_INLINE gmsg_error gmsg_write_reserve(gmsg_writer* w, size_t n, size_t* at) {
    if (gmsg_writer_remaining(w) < n) return GMSG_ERR_BUFFER_FULL;
    *at = w->pos;
    gmsg_memset(w->data + w->pos, 0, n);
    w->pos += n;
    return GMSG_OK;
}

/* Overwrite n previously written bytes at absolute offset `at`. */
GMSG_INLINE void gmsg_write_patch_uint(gmsg_writer* w, size_t at, size_t n, gmsg_endian e, uint64_t v) {
    GMSG_ASSERT(at + n <= w->pos);
    gmsg_store_uint(w->data + at, n, v, e);
}

/* ---- typed read / write ------------------------------------------------------------------------- */

#define GMSG_DEFINE_READ_WRITE_(name, type, n)                                                           \
    GMSG_INLINE gmsg_error gmsg_read_##name(gmsg_reader* r, gmsg_endian e, type* out) {                         \
        if (gmsg_reader_remaining(r) < (n)) return GMSG_ERR_TRUNCATED;                                    \
        *out = gmsg_load_##name(r->data + r->pos, e);                                                    \
        r->pos += (n);                                                                                  \
        return GMSG_OK;                                                                                  \
    }                                                                                                   \
    GMSG_INLINE gmsg_error gmsg_write_##name(gmsg_writer* w, gmsg_endian e, type v) {                           \
        if (gmsg_writer_remaining(w) < (n)) return GMSG_ERR_BUFFER_FULL;                                  \
        gmsg_store_##name(w->data + w->pos, v, e);                                                       \
        w->pos += (n);                                                                                  \
        return GMSG_OK;                                                                                  \
    }

GMSG_DEFINE_READ_WRITE_(u8, uint8_t, 1)
GMSG_DEFINE_READ_WRITE_(u16, uint16_t, 2)
GMSG_DEFINE_READ_WRITE_(u32, uint32_t, 4)
GMSG_DEFINE_READ_WRITE_(u64, uint64_t, 8)
GMSG_DEFINE_READ_WRITE_(i8, int8_t, 1)
GMSG_DEFINE_READ_WRITE_(i16, int16_t, 2)
GMSG_DEFINE_READ_WRITE_(i32, int32_t, 4)
GMSG_DEFINE_READ_WRITE_(i64, int64_t, 8)
GMSG_DEFINE_READ_WRITE_(f32, float, 4)
GMSG_DEFINE_READ_WRITE_(f64, double, 8)

GMSG_C_END

#endif /* GMSG_C_READER_H */
