/* gmp/c/types.h - small value types used by generated C code.
 *
 *   gmsg_bytes   borrowed byte range { data, size } (gmp::bytes_view)
 *   gmsg_str     borrowed character range { data, size }, not NUL terminated (std::string_view)
 *   gmsg_round_i64(x)  round half away from zero, like llround(), without <math.h>
 */
#ifndef GMSG_C_TYPES_H
#define GMSG_C_TYPES_H

#include "gmp/c/config.h"

GMSG_C_BEGIN

typedef struct gmsg_bytes {
    const uint8_t* data;
    size_t size;
} gmsg_bytes;

typedef struct gmsg_str {
    const char* data;
    size_t size;
} gmsg_str;

GMSG_INLINE gmsg_bytes gmsg_bytes_make(const uint8_t* data, size_t size) {
    gmsg_bytes b;
    b.data = data;
    b.size = size;
    return b;
}

GMSG_INLINE gmsg_str gmsg_str_make(const uint8_t* data, size_t size) {
    gmsg_str s;
    s.data = (const char*)data;
    s.size = size;
    return s;
}

/* Drop trailing `pad` characters (fixed-length strings padded on the wire). */
GMSG_INLINE gmsg_str gmsg_str_strip(gmsg_str s, char pad) {
    while (s.size > 0 && s.data[s.size - 1] == pad) --s.size;
    return s;
}

/* Round half away from zero (same results as llround for values that fit in int64_t). */
GMSG_INLINE int64_t gmsg_round_i64(double x) {
    int64_t t = (int64_t)x; /* truncates toward zero */
    const double frac = x - (double)t;
    if (frac >= 0.5) ++t;
    else if (frac <= -0.5) --t;
    return t;
}

GMSG_C_END

#endif /* GMSG_C_TYPES_H */
