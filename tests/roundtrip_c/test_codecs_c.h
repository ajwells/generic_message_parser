/* C versions of the user extensions in tests/roundtrip/test_codecs.hpp, referenced from the `c:` blocks of
 * tests/schemas/hdlc.yaml and custom.yaml. Same behaviour as the C++ versions (checked by the cross-check tests). */
#ifndef TEST_CODECS_C_H
#define TEST_CODECS_C_H

#include <math.h>

#include <gmp/c/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct test_time {
    uint8_t h, m, s;
} test_time;

static inline gmsg_error test_from_bcd(uint8_t b, uint8_t* out) {
    if ((b >> 4) > 9 || (b & 0xFu) > 9) return GMSG_ERR_INVALID_VALUE;
    *out = (uint8_t)((b >> 4) * 10u + (b & 0xFu));
    return GMSG_OK;
}

/* Three BCD bytes: HH MM SS. */
static inline gmsg_error test_bcd_time_decode(gmsg_reader* r, test_time* t) {
    uint8_t* dst[3];
    size_t i;
    dst[0] = &t->h;
    dst[1] = &t->m;
    dst[2] = &t->s;
    for (i = 0; i < 3; ++i) {
        uint8_t b = 0;
        gmsg_error e = gmsg_read_u8(r, GMSG_ENDIAN_BIG, &b);
        if (e != GMSG_OK) return e;
        e = test_from_bcd(b, dst[i]);
        if (e != GMSG_OK) return e;
    }
    return GMSG_OK;
}

static inline gmsg_error test_bcd_time_encode(gmsg_writer* w, const test_time* t) {
    const uint8_t v[3] = {t->h, t->m, t->s};
    size_t i;
    for (i = 0; i < 3; ++i) {
        gmsg_error e;
        if (v[i] > 99) return GMSG_ERR_OUT_OF_RANGE;
        e = gmsg_write_u8(w, GMSG_ENDIAN_BIG, (uint8_t)(((v[i] / 10u) << 4) | (v[i] % 10u)));
        if (e != GMSG_OK) return e;
    }
    return GMSG_OK;
}

/* Print hook (c: { print: test_time_print }): HH:MM:SS, as test::gmp_print. */
static inline void test_time_print(gmsg_printer* p, const test_time* t) {
    const char b[8] = {(char)('0' + t->h / 10 % 10), (char)('0' + t->h % 10), ':',
                       (char)('0' + t->m / 10 % 10), (char)('0' + t->m % 10), ':',
                       (char)('0' + t->s / 10 % 10), (char)('0' + t->s % 10)};
    gmsg_print_text(p, b, sizeof b);
}

/* Custom checksum: 16-bit sum of all bytes. */
static inline uint16_t test_add16(const uint8_t* d, size_t n) {
    uint16_t s = 0;
    size_t i;
    for (i = 0; i < n; ++i) s = (uint16_t)(s + d[i]);
    return s;
}

/* Custom framer: frame = [n][n bytes]; a zero length byte is treated as garbage. */
static inline gmsg_frame_scan test_len_byte_scan(const uint8_t* b, size_t n) {
    size_t total;
    if (n == 0) return gmsg_scan_need_more(1);
    if (b[0] == 0) return gmsg_scan_skip(1, GMSG_ERR_BAD_LENGTH);
    total = 1u + b[0];
    if (n < total) return gmsg_scan_need_more(total);
    return gmsg_scan_frame(total, total, total);
}

/* Custom text type: NMEA "ddmm.mmmm" <-> decimal degrees (as test::decode_coord / test::encode_coord). */
static inline gmsg_error test_coord_decode(const char* s, size_t n, double* out) {
    double v = 0, deg;
    const gmsg_error e = gmsg_parse_double(s, n, &v);
    if (e != GMSG_OK) return e;
    deg = floor(v / 100.0);
    *out = deg + (v - deg * 100.0) / 60.0;
    return GMSG_OK;
}

static inline gmsg_error test_coord_encode(gmsg_text_writer* w, const double* degrees) {
    const double deg = floor(*degrees);
    const double ddmm = deg * 100.0 + (*degrees - deg) * 60.0;
    char buf[64];
    size_t n = 0;
    if (gmsg_format_double(ddmm, 4, buf, sizeof buf, &n) != GMSG_OK) return GMSG_ERR_INVALID_VALUE;
    return gmsg_text_raw(w, buf, n);
}

#ifdef __cplusplus
}
#endif

#endif /* TEST_CODECS_C_H */
