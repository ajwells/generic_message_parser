/* Unit tests of the GMP C runtime (valid C99 and C11). Built as C11 and C99, with GMSG_USE_LIBC=1 and 0, with
 * GMSG_CRC_TABLES=1, and against the library build (GMSG_RUNTIME_LIBRARY=1). Exit code = number of failed checks. */
#include "gmp/c/runtime.h"

#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                          \
    } while (0)

static const uint8_t k_check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
#define K_CHECK_N sizeof k_check

static void test_config(void) {
#if GMSG_USE_LIBC
    CHECK(GMSG_USE_LIBC == 1);
#else
    CHECK(GMSG_USE_LIBC == 0);
#endif
    CHECK(GMSG_FLOAT_TEXT == GMSG_FLOAT_TEXT_BUILTIN || GMSG_FLOAT_TEXT == GMSG_FLOAT_TEXT_LIBC);
}

static void test_error(void) {
    CHECK(GMSG_OK == 0);
    CHECK(GMSG_ERR_TIMEOUT == GMSG_ERROR_COUNT - 1);
    CHECK(gmsg_memcmp(gmsg_error_str(GMSG_ERR_BAD_CHECKSUM), "bad checksum", 13) == 0);
    CHECK(gmsg_memcmp(gmsg_error_str((gmsg_error)99), "unknown", 8) == 0);
}

static void test_mem(void) {
    uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8}, b[8] = {0};
    gmsg_memcpy(b, a, 8);
    CHECK(gmsg_memcmp(a, b, 8) == 0);
    b[3] = 0xFF;
    CHECK(gmsg_memcmp(a, b, 8) < 0);
    CHECK(gmsg_memcmp(b, a, 8) > 0);
    CHECK(gmsg_memcmp(a, b, 0) == 0);
    gmsg_memset(b, 0xAA, 4);
    CHECK(b[0] == 0xAA && b[3] == 0xAA && b[4] == 5);
    gmsg_memmove(a + 1, a, 6); /* overlapping, forward */
    CHECK(a[0] == 1 && a[1] == 1 && a[6] == 6 && a[7] == 8);
    gmsg_memmove(a, a + 1, 6); /* overlapping, backward */
    CHECK(a[0] == 1 && a[1] == 2 && a[5] == 6);
    gmsg_memcpy(b, a, 0); /* n = 0 with valid pointers is a no-op */
}

static void test_endian(void) {
    uint8_t buf[8];
    CHECK(gmsg_load_u16_be((const uint8_t[]){0x12, 0x34}) == 0x1234);
    CHECK(gmsg_load_u16_le((const uint8_t[]){0x12, 0x34}) == 0x3412);
    CHECK(gmsg_load_uint((const uint8_t[]){0x01, 0x02, 0x03}, 3, GMSG_ENDIAN_BIG) == 0x010203u);
    CHECK(gmsg_load_uint((const uint8_t[]){0x01, 0x02, 0x03}, 3, GMSG_ENDIAN_LITTLE) == 0x030201u);

    gmsg_store_u32_be(buf, 0xDEADBEEFu);
    CHECK(buf[0] == 0xDE && buf[3] == 0xEF);
    CHECK(gmsg_load_u32_be(buf) == 0xDEADBEEFu);
    gmsg_store_u64_le(buf, 0x0102030405060708u);
    CHECK(buf[0] == 0x08 && buf[7] == 0x01);
    CHECK(gmsg_load_u64(buf, GMSG_ENDIAN_LITTLE) == 0x0102030405060708u);

    gmsg_store_i16_be(buf, -2);
    CHECK(buf[0] == 0xFF && buf[1] == 0xFE);
    CHECK(gmsg_load_i16_be(buf) == -2);
    gmsg_store_i8(buf, INT8_MIN, GMSG_ENDIAN_BIG);
    CHECK(buf[0] == 0x80 && gmsg_load_i8(buf, GMSG_ENDIAN_BIG) == INT8_MIN);
    gmsg_store_i32_le(buf, INT32_MIN);
    CHECK(gmsg_load_i32_le(buf) == INT32_MIN);
    gmsg_store_i64_be(buf, INT64_MIN);
    CHECK(buf[0] == 0x80 && buf[7] == 0x00 && gmsg_load_i64_be(buf) == INT64_MIN);
    gmsg_store_i64_be(buf, -1);
    CHECK(gmsg_load_i64_be(buf) == -1 && gmsg_load_u64_be(buf) == UINT64_MAX);

    gmsg_store_f32_be(buf, 1.5f);
    CHECK(buf[0] == 0x3F && buf[1] == 0xC0 && buf[2] == 0 && buf[3] == 0);
    CHECK(gmsg_load_f32_be(buf) == 1.5f);
    gmsg_store_f64_le(buf, -0.25);
    CHECK(buf[7] == 0xBF && buf[6] == 0xD0);
    CHECK(gmsg_load_f64_le(buf) == -0.25);

    gmsg_store_bool(buf, true);
    CHECK(buf[0] == 1 && gmsg_load_bool(buf));
    buf[0] = 7;
    CHECK(gmsg_load_bool(buf));
}

static void test_bits(void) {
    uint8_t b[2] = {0xA5, 0x3C}; /* 1010 0101  0011 1100 */
    CHECK(gmsg_get_bits(b, 0, 4, GMSG_MSB_FIRST) == 0xA);
    CHECK(gmsg_get_bits(b, 4, 8, GMSG_MSB_FIRST) == 0x53);
    CHECK(gmsg_get_bits(b, 0, 4, GMSG_LSB_FIRST) == 0x5);
    CHECK(gmsg_get_bits(b, 4, 8, GMSG_LSB_FIRST) == 0xCA);
    CHECK(gmsg_get_bits(b, 0, 16, GMSG_MSB_FIRST) == 0xA53C);
    CHECK(gmsg_get_bits_signed(b, 0, 4, GMSG_MSB_FIRST) == -6);
    CHECK(gmsg_sign_extend(0x7, 3) == -1);
    CHECK(gmsg_sign_extend(0x3, 3) == 3);
    CHECK(gmsg_sign_extend(UINT64_MAX, 64) == -1);

    {
        uint8_t z[2] = {0, 0};
        gmsg_set_bits(z, 1, 3, 0x5, GMSG_MSB_FIRST); /* 0101 0000 */
        CHECK(z[0] == 0x50 && z[1] == 0);
        gmsg_set_bits(z, 1, 3, 0x0, GMSG_MSB_FIRST);
        CHECK(z[0] == 0);
        gmsg_set_bits(z, 6, 4, 0xF, GMSG_LSB_FIRST); /* bits 6..9 */
        CHECK(z[0] == 0xC0 && z[1] == 0x03);
        gmsg_set_bits_signed(z, 0, 4, -1, GMSG_MSB_FIRST);
        CHECK(z[0] == 0xF0 && z[1] == 0x03);
        CHECK(gmsg_get_bits_signed(z, 0, 4, GMSG_MSB_FIRST) == -1);
    }
    {
        uint8_t w[8];
        gmsg_memset(w, 0, 8);
        gmsg_set_bits(w, 0, 64, 0x0123456789ABCDEFu, GMSG_MSB_FIRST);
        CHECK(gmsg_load_u64_be(w) == 0x0123456789ABCDEFu);
        CHECK(gmsg_get_bits(w, 0, 64, GMSG_MSB_FIRST) == 0x0123456789ABCDEFu);
        gmsg_set_bits(w, 0, 64, 0x0123456789ABCDEFu, GMSG_LSB_FIRST);
        CHECK(gmsg_load_u64_le(w) == 0x0123456789ABCDEFu);
    }
}

static void test_reader_writer(void) {
    uint8_t out[6];
    gmsg_writer w = gmsg_writer_init(out, sizeof out);
    size_t at = 0;
    CHECK(gmsg_write_u8(&w, GMSG_ENDIAN_BIG, 0xAA) == GMSG_OK);
    CHECK(gmsg_write_reserve(&w, 2, &at) == GMSG_OK && at == 1);
    CHECK(gmsg_write_i16(&w, GMSG_ENDIAN_LITTLE, -3) == GMSG_OK);
    CHECK(gmsg_write_bool(&w, true) == GMSG_OK);
    CHECK(gmsg_writer_remaining(&w) == 0);
    CHECK(gmsg_write_u8(&w, GMSG_ENDIAN_BIG, 1) == GMSG_ERR_BUFFER_FULL);
    CHECK(gmsg_write_fill(&w, 0, 1) == GMSG_ERR_BUFFER_FULL);
    CHECK(w.pos == 6);
    gmsg_write_patch_uint(&w, at, 2, GMSG_ENDIAN_BIG, 0x0102);
    CHECK(out[0] == 0xAA && out[1] == 0x01 && out[2] == 0x02 && out[3] == 0xFD && out[4] == 0xFF && out[5] == 1);

    {
        gmsg_reader r = gmsg_reader_init(out, sizeof out);
        uint8_t u8 = 0;
        uint16_t u16 = 0;
        int16_t i16 = 0;
        bool flag = false;
        uint64_t keep = 77;
        CHECK(gmsg_read_u8(&r, GMSG_ENDIAN_BIG, &u8) == GMSG_OK && u8 == 0xAA);
        CHECK(gmsg_read_u16(&r, GMSG_ENDIAN_BIG, &u16) == GMSG_OK && u16 == 0x0102);
        CHECK(gmsg_read_i16(&r, GMSG_ENDIAN_LITTLE, &i16) == GMSG_OK && i16 == -3);
        CHECK(gmsg_read_bool(&r, &flag) == GMSG_OK && flag);
        CHECK(gmsg_read_uint(&r, 1, GMSG_ENDIAN_BIG, &keep) == GMSG_ERR_TRUNCATED && keep == 77);
        CHECK(r.pos == 6);
    }
    {
        const uint8_t text[] = {'a', 'b', 0, 'c'};
        gmsg_reader r = gmsg_reader_init(text, sizeof text);
        const uint8_t* p = NULL;
        size_t n = 0;
        uint8_t c = 0;
        CHECK(gmsg_read_until(&r, 0, &p, &n) == GMSG_OK && n == 2 && p == text && r.pos == 3);
        CHECK(gmsg_read_until(&r, 0, &p, &n) == GMSG_ERR_TRUNCATED && r.pos == 3);
        CHECK(gmsg_read_copy(&r, &c, 1) == GMSG_OK && c == 'c');
        CHECK(gmsg_read_skip(&r, 1) == GMSG_ERR_TRUNCATED);
        r.pos = 0;
        CHECK(gmsg_read_bytes(&r, 4, &p) == GMSG_OK && p == text && gmsg_reader_remaining(&r) == 0);
    }
}

static void test_checksums(void) {
    /* Check values of the reveng.sourceforge.io CRC catalogue (input "123456789"). */
    CHECK(gmsg_crc8_compute(k_check, K_CHECK_N) == 0xF4);
    CHECK(gmsg_crc8_maxim_compute(k_check, K_CHECK_N) == 0xA1);
    CHECK(gmsg_crc8_autosar_compute(k_check, K_CHECK_N) == 0xDF);
    CHECK(gmsg_crc8_nrsc5_compute(k_check, K_CHECK_N) == 0xF7);
    CHECK(gmsg_crc15_can_compute(k_check, K_CHECK_N) == 0x059E);
    CHECK(gmsg_crc16_ccitt_compute(k_check, K_CHECK_N) == 0x29B1);
    CHECK(gmsg_crc16_xmodem_compute(k_check, K_CHECK_N) == 0x31C3);
    CHECK(gmsg_crc16_kermit_compute(k_check, K_CHECK_N) == 0x2189);
    CHECK(gmsg_crc16_x25_compute(k_check, K_CHECK_N) == 0x906E);
    CHECK(gmsg_crc16_modbus_compute(k_check, K_CHECK_N) == 0x4B37);
    CHECK(gmsg_crc16_arc_compute(k_check, K_CHECK_N) == 0xBB3D);
    CHECK(gmsg_crc24_openpgp_compute(k_check, K_CHECK_N) == 0x21CF02u);
    CHECK(gmsg_crc32_compute(k_check, K_CHECK_N) == 0xCBF43926u);
    CHECK(gmsg_crc32c_compute(k_check, K_CHECK_N) == 0xE3069283u);
    CHECK(gmsg_crc32_mpeg2_compute(k_check, K_CHECK_N) == 0x0376E6E7u);
    CHECK(gmsg_crc64_ecma_compute(k_check, K_CHECK_N) == 0x6C40DF5F0B497347u);

    /* Sensirion datasheet example: CRC of 0xBEEF is 0x92. */
    CHECK(gmsg_crc8_nrsc5_compute((const uint8_t[]){0xBE, 0xEF}, 2) == 0x92);

    /* Small widths and inline parameters: CRC-3/GSM (check 0x4), CRC-5/USB (reflected, check 0x19). */
    {
        const gmsg_crc_params gsm = {3, false, false, 0x3, 0x0, 0x7};
        const gmsg_crc_params usb = {5, true, true, 0x05, 0x1F, 0x1F};
        CHECK(gmsg_crc_compute(&gsm, k_check, K_CHECK_N) == 0x4);
        CHECK(gmsg_crc_compute(&usb, k_check, K_CHECK_N) == 0x19);
    }

    /* Incremental API gives the same result as one call. */
    {
        uint16_t s = gmsg_crc16_modbus_init();
        s = gmsg_crc16_modbus_update(s, k_check, 4);
        s = gmsg_crc16_modbus_update(s, k_check + 4, K_CHECK_N - 4);
        CHECK(gmsg_crc16_modbus_finish(s) == 0x4B37);
    }

    CHECK(gmsg_xor8_compute(k_check, K_CHECK_N) == 0x31);
    CHECK(gmsg_sum8_compute(k_check, K_CHECK_N) == 0xDD);
    CHECK(gmsg_lrc8_compute(k_check, K_CHECK_N) == 0x23);
    CHECK(gmsg_sum16_compute(k_check, K_CHECK_N) == 0x01DD);
    CHECK(gmsg_fletcher16_compute((const uint8_t*)"abcde", 5) == 0xC8F0);
    CHECK(gmsg_fletcher16_compute(k_check, 0) == 0);
}

/* Every preset gives the same register as the generic bit-by-bit CRC with the preset's parameters: for every
 * byte value from several start registers (this reads every entry of a lookup table when GMSG_CRC_TABLES is 1),
 * and for random data split at random points. */
static uint32_t g_rand = 12345u;
static uint32_t next_rand(void) {
    g_rand = g_rand * 1664525u + 1013904223u;
    return g_rand >> 8;
}

#define CHECK_PRESET(name, T)                                                                            \
    do {                                                                                                  \
        const gmsg_crc_params p = gmsg_##name##_params();                                                 \
        const uint64_t mask = p.width >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << p.width) - 1);               \
        const uint64_t starts[5] = {0, mask, gmsg_crc_init(&p), 0x5A5A5A5A5A5A5A5Au & mask,                 \
                                    0x123456789ABCDEF1u & mask};                                          \
        int bad = 0, k, b, round;                                                                         \
        for (k = 0; k < 5; ++k)                                                                           \
            for (b = 0; b < 256; ++b) {                                                                   \
                const uint8_t byte = (uint8_t)b;                                                          \
                if ((uint64_t)gmsg_##name##_update((T)starts[k], &byte, 1) != gmsg_crc_update(&p, starts[k], &byte, 1)) \
                    ++bad;                                                                                \
            }                                                                                             \
        for (round = 0; round < 50; ++round) {                                                            \
            uint8_t data[300];                                                                            \
            const size_t n = next_rand() % sizeof data, cut = n ? next_rand() % n : 0;                    \
            size_t i;                                                                                     \
            T st;                                                                                         \
            for (i = 0; i < n; ++i) data[i] = (uint8_t)next_rand();                                       \
            st = gmsg_##name##_update(gmsg_##name##_init(), data, cut);                                   \
            st = gmsg_##name##_update(st, data + cut, n - cut);                                           \
            if ((uint64_t)gmsg_##name##_finish(st) != gmsg_crc_compute(&p, data, n)) ++bad;               \
            if ((uint64_t)gmsg_##name##_compute(data, n) != gmsg_crc_compute(&p, data, n)) ++bad;          \
        }                                                                                                 \
        if (bad) printf("preset %s: %d mismatches\n", #name, bad);                                       \
        CHECK(bad == 0);                                                                                  \
    } while (0)

static void test_crc_presets_vs_bitwise(void) {
    CHECK_PRESET(crc8, uint8_t);
    CHECK_PRESET(crc8_maxim, uint8_t);
    CHECK_PRESET(crc8_autosar, uint8_t);
    CHECK_PRESET(crc8_nrsc5, uint8_t);
    CHECK_PRESET(crc15_can, uint16_t);
    CHECK_PRESET(crc16_ccitt, uint16_t);
    CHECK_PRESET(crc16_xmodem, uint16_t);
    CHECK_PRESET(crc16_kermit, uint16_t);
    CHECK_PRESET(crc16_x25, uint16_t);
    CHECK_PRESET(crc16_modbus, uint16_t);
    CHECK_PRESET(crc16_arc, uint16_t);
    CHECK_PRESET(crc24_openpgp, uint32_t);
    CHECK_PRESET(crc32, uint32_t);
    CHECK_PRESET(crc32c, uint32_t);
    CHECK_PRESET(crc32_mpeg2, uint32_t);
    CHECK_PRESET(crc64_ecma, uint64_t);
}

static void test_float_text(void) {
#if GMSG_FLOAT_TEXT != GMSG_FLOAT_TEXT_NONE
    char buf[64];
    size_t n = 0;
    double d = 0;
    float f = 0;
    CHECK(gmsg_format_double(4807.0381, 4, buf, sizeof buf, &n) == GMSG_OK && gmsg_memcmp(buf, "4807.0381", 9) == 0 && n == 9);
    CHECK(gmsg_format_double(0.5, 0, buf, sizeof buf, &n) == GMSG_OK && n == 1 && buf[0] == '0'); /* ties to even */
    CHECK(gmsg_format_double(1.5, 0, buf, sizeof buf, &n) == GMSG_OK && n == 1 && buf[0] == '2');
    CHECK(gmsg_format_double(1.0, 3, buf, 3, &n) == GMSG_ERR_BUFFER_FULL);
    CHECK(gmsg_parse_double("-12.25", 6, &d) == GMSG_OK && d == -12.25);
    CHECK(gmsg_parse_float("+0.5", 4, &f) == GMSG_OK && f == 0.5f);
    CHECK(gmsg_parse_double("", 0, &d) == GMSG_ERR_MISSING_FIELD);
    CHECK(gmsg_parse_double("1x", 2, &d) == GMSG_ERR_PARSE);
    /* shortest text reads back to the same value */
    CHECK(gmsg_format_double(0.1, -1, buf, sizeof buf, &n) == GMSG_OK && gmsg_parse_double(buf, n, &d) == GMSG_OK && d == 0.1);
    CHECK(gmsg_format_float(0.9f, -1, buf, sizeof buf, &n) == GMSG_OK && gmsg_parse_float(buf, n, &f) == GMSG_OK && f == 0.9f);
#if GMSG_FLOAT_TEXT == GMSG_FLOAT_TEXT_BUILTIN
    CHECK(gmsg_format_double(0.1, -1, buf, sizeof buf, &n) == GMSG_OK && n == 3 && gmsg_memcmp(buf, "0.1", 3) == 0);
    CHECK(gmsg_format_double(1e21, -1, buf, sizeof buf, &n) == GMSG_OK && n == 5 && gmsg_memcmp(buf, "1e+21", 5) == 0);
    CHECK(gmsg_parse_double("1e400", 5, &d) == GMSG_ERR_OUT_OF_RANGE);
#endif
#endif
}

/* gmsg_ring: init checks, push / wrap / readable parts, full, drop, overrun, high water, index wrap. */
static void test_ring(void) {
    uint8_t store[256];
    uint8_t out[256];
    gmsg_ring r;
    size_t n, i;
    const uint8_t more[7] = {10, 11, 12, 13, 14, 15, 16};
    const uint8_t big[10] = {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33};
    const uint8_t* part;
#if GMSG_C11 && !defined(__STDC_NO_ATOMICS__) && !defined(GMP_TEST_RING_FENCE)
    CHECK(GMSG_RING_ATOMICS == 1);
#else
    CHECK(GMSG_RING_ATOMICS == 0);
#endif
    CHECK(gmsg_ring_init(&r, store, 6) == GMSG_ERR_OUT_OF_RANGE);
    CHECK(gmsg_ring_init(&r, store, 1) == GMSG_ERR_OUT_OF_RANGE);
    CHECK(gmsg_ring_init(&r, NULL, 8) == GMSG_ERR_OUT_OF_RANGE);
#if GMSG_RING_INDEX_BITS == 8
    CHECK(gmsg_ring_init(&r, store, 256) == GMSG_ERR_OUT_OF_RANGE);
    CHECK(gmsg_ring_init(&r, store, 128) == GMSG_OK);
#else
    CHECK(gmsg_ring_init(&r, store, 256) == GMSG_OK);
#endif
    CHECK(gmsg_ring_init(&r, store, 8) == GMSG_OK);
    CHECK(gmsg_ring_capacity(&r) == 8);
    CHECK(gmsg_ring_empty(&r));
    (void)gmsg_ring_readable(&r, &n);
    CHECK(n == 0);
    for (i = 0; i < 6; ++i) CHECK(gmsg_ring_push(&r, (uint8_t)i));
    CHECK(gmsg_ring_size(&r) == 6);
    gmsg_ring_consume(&r, 5);
    CHECK(gmsg_ring_push_n(&r, more, 7) == 7);
    CHECK(gmsg_ring_size(&r) == 8);
    CHECK(gmsg_ring_high_water(&r) == 8);
    part = gmsg_ring_readable(&r, &n);
    CHECK(n == 3 && part[0] == 5);
    CHECK(gmsg_ring_read(&r, out, sizeof out) == 8);
    CHECK(out[0] == 5 && out[1] == 10 && out[7] == 16);
    CHECK(gmsg_ring_empty(&r));
    CHECK(!gmsg_ring_overrun_pending(&r));

    for (i = 0; i < 8; ++i) CHECK(gmsg_ring_push(&r, (uint8_t)i));
    CHECK(!gmsg_ring_push(&r, 99));
    CHECK(gmsg_ring_overrun_pending(&r));
    CHECK(gmsg_ring_dropped(&r) == 1);
    gmsg_ring_consume(&r, 4);
    CHECK(!gmsg_ring_push(&r, 98));
    CHECK(gmsg_ring_push_n(&r, more, 7) == 0);
    CHECK(gmsg_ring_dropped(&r) == 9);
    CHECK(gmsg_ring_size(&r) == 4);
    gmsg_ring_consume(&r, 4);
    gmsg_ring_clear_overrun(&r);
    CHECK(gmsg_ring_push(&r, 7));
    CHECK(gmsg_ring_push_n(&r, big, 10) == 7);
    CHECK(gmsg_ring_overrun_pending(&r));
    CHECK(gmsg_ring_dropped(&r) == 12);

    /* many wraps of the free-running indices (8-bit indices wrap every 256 bytes) */
    {
        uint8_t next_in = 0, next_out = 0;
        uint32_t seed = 7;
        int round, ok = 1;
        CHECK(gmsg_ring_init(&r, store, 128) == GMSG_OK);
        for (round = 0; round < 3000; ++round) {
            size_t burst, take, got;
            seed = seed * 1103515245u + 12345u;
            burst = (seed >> 16) % 100;
            for (i = 0; i < burst && gmsg_ring_size(&r) < 128; ++i) ok &= gmsg_ring_push(&r, next_in++);
            seed = seed * 1103515245u + 12345u;
            take = (seed >> 16) % 120;
            got = gmsg_ring_read(&r, out, take);
            for (i = 0; i < got; ++i) ok &= out[i] == next_out++;
        }
        CHECK(ok);
        CHECK(!gmsg_ring_overrun_pending(&r));
        CHECK(gmsg_ring_high_water(&r) <= 128);
    }
}

/* A stream over a hand-written protocol: 0x24 <len> <payload...>; payload byte 0 odd = bad checksum. Events are
 * logged as "P<n> " (frame with payload byte 0 = 2n) and "E<code>:<size> ". */
static char g_log[256];
static size_t g_log_n;
static void log_char(char c) {
    if (g_log_n + 1 < sizeof g_log) g_log[g_log_n++] = c;
    g_log[g_log_n] = 0;
}
static void log_num(size_t v) {
    char d[20];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v != 0);
    while (k > 0) log_char(d[--k]);
}
static gmsg_frame_scan t_scan(const uint8_t* b, size_t n) {
    size_t k = 0;
    if (n == 0) return gmsg_scan_need_more(0);
    if (b[0] != 0x24) {
        while (k < n && b[k] != 0x24) ++k;
        return gmsg_scan_skip(k, GMSG_ERR_BAD_SYNC);
    }
    if (n < 2 || n < (size_t)b[1] + 2) return gmsg_scan_need_more(0);
    return gmsg_scan_frame((size_t)b[1] + 2, (size_t)b[1] + 2, 1);
}
static gmsg_error t_process(gmsg_stream* s, uint8_t* f, size_t n) {
    (void)s;
    if (n < 3 || (f[2] & 1)) return GMSG_ERR_BAD_CHECKSUM;
    log_char('P');
    log_num(f[2] / 2u);
    log_char(' ');
    return GMSG_OK;
}
static void t_error(gmsg_stream* s, gmsg_error e, const uint8_t* d, size_t n) {
    (void)s;
    (void)d;
    log_char('E');
    log_num((size_t)e);
    log_char(':');
    log_num(n);
    log_char(' ');
}
static int log_is(const char* want) {
    size_t i = 0;
    while (want[i] && g_log[i] == want[i]) ++i;
    return want[i] == 0 && g_log[i] == 0;
}

/* gmsg_stream with a gmsg_ring: overruns, byte limits, flush, and bytes pushed after a taken-over gap (the scan stops
 * at the gap; compaction moves it). */
static void test_stream_ring(void) {
    static uint8_t ring_buf[16], sbuf[40];
    static const uint8_t abc[] = {0x24, 2, 2, 0, 0x24, 2, 4, 0, 0x24, 8, 6, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t d[] = {0x24, 2, 8, 0};
    gmsg_ring ring;
    gmsg_stream s;
    size_t cap;
    CHECK(gmsg_ring_init(&ring, ring_buf, sizeof ring_buf) == GMSG_OK);
    for (cap = 16; cap <= 40; cap += 24) {
        CHECK(gmsg_stream_init(&s, sbuf, cap, cap, t_scan, t_process, t_error) == GMSG_OK);
        g_log_n = 0;
        g_log[0] = 0;
        CHECK(gmsg_ring_push_n(&ring, abc, sizeof abc) == 16); /* the last 2 bytes are dropped */
        CHECK(gmsg_stream_push_ring(&s, &ring, SIZE_MAX) == 16);
        CHECK(gmsg_stream_next(&s)); /* P1 */
        CHECK(gmsg_stream_push(&s, d, sizeof d) == 4); /* capacity 16: compacts */
        while (gmsg_stream_next(&s)) {
        }
        CHECK(log_is("P1 P2 E16:8 P4 "));
        CHECK(s.stats.overruns == 1);
    }
    /* feed_ring with a byte limit, a gap between frames, and flush with a gap waiting */
    g_log_n = 0;
    g_log[0] = 0;
    CHECK(gmsg_ring_push_n(&ring, abc, 8) == 8);
    CHECK(gmsg_ring_push_n(&ring, abc, 8) == 8);
    CHECK(!gmsg_ring_push(&ring, 0x24));
    CHECK(gmsg_stream_feed_ring(&s, &ring, 5) == 1);
    CHECK(gmsg_stream_feed_ring(&s, &ring, SIZE_MAX) == 3);
    CHECK(log_is("P1 P2 P1 P2 E16:0 "));
    g_log_n = 0;
    g_log[0] = 0;
    CHECK(gmsg_ring_push_n(&ring, abc, sizeof abc) == 16);
    CHECK(gmsg_stream_push_ring(&s, &ring, SIZE_MAX) == 16);
    CHECK(gmsg_stream_push(&s, d, 2) == 2);
    CHECK(!gmsg_stream_resync(&s));
    CHECK(gmsg_stream_flush(&s));
    CHECK(log_is("E16:16 E1:2 "));
    CHECK(gmsg_stream_buffered(&s) == 0);
}

/* ---- endpoint pieces: adapters, push_gap / push_from, write policies, the wait loop, the DMA source ---- */

static uint32_t g_now = 0;
static int g_idles = 0;
static uint32_t t_clock(void* ctx) {
    (void)ctx;
    return g_now;
}
static void t_idle(void* ctx) {
    (void)ctx;
    ++g_now;
    ++g_idles;
}
static size_t g_take = 1; /* bytes a slow sink takes per call */
static uint8_t g_out[64];
static size_t g_out_n = 0;
static size_t t_slow_write(void* ctx, const uint8_t* d, size_t n) {
    size_t k = n < g_take ? n : g_take;
    (void)ctx;
    if (g_out_n + k > sizeof g_out) k = sizeof g_out - g_out_n;
    gmsg_memcpy(g_out + g_out_n, d, k);
    g_out_n += k;
    return k;
}
static int g_pulls = 0;
static bool t_pull_third(void* ctx) {
    (void)ctx;
    return ++g_pulls >= 3;
}
static bool t_pull_never(void* ctx) {
    (void)ctx;
    ++g_pulls;
    return false;
}

/* simulated circular DMA of 16 bytes */
static volatile uint8_t g_dma[16];
static size_t g_dma_pos = 0;
static gmsg_dma_source* g_dma_src = NULL;
static size_t t_dma_pos(void* ctx) {
    (void)ctx;
    return g_dma_pos;
}
static void dma_write(const uint8_t* d, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        g_dma[g_dma_pos] = d[i];
        g_dma_pos = (g_dma_pos + 1) % sizeof g_dma;
        if (g_dma_src && (g_dma_pos == 0 || g_dma_pos == sizeof g_dma / 2)) gmsg_dma_source_on_half(g_dma_src);
    }
}

static void test_endpoint(void) {
    static uint8_t sbuf[32];
    static const uint8_t frames[] = {0x24, 2, 2, 0, 0x55, 0x24, 2, 4, 0, 0x24, 2, 8, 0};
    static const uint8_t f1[] = {0x24, 2, 2, 0};
    gmsg_stream s;
    gmsg_endpoint e;
    gmsg_buffer_source bs;
    gmsg_buffer_sink bk;
    gmsg_source src;
    gmsg_sink snk;
    uint8_t out[8];
    uint8_t ring_buf[8];
    gmsg_ring ring;
    gmsg_dma_source dma;
    size_t i;

    /* service(): a chunked memory source, every frame delivered */
    CHECK(gmsg_stream_init(&s, sbuf, sizeof sbuf, 8, t_scan, t_process, t_error) == GMSG_OK);
    src = gmsg_buffer_source_init(&bs, frames, sizeof frames, 3);
    gmsg_endpoint_init(&e, &s, &src, NULL);
    g_log_n = 0;
    g_log[0] = 0;
    CHECK(gmsg_endpoint_service(&e, SIZE_MAX) == 3);
    CHECK(log_is("P1 E2:1 P2 P4 "));
    CHECK(bs.pos == sizeof frames);
    /* service(max) reads at most max bytes */
    gmsg_stream_reset(&s);
    src = gmsg_buffer_source_init(&bs, frames, sizeof frames, 0);
    gmsg_endpoint_init(&e, &s, &src, NULL);
    CHECK(gmsg_endpoint_service(&e, 4) == 1);
    CHECK(bs.pos == 4);

    /* push_gap: the frame in progress is dropped as an overrun; one gap at a time */
    gmsg_stream_reset(&s);
    g_log_n = 0;
    g_log[0] = 0;
    CHECK(gmsg_stream_space(&s) == sizeof sbuf);
    CHECK(gmsg_stream_push(&s, frames, 7) == 7); /* P1, garbage, half of P2 */
    CHECK(gmsg_stream_push_gap(&s));
    CHECK(gmsg_stream_push_gap(&s));
    CHECK(gmsg_stream_push(&s, f1, sizeof f1) == 4);
    CHECK(!gmsg_stream_push_gap(&s));
    while (gmsg_stream_next(&s)) {
    }
    CHECK(log_is("P1 E2:1 E16:2 P1 "));
    CHECK(s.stats.overruns == 1);

    /* write: whole frame into a buffer sink, refused whole when it does not fit; a NULL sink refuses everything */
    gmsg_stream_reset(&s);
    snk = gmsg_buffer_sink_init(&bk, out, 6);
    gmsg_endpoint_init(&e, &s, NULL, &snk);
    CHECK(gmsg_endpoint_write(&e, f1, 4) == GMSG_OK);
    CHECK(bk.used == 4);
    CHECK(gmsg_endpoint_write(&e, f1, 4) == GMSG_ERR_BUFFER_FULL);
    CHECK(bk.used == 4);
    gmsg_endpoint_init(&e, &s, NULL, NULL);
    CHECK(gmsg_endpoint_write(&e, f1, 4) == GMSG_ERR_BUFFER_FULL);
    CHECK(gmsg_endpoint_read(&e, SIZE_MAX) == 0);

    /* a sink without writable(): whole frame fails after a short write; retry finishes it (idle in between) */
    snk = gmsg_sink_fn(t_slow_write, NULL);
    gmsg_endpoint_init(&e, &s, NULL, &snk);
    gmsg_endpoint_set_clock(&e, t_clock, NULL);
    gmsg_endpoint_set_idle(&e, t_idle, NULL);
    g_out_n = 0;
    g_take = 1;
    CHECK(gmsg_endpoint_write(&e, frames, 9) == GMSG_ERR_BUFFER_FULL);
    CHECK(g_out_n == 1);
    g_out_n = 0;
    g_idles = 0;
    gmsg_endpoint_set_send_policy(&e, GMSG_SEND_RETRY, GMSG_FOREVER);
    CHECK(gmsg_endpoint_write(&e, frames, 9) == GMSG_OK);
    CHECK(g_out_n == 9 && g_idles == 8);
    for (i = 0; i < 9; ++i) CHECK(g_out[i] == frames[i]);
    /* retry with a limit: a sink that stops taking bytes */
    g_out_n = 0;
    g_now = 100;
    g_take = 0;
    gmsg_endpoint_set_send_policy(&e, GMSG_SEND_RETRY, 20);
    CHECK(gmsg_endpoint_write(&e, frames, 9) == GMSG_ERR_TIMEOUT);
    CHECK(g_now == 120);

    /* a ring as the sink: room checked first */
    CHECK(gmsg_ring_init(&ring, ring_buf, sizeof ring_buf) == GMSG_OK);
    snk = gmsg_sink_ring(&ring);
    gmsg_endpoint_init(&e, &s, NULL, &snk);
    CHECK(gmsg_ring_room(&ring) == 8);
    CHECK(gmsg_endpoint_write(&e, f1, 4) == GMSG_OK);
    CHECK(gmsg_endpoint_write(&e, frames, 5) == GMSG_ERR_BUFFER_FULL);
    CHECK(gmsg_ring_size(&ring) == 4 && gmsg_ring_room(&ring) == 4);

    /* the wait loop: pull until it succeeds; timeout in clock units; no clock: look once */
    gmsg_endpoint_init(&e, &s, NULL, NULL);
    gmsg_endpoint_set_clock(&e, t_clock, NULL);
    gmsg_endpoint_set_idle(&e, t_idle, NULL);
    g_pulls = 0;
    CHECK(gmsg_endpoint_wait(&e, 10, t_pull_third, NULL) == GMSG_OK);
    CHECK(g_pulls == 3);
    g_now = 0xFFFFFFF0u; /* wraps during the wait */
    g_pulls = 0;
    CHECK(gmsg_endpoint_wait(&e, 30, t_pull_never, NULL) == GMSG_ERR_TIMEOUT);
    CHECK(g_now == 0xFFFFFFF0u + 30u);
    CHECK(g_pulls == 32);
    gmsg_endpoint_set_clock(&e, NULL, NULL);
    g_pulls = 0;
    CHECK(gmsg_endpoint_wait(&e, 30, t_pull_never, NULL) == GMSG_ERR_TIMEOUT);
    CHECK(g_pulls == 2);

    /* DMA source across the wrap (no lap tracking) */
    gmsg_stream_reset(&s);
    g_dma_pos = 0;
    g_dma_src = NULL;
    src = gmsg_dma_source_init(&dma, g_dma, sizeof g_dma, t_dma_pos, NULL, false);
    gmsg_endpoint_init(&e, &s, &src, NULL);
    g_log_n = 0;
    g_log[0] = 0;
    for (i = 0; i < 6; ++i) { /* 9 bytes at a time into 16: wraps */
        dma_write(frames + 4, 9);
        (void)gmsg_endpoint_service(&e, SIZE_MAX);
    }
    CHECK(log_is("E2:1 P2 P4 E2:1 P2 P4 E2:1 P2 P4 E2:1 P2 P4 E2:1 P2 P4 E2:1 P2 P4 "));
    /* with lap tracking: more than 16 unread bytes is an overrun; a frame in progress is dropped */
    gmsg_stream_reset(&s);
    g_dma_pos = 0;
    src = gmsg_dma_source_init(&dma, g_dma, sizeof g_dma, t_dma_pos, NULL, true);
    g_dma_src = &dma;
    gmsg_endpoint_init(&e, &s, &src, NULL);
    g_log_n = 0;
    g_log[0] = 0;
    dma_write(frames, 2); /* the start of P1 */
    (void)gmsg_endpoint_service(&e, SIZE_MAX);
    for (i = 0; i < 5; ++i) dma_write(frames + 9, 4); /* 20 bytes: laps */
    (void)gmsg_endpoint_service(&e, SIZE_MAX);
    dma_write(f1, 4);
    (void)gmsg_endpoint_service(&e, SIZE_MAX);
    CHECK(log_is("E16:2 P1 "));
    CHECK(dma.overruns == 1);
    CHECK(s.stats.overruns == 1);
    g_dma_src = NULL;
}

int main(void) {
    test_config();
    test_error();
    test_mem();
    test_endian();
    test_bits();
    test_reader_writer();
    test_checksums();
    test_crc_presets_vs_bitwise();
    test_float_text();
    test_ring();
    test_stream_ring();
    test_endpoint();
    printf("%d checks, %d failed (C%s, GMSG_USE_LIBC=%d, GMSG_CRC_TABLES=%d, GMSG_RUNTIME_LIBRARY=%d)\n", g_checks,
           g_failures, GMSG_C11 ? "11" : "99", GMSG_USE_LIBC, GMSG_CRC_TABLES, GMSG_RUNTIME_LIBRARY);
    return g_failures;
}
