/* gmp/c/endian.h - endian-aware loads and stores from unaligned byte buffers.
 *
 * For every type t in u8 u16 u32 u64 i8 i16 i32 i64 f32 f64:
 *     t gmsg_load_<t>(const uint8_t* p, gmsg_endian e);     gmsg_load_<t>_be(p) / gmsg_load_<t>_le(p)
 *     void gmsg_store_<t>(uint8_t* p, t v, gmsg_endian e);  gmsg_store_<t>_be(p, v) / gmsg_store_<t>_le(p, v)
 * plus gmsg_load_bool / gmsg_store_bool and gmsg_load_uint / gmsg_store_uint for odd widths (1..8 bytes).
 * No alignment requirement; floats are IEEE 754 bit patterns. Matches gmsg::load / gmsg::store in C++.
 */
#ifndef GMSG_C_ENDIAN_H
#define GMSG_C_ENDIAN_H

#include "gmp/c/config.h"

GMSG_C_BEGIN

typedef enum gmsg_endian { GMSG_ENDIAN_BIG = 0, GMSG_ENDIAN_LITTLE = 1 } gmsg_endian;
typedef enum gmsg_bit_order { GMSG_MSB_FIRST = 0, GMSG_LSB_FIRST = 1 } gmsg_bit_order;

/* Load an unsigned integer of n bytes (1..8). */
GMSG_INLINE uint64_t gmsg_load_uint(const uint8_t* p, size_t n, gmsg_endian e) {
    uint64_t v = 0;
    size_t i;
    GMSG_ASSERT(n >= 1 && n <= 8);
    if (e == GMSG_ENDIAN_BIG) {
        for (i = 0; i < n; ++i) v = (v << 8) | p[i];
    } else {
        for (i = n; i-- > 0;) v = (v << 8) | p[i];
    }
    return v;
}

/* Store the low n bytes (1..8) of v. */
GMSG_INLINE void gmsg_store_uint(uint8_t* p, size_t n, uint64_t v, gmsg_endian e) {
    size_t i;
    GMSG_ASSERT(n >= 1 && n <= 8);
    if (e == GMSG_ENDIAN_BIG) {
        for (i = n; i-- > 0;) { p[i] = (uint8_t)v; v >>= 8; }
    } else {
        for (i = 0; i < n; ++i) { p[i] = (uint8_t)v; v >>= 8; }
    }
}

GMSG_INLINE bool gmsg_load_bool(const uint8_t* p) { return p[0] != 0; }
GMSG_INLINE void gmsg_store_bool(uint8_t* p, bool v) { p[0] = v ? 1u : 0u; }

/* Bit-pattern conversions. Union type punning is well defined in C (C11 6.5.2.3) and needs no memcpy;
 * if these headers are compiled as C++, GCC, Clang and MSVC document the same behaviour. */
typedef union gmsg_f32_bits { float f; uint32_t u; } gmsg_f32_bits;
typedef union gmsg_f64_bits { double f; uint64_t u; } gmsg_f64_bits;

GMSG_INLINE float gmsg_f32_from_bits(uint32_t u) { gmsg_f32_bits b; b.u = u; return b.f; }
GMSG_INLINE uint32_t gmsg_f32_to_bits(float f) { gmsg_f32_bits b; b.f = f; return b.u; }
GMSG_INLINE double gmsg_f64_from_bits(uint64_t u) { gmsg_f64_bits b; b.u = u; return b.f; }
GMSG_INLINE uint64_t gmsg_f64_to_bits(double f) { gmsg_f64_bits b; b.f = f; return b.u; }

/* Signed values: the unsigned bit pattern is reinterpreted as two's complement without relying on
 * implementation-defined conversions. */
GMSG_INLINE int8_t gmsg_i8_from_bits(uint8_t u) { return u < 0x80u ? (int8_t)u : (int8_t)(-(int)(uint8_t)(~u) - 1); }
GMSG_INLINE int16_t gmsg_i16_from_bits(uint16_t u) {
    return u < 0x8000u ? (int16_t)u : (int16_t)(-(int32_t)(uint16_t)(~u) - 1);
}
GMSG_INLINE int32_t gmsg_i32_from_bits(uint32_t u) {
    return u < 0x80000000u ? (int32_t)u : (int32_t)(-(int64_t)(uint32_t)(~u) - 1);
}
GMSG_INLINE int64_t gmsg_i64_from_bits(uint64_t u) {
    return u < 0x8000000000000000u ? (int64_t)u : -(int64_t)(~u) - 1;
}

#define GMSG_DEFINE_LOAD_STORE_(name, type, n, from_bits, to_bits)                                       \
    GMSG_INLINE type gmsg_load_##name(const uint8_t* p, gmsg_endian e) {                                      \
        return from_bits(gmsg_load_uint(p, n, e));                                                       \
    }                                                                                                   \
    GMSG_INLINE void gmsg_store_##name(uint8_t* p, type v, gmsg_endian e) { gmsg_store_uint(p, n, to_bits(v), e); } \
    GMSG_INLINE type gmsg_load_##name##_be(const uint8_t* p) { return gmsg_load_##name(p, GMSG_ENDIAN_BIG); }    \
    GMSG_INLINE type gmsg_load_##name##_le(const uint8_t* p) { return gmsg_load_##name(p, GMSG_ENDIAN_LITTLE); } \
    GMSG_INLINE void gmsg_store_##name##_be(uint8_t* p, type v) { gmsg_store_##name(p, v, GMSG_ENDIAN_BIG); }     \
    GMSG_INLINE void gmsg_store_##name##_le(uint8_t* p, type v) { gmsg_store_##name(p, v, GMSG_ENDIAN_LITTLE); }

#define GMSG_BITS_U8_(x) ((uint8_t)(x))
#define GMSG_BITS_U16_(x) ((uint16_t)(x))
#define GMSG_BITS_U32_(x) ((uint32_t)(x))
#define GMSG_BITS_U64_(x) ((uint64_t)(x))
#define GMSG_I8_FROM_(x) gmsg_i8_from_bits((uint8_t)(x))
#define GMSG_I16_FROM_(x) gmsg_i16_from_bits((uint16_t)(x))
#define GMSG_I32_FROM_(x) gmsg_i32_from_bits((uint32_t)(x))
#define GMSG_I64_FROM_(x) gmsg_i64_from_bits((uint64_t)(x))
#define GMSG_F32_FROM_(x) gmsg_f32_from_bits((uint32_t)(x))
#define GMSG_F64_FROM_(x) gmsg_f64_from_bits((uint64_t)(x))

GMSG_DEFINE_LOAD_STORE_(u8, uint8_t, 1, GMSG_BITS_U8_, GMSG_BITS_U64_)
GMSG_DEFINE_LOAD_STORE_(u16, uint16_t, 2, GMSG_BITS_U16_, GMSG_BITS_U64_)
GMSG_DEFINE_LOAD_STORE_(u32, uint32_t, 4, GMSG_BITS_U32_, GMSG_BITS_U64_)
GMSG_DEFINE_LOAD_STORE_(u64, uint64_t, 8, GMSG_BITS_U64_, GMSG_BITS_U64_)
/* Signed -> bits: conversion to an unsigned type is well defined (modulo 2^N). */
GMSG_DEFINE_LOAD_STORE_(i8, int8_t, 1, GMSG_I8_FROM_, GMSG_BITS_U8_)
GMSG_DEFINE_LOAD_STORE_(i16, int16_t, 2, GMSG_I16_FROM_, GMSG_BITS_U16_)
GMSG_DEFINE_LOAD_STORE_(i32, int32_t, 4, GMSG_I32_FROM_, GMSG_BITS_U32_)
GMSG_DEFINE_LOAD_STORE_(i64, int64_t, 8, GMSG_I64_FROM_, GMSG_BITS_U64_)
GMSG_DEFINE_LOAD_STORE_(f32, float, 4, GMSG_F32_FROM_, gmsg_f32_to_bits)
GMSG_DEFINE_LOAD_STORE_(f64, double, 8, GMSG_F64_FROM_, gmsg_f64_to_bits)

GMSG_C_END

#endif /* GMSG_C_ENDIAN_H */
