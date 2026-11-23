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
    CHECK(GMSG_ERR_VALUE_OUT_OF_LIMITS == GMSG_ERROR_COUNT - 1);
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
    printf("%d checks, %d failed (C%s, GMSG_USE_LIBC=%d, GMSG_CRC_TABLES=%d, GMSG_RUNTIME_LIBRARY=%d)\n", g_checks,
           g_failures, GMSG_C11 ? "11" : "99", GMSG_USE_LIBC, GMSG_CRC_TABLES, GMSG_RUNTIME_LIBRARY);
    return g_failures;
}
