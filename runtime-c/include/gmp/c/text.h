/* gmp/c/text.h - tokenizer and text codecs for ASCII protocols (port of gmp/text.hpp; same results).
 *
 *   gmsg_split(s, n, sep, tokens, max)       split on sep, keeping empty tokens; returns the token count (may
 *                                            exceed max: extra tokens are not stored)
 *   gmsg_parse_i64 / gmsg_parse_u64          integer in a base, range-checked like std::from_chars into the target
 *                                            type: GMSG_ERR_MISSING_FIELD (empty), _PARSE, _OUT_OF_RANGE
 *   gmsg_parse_char, gmsg_parse_hex_byte, gmsg_hex_digit
 *   gmsg_text_writer                         writes tokens with separators into a caller buffer
 *   floats: gmp/c/float_text.h
 */
#ifndef GMSG_C_TEXT_H
#define GMSG_C_TEXT_H

#include "gmp/c/config.h"
#include "gmp/c/error.h"
#include "gmp/c/float_text.h"
#include "gmp/c/types.h"

GMSG_C_BEGIN

GMSG_API size_t gmsg_split(const char* s, size_t n, char sep, gmsg_str* tokens, size_t max);

/* Signed integer in [min, max] (as std::from_chars into a type of that range, after an optional '+'). */
GMSG_API gmsg_error gmsg_parse_i64(const char* s, size_t n, int base, int64_t min, int64_t max, int64_t* out);

/* Unsigned integer in [0, max]; a '-' sign is a parse error (as std::from_chars for unsigned types). */
GMSG_API gmsg_error gmsg_parse_u64(const char* s, size_t n, int base, uint64_t max, uint64_t* out);
GMSG_API gmsg_error gmsg_parse_char(const char* s, size_t n, char* out);

/* Exactly two hex digits (either case). */
GMSG_API gmsg_error gmsg_parse_hex_byte(const char* s, size_t n, uint8_t* out);
GMSG_API char gmsg_hex_digit(unsigned v, bool upper);

/* ---- writer ------------------------------------------------------------------------------------------ */

typedef struct gmsg_text_writer {
    char* out;
    size_t cap;
    size_t pos;
    char sep;
    bool first;
} gmsg_text_writer;

GMSG_API gmsg_text_writer gmsg_text_writer_init(uint8_t* out, size_t cap, char sep);

/* Start a new token (writes the separator unless this is the first token). */
GMSG_API gmsg_error gmsg_text_begin_token(gmsg_text_writer* w);

/* Raw characters inside the current token (custom text encoders write their token with this). */
GMSG_API gmsg_error gmsg_text_raw(gmsg_text_writer* w, const char* s, size_t n);
GMSG_API gmsg_error gmsg_text_empty_token(gmsg_text_writer* w);
GMSG_API gmsg_error gmsg_text_token(gmsg_text_writer* w, const char* s, size_t n);
GMSG_API gmsg_error gmsg_text_char_token(gmsg_text_writer* w, char c);
GMSG_API gmsg_error gmsg_text_i64_token(gmsg_text_writer* w, int64_t v, int base, int width);
GMSG_API gmsg_error gmsg_text_u64_token(gmsg_text_writer* w, uint64_t v, int base, int width);

#if GMSG_FLOAT_TEXT != GMSG_FLOAT_TEXT_NONE

GMSG_API gmsg_error gmsg_text_f64_token(gmsg_text_writer* w, double v, int precision, int width);
GMSG_API gmsg_error gmsg_text_f32_token(gmsg_text_writer* w, float v, int precision, int width);
#endif

/* ---- implementation (header-only build, or runtime-c/src/text.c of the library build) ---------------------------------- */

#if GMSG_DEFINE_FUNCTIONS_ || defined(GMSG_IMPL_TEXT_)

GMSG_API size_t gmsg_split(const char* s, size_t n, char sep, gmsg_str* tokens, size_t max) {
    size_t count = 0, start = 0, i;
    for (i = 0; i <= n; ++i) {
        if (i == n || s[i] == sep) {
            if (count < max) {
                tokens[count].data = s + start;
                tokens[count].size = i - start;
            }
            ++count;
            start = i + 1;
        }
    }
    for (i = count; i < max; ++i) {
        tokens[i].data = s + n;
        tokens[i].size = 0;
    }
    return count;
}

GMSG_PRIV_ int gmsg_digit_value_(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* Digits of s[0..n) in `base` as an unsigned magnitude. *consumed = digits read; *overflow when > limit. */
GMSG_PRIV_ uint64_t gmsg_parse_mag_(const char* s, size_t n, int base, uint64_t limit, size_t* consumed, bool* overflow) {
    uint64_t v = 0;
    size_t i;
    *overflow = false;
    for (i = 0; i < n; ++i) {
        const int d = gmsg_digit_value_(s[i]);
        if (d >= base) break;
        if (!*overflow) {
            if (v > (limit - (uint64_t)d) / (uint64_t)base) *overflow = true;
            else v = v * (uint64_t)base + (uint64_t)d;
        }
    }
    *consumed = i;
    return v;
}

GMSG_API gmsg_error gmsg_parse_i64(const char* s, size_t n, int base, int64_t min, int64_t max, int64_t* out) {
    bool neg = false, overflow = false;
    size_t used = 0;
    uint64_t mag, limit;
    if (n == 0) return GMSG_ERR_MISSING_FIELD;
    if (s[0] == '+') {
        ++s;
        --n;
    }
    if (n > 0 && s[0] == '-') {
        neg = true;
        ++s;
        --n;
    }
    limit = neg ? (uint64_t)(-(min + 1)) + 1u : (uint64_t)max;
    mag = gmsg_parse_mag_(s, n, base, limit, &used, &overflow);
    if (used == 0) return GMSG_ERR_PARSE;
    if (overflow) return GMSG_ERR_OUT_OF_RANGE;
    if (used != n) return GMSG_ERR_PARSE;
    *out = neg ? (mag == 0 ? 0 : -(int64_t)(mag - 1u) - 1) : (int64_t)mag;
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_parse_u64(const char* s, size_t n, int base, uint64_t max, uint64_t* out) {
    bool overflow = false;
    size_t used = 0;
    uint64_t mag;
    if (n == 0) return GMSG_ERR_MISSING_FIELD;
    if (s[0] == '+') {
        ++s;
        --n;
    }
    mag = gmsg_parse_mag_(s, n, base, max, &used, &overflow);
    if (used == 0) return GMSG_ERR_PARSE;
    if (overflow) return GMSG_ERR_OUT_OF_RANGE;
    if (used != n) return GMSG_ERR_PARSE;
    *out = mag;
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_parse_char(const char* s, size_t n, char* out) {
    if (n == 0) return GMSG_ERR_MISSING_FIELD;
    if (n != 1) return GMSG_ERR_PARSE;
    *out = s[0];
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_parse_hex_byte(const char* s, size_t n, uint8_t* out) {
    uint64_t v = 0;
    gmsg_error e;
    if (n != 2) return GMSG_ERR_PARSE;
    e = gmsg_parse_u64(s, n, 16, 0xFFu, &v);
    if (e == GMSG_OK) *out = (uint8_t)v;
    return e;
}

GMSG_API char gmsg_hex_digit(unsigned v, bool upper) {
    return (char)(v < 10 ? '0' + v : (upper ? 'A' : 'a') + (v - 10));
}

GMSG_API gmsg_text_writer gmsg_text_writer_init(uint8_t* out, size_t cap, char sep) {
    gmsg_text_writer w;
    w.out = (char*)out;
    w.cap = cap;
    w.pos = 0;
    w.sep = sep;
    w.first = true;
    return w;
}

GMSG_PRIV_ gmsg_error gmsg_text_put_(gmsg_text_writer* w, char c) {
    if (w->pos >= w->cap) return GMSG_ERR_BUFFER_FULL;
    w->out[w->pos++] = c;
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_text_begin_token(gmsg_text_writer* w) {
    if (!w->first && gmsg_text_put_(w, w->sep) != GMSG_OK) return GMSG_ERR_BUFFER_FULL;
    w->first = false;
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_text_raw(gmsg_text_writer* w, const char* s, size_t n) {
    size_t i;
    if (w->cap - w->pos < n) return GMSG_ERR_BUFFER_FULL;
    for (i = 0; i < n; ++i) w->out[w->pos++] = s[i];
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_text_empty_token(gmsg_text_writer* w) { return gmsg_text_begin_token(w); }

GMSG_API gmsg_error gmsg_text_token(gmsg_text_writer* w, const char* s, size_t n) {
    const gmsg_error e = gmsg_text_begin_token(w);
    return e != GMSG_OK ? e : gmsg_text_raw(w, s, n);
}

GMSG_API gmsg_error gmsg_text_char_token(gmsg_text_writer* w, char c) {
    const gmsg_error e = gmsg_text_begin_token(w);
    return e != GMSG_OK ? e : gmsg_text_put_(w, c);
}

/* Integer token: magnitude `mag` with sign `neg`; width > 0 zero-pads the digits; base > 10 digits upper case if
 * `upper`. */
GMSG_PRIV_ gmsg_error gmsg_text_int_(gmsg_text_writer* w, bool neg, uint64_t mag, int base, int width, bool upper) {
    char tmp[72];
    int n = 0, i;
    gmsg_error e = gmsg_text_begin_token(w);
    if (e != GMSG_OK) return e;
    do {
        tmp[n++] = gmsg_hex_digit((unsigned)(mag % (uint64_t)base), upper);
        mag /= (uint64_t)base;
    } while (mag);
    if (neg && (e = gmsg_text_put_(w, '-')) != GMSG_OK) return e;
    for (i = n; i < width; ++i)
        if ((e = gmsg_text_put_(w, '0')) != GMSG_OK) return e;
    for (i = n - 1; i >= 0; --i)
        if ((e = gmsg_text_put_(w, tmp[i])) != GMSG_OK) return e;
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_text_i64_token(gmsg_text_writer* w, int64_t v, int base, int width) {
    const bool neg = v < 0;
    const uint64_t mag = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    return gmsg_text_int_(w, neg, mag, base, width, true);
}

GMSG_API gmsg_error gmsg_text_u64_token(gmsg_text_writer* w, uint64_t v, int base, int width) {
    return gmsg_text_int_(w, false, v, base, width, true);
}
#if GMSG_FLOAT_TEXT != GMSG_FLOAT_TEXT_NONE
/* Float token: precision >= 0 fixed with that many decimals, else shortest; width > 0 zero-pads after the sign to
 * that many characters in total (NMEA style "0123.45"). */
GMSG_PRIV_ gmsg_error gmsg_text_float_(gmsg_text_writer* w, double v, bool is_float, int precision, int width) {
    char tmp[1100];
    size_t n = 0, i, start = 0;
    gmsg_error e = gmsg_text_begin_token(w);
    if (e != GMSG_OK) return e;
    e = is_float ? gmsg_format_float((float)v, precision, tmp, sizeof tmp, &n) : gmsg_format_double(v, precision, tmp, sizeof tmp, &n);
    if (e != GMSG_OK) return GMSG_ERR_INVALID_VALUE;
    if (n > 0 && tmp[0] == '-') {
        if ((e = gmsg_text_put_(w, '-')) != GMSG_OK) return e;
        start = 1;
    }
    for (i = n; i < (size_t)(width > 0 ? width : 0); ++i)
        if ((e = gmsg_text_put_(w, '0')) != GMSG_OK) return e;
    return gmsg_text_raw(w, tmp + start, n - start);
}

GMSG_API gmsg_error gmsg_text_f64_token(gmsg_text_writer* w, double v, int precision, int width) {
    return gmsg_text_float_(w, v, false, precision, width);
}

GMSG_API gmsg_error gmsg_text_f32_token(gmsg_text_writer* w, float v, int precision, int width) {
    return gmsg_text_float_(w, (double)v, true, precision, width);
}
#endif

#endif /* implementation */

GMSG_C_END

#endif /* GMSG_C_TEXT_H */
