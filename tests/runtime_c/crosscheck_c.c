/* C side of the C/C++ runtime cross-check (compiled as C11). Exposes the C runtime through plain functions that
 * test_c_vs_cpp.cpp calls and compares with the C++ runtime. */
#include "crosscheck_c.h"

#include "gmp/c/runtime.h"

int gmpct_checksum_count(void) { return GMPCT_CHECKSUM_COUNT; }

uint64_t gmpct_checksum(int algo, const uint8_t* d, size_t n) {
    switch (algo) {
        case 0: return gmsg_crc8_compute(d, n);
        case 1: return gmsg_crc8_maxim_compute(d, n);
        case 2: return gmsg_crc8_autosar_compute(d, n);
        case 3: return gmsg_crc8_nrsc5_compute(d, n);
        case 4: return gmsg_crc15_can_compute(d, n);
        case 5: return gmsg_crc16_ccitt_compute(d, n);
        case 6: return gmsg_crc16_xmodem_compute(d, n);
        case 7: return gmsg_crc16_kermit_compute(d, n);
        case 8: return gmsg_crc16_x25_compute(d, n);
        case 9: return gmsg_crc16_modbus_compute(d, n);
        case 10: return gmsg_crc16_arc_compute(d, n);
        case 11: return gmsg_crc24_openpgp_compute(d, n);
        case 12: return gmsg_crc32_compute(d, n);
        case 13: return gmsg_crc32c_compute(d, n);
        case 14: return gmsg_crc32_mpeg2_compute(d, n);
        case 15: return gmsg_crc64_ecma_compute(d, n);
        case 16: return gmsg_xor8_compute(d, n);
        case 17: return gmsg_sum8_compute(d, n);
        case 18: return gmsg_lrc8_compute(d, n);
        case 19: return gmsg_sum16_compute(d, n);
        case 20: return gmsg_fletcher16_compute(d, n);
        case 21: return gmsg_fletcher8_ubx_compute(d, n);
        default: return 0;
    }
}

uint64_t gmpct_checksum_split(int algo, const uint8_t* d, size_t n, size_t split) {
    /* Incremental API over two chunks. */
    switch (algo) {
        case 9: {
            uint16_t s = gmsg_crc16_modbus_init();
            s = gmsg_crc16_modbus_update(s, d, split);
            return gmsg_crc16_modbus_finish(gmsg_crc16_modbus_update(s, d + split, n - split));
        }
        case 12: {
            uint32_t s = gmsg_crc32_init();
            s = gmsg_crc32_update(s, d, split);
            return gmsg_crc32_finish(gmsg_crc32_update(s, d + split, n - split));
        }
        case 20: {
            uint16_t s = gmsg_fletcher16_init();
            s = gmsg_fletcher16_update(s, d, split);
            return gmsg_fletcher16_finish(gmsg_fletcher16_update(s, d + split, n - split));
        }
        default: return gmpct_checksum(algo, d, n);
    }
}

uint64_t gmpct_crc_params(unsigned width, bool refin, bool refout, uint64_t poly, uint64_t init, uint64_t xorout,
                          const uint8_t* d, size_t n) {
    gmsg_crc_params p;
    p.width = (uint8_t)width;
    p.refin = refin;
    p.refout = refout;
    p.poly = poly;
    p.init = init;
    p.xorout = xorout;
    return gmsg_crc_compute(&p, d, n);
}

uint64_t gmpct_load(int type, const uint8_t* p, int big) {
    const gmsg_endian e = big ? GMSG_ENDIAN_BIG : GMSG_ENDIAN_LITTLE;
    switch (type) {
        case GMPCT_U8: return gmsg_load_u8(p, e);
        case GMPCT_U16: return gmsg_load_u16(p, e);
        case GMPCT_U32: return gmsg_load_u32(p, e);
        case GMPCT_U64: return gmsg_load_u64(p, e);
        case GMPCT_I8: return (uint64_t)(int64_t)gmsg_load_i8(p, e);
        case GMPCT_I16: return (uint64_t)(int64_t)gmsg_load_i16(p, e);
        case GMPCT_I32: return (uint64_t)(int64_t)gmsg_load_i32(p, e);
        case GMPCT_I64: return (uint64_t)gmsg_load_i64(p, e);
        case GMPCT_F32: return gmsg_f32_to_bits(gmsg_load_f32(p, e));
        case GMPCT_F64: return gmsg_f64_to_bits(gmsg_load_f64(p, e));
        default: return 0;
    }
}

void gmpct_store(int type, uint8_t* p, uint64_t bits, int big) {
    const gmsg_endian e = big ? GMSG_ENDIAN_BIG : GMSG_ENDIAN_LITTLE;
    switch (type) {
        case GMPCT_U8: gmsg_store_u8(p, (uint8_t)bits, e); break;
        case GMPCT_U16: gmsg_store_u16(p, (uint16_t)bits, e); break;
        case GMPCT_U32: gmsg_store_u32(p, (uint32_t)bits, e); break;
        case GMPCT_U64: gmsg_store_u64(p, bits, e); break;
        case GMPCT_I8: gmsg_store_i8(p, gmsg_i8_from_bits((uint8_t)bits), e); break;
        case GMPCT_I16: gmsg_store_i16(p, gmsg_i16_from_bits((uint16_t)bits), e); break;
        case GMPCT_I32: gmsg_store_i32(p, gmsg_i32_from_bits((uint32_t)bits), e); break;
        case GMPCT_I64: gmsg_store_i64(p, gmsg_i64_from_bits(bits), e); break;
        case GMPCT_F32: gmsg_store_f32(p, gmsg_f32_from_bits((uint32_t)bits), e); break;
        case GMPCT_F64: gmsg_store_f64(p, gmsg_f64_from_bits(bits), e); break;
        default: break;
    }
}

uint64_t gmpct_get_bits(const uint8_t* p, size_t off, unsigned n, int msb) {
    return gmsg_get_bits(p, off, n, msb ? GMSG_MSB_FIRST : GMSG_LSB_FIRST);
}

int64_t gmpct_get_bits_signed(const uint8_t* p, size_t off, unsigned n, int msb) {
    return gmsg_get_bits_signed(p, off, n, msb ? GMSG_MSB_FIRST : GMSG_LSB_FIRST);
}

void gmpct_set_bits(uint8_t* p, size_t off, unsigned n, uint64_t v, int msb) {
    gmsg_set_bits(p, off, n, v, msb ? GMSG_MSB_FIRST : GMSG_LSB_FIRST);
}

int gmpct_error_count(void) { return GMSG_ERROR_COUNT; }
const char* gmpct_error_str(int e) { return gmsg_error_str((gmsg_error)e); }
