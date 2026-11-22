/* User-supplied C codecs referenced from the `c:` blocks of meter.yaml (the C twin of meter_codecs.hpp). */
#ifndef METER_CODECS_H
#define METER_CODECS_H

#include <stdio.h>

#include <gmp/c/runtime.h>

typedef struct meter_datetime {
    int year, month, day, hour, minute, second;
} meter_datetime;

static inline int meter_bcd(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static inline uint8_t meter_to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* YY MM DD hh mm ss, BCD encoded. */
static inline gmsg_error meter_decode_bcd(gmsg_reader* r, meter_datetime* t) {
    uint8_t d[6];
    size_t i;
    for (i = 0; i < 6; ++i) {
        const gmsg_error e = gmsg_read_u8(r, GMSG_ENDIAN_BIG, &d[i]);
        if (e != GMSG_OK) return e;
    }
    t->year = 2000 + meter_bcd(d[0]);
    t->month = meter_bcd(d[1]);
    t->day = meter_bcd(d[2]);
    t->hour = meter_bcd(d[3]);
    t->minute = meter_bcd(d[4]);
    t->second = meter_bcd(d[5]);
    if (t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31) return GMSG_ERR_INVALID_VALUE;
    return GMSG_OK;
}

static inline gmsg_error meter_encode_bcd(gmsg_writer* w, const meter_datetime* t) {
    const int v[6] = {t->year - 2000, t->month, t->day, t->hour, t->minute, t->second};
    size_t i;
    for (i = 0; i < 6; ++i) {
        const gmsg_error e = gmsg_write_u8(w, GMSG_ENDIAN_BIG, meter_to_bcd(v[i]));
        if (e != GMSG_OK) return e;
    }
    return GMSG_OK;
}

/* Optional print hook (c: { print: ... }): 2026-11-20 14:30:05. Without it the field prints as <?>. */
static inline void meter_print_datetime(gmsg_printer* p, const meter_datetime* t) {
    char b[20];
    const int n = snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", t->year, t->month, t->day, t->hour, t->minute,
                           t->second);
    if (n > 0) gmsg_print_text(p, b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}

/* Custom checksum: modulo-256 sum. */
static inline uint8_t meter_mod256(const uint8_t* d, size_t n) {
    unsigned s = 0;
    size_t i;
    for (i = 0; i < n; ++i) s += d[i];
    return (uint8_t)s;
}

#endif
