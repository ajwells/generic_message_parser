/* gmp/c/bits.h - bit level field extraction / insertion (matches gmp/bits.hpp).
 *
 * Bit numbering: bit offset 0 is the first bit of the first byte of the group.
 *  - GMSG_MSB_FIRST: the first bit is the most significant bit of byte 0 (network / Motorola order);
 *                   multi-bit values are read most-significant bit first.
 *  - GMSG_LSB_FIRST: the first bit is the least significant bit of byte 0 (Intel / CAN-Intel order);
 *                   multi-bit values are read least-significant bit first.
 */
#ifndef GMSG_C_BITS_H
#define GMSG_C_BITS_H

#include "gmp/c/config.h"
#include "gmp/c/endian.h"

GMSG_C_BEGIN

/* Extract an unsigned value of nbits (1..64) starting at bit_offset. The caller ensures bounds. */
GMSG_INLINE uint64_t gmsg_get_bits(const uint8_t* p, size_t bit_offset, unsigned nbits, gmsg_bit_order order) {
    uint64_t v = 0;
    unsigned i;
    GMSG_ASSERT(nbits >= 1 && nbits <= 64);
    if (order == GMSG_MSB_FIRST) {
        for (i = 0; i < nbits; ++i) {
            const size_t b = bit_offset + i;
            const unsigned bit = (unsigned)(p[b >> 3] >> (7u - (b & 7u))) & 1u;
            v = (v << 1) | bit;
        }
    } else {
        for (i = 0; i < nbits; ++i) {
            const size_t b = bit_offset + i;
            const uint64_t bit = (uint64_t)(p[b >> 3] >> (b & 7u)) & 1u;
            v |= bit << i;
        }
    }
    return v;
}

/* Write the low nbits (1..64) of value at bit_offset; other bits are left unchanged. */
GMSG_INLINE void gmsg_set_bits(uint8_t* p, size_t bit_offset, unsigned nbits, uint64_t value, gmsg_bit_order order) {
    unsigned i;
    GMSG_ASSERT(nbits >= 1 && nbits <= 64);
    for (i = 0; i < nbits; ++i) {
        const size_t b = bit_offset + i;
        const unsigned bit = (unsigned)(order == GMSG_MSB_FIRST ? (value >> (nbits - 1 - i)) : (value >> i)) & 1u;
        const uint8_t mask = (uint8_t)(order == GMSG_MSB_FIRST ? (1u << (7u - (b & 7u))) : (1u << (b & 7u)));
        p[b >> 3] = (uint8_t)(bit ? (p[b >> 3] | mask) : (p[b >> 3] & (uint8_t)~mask));
    }
}

/* Sign-extend the low nbits (1..64) of v. */
GMSG_INLINE int64_t gmsg_sign_extend(uint64_t v, unsigned nbits) {
    uint64_t m;
    GMSG_ASSERT(nbits >= 1 && nbits <= 64);
    if (nbits >= 64) return gmsg_i64_from_bits(v);
    m = (uint64_t)1 << (nbits - 1);
    v &= ((uint64_t)1 << nbits) - 1;
    return gmsg_i64_from_bits((v ^ m) - m);
}

/* Signed bit field: extract and sign-extend. */
GMSG_INLINE int64_t gmsg_get_bits_signed(const uint8_t* p, size_t bit_offset, unsigned nbits, gmsg_bit_order order) {
    return gmsg_sign_extend(gmsg_get_bits(p, bit_offset, nbits, order), nbits);
}

/* Signed bit field: the value is truncated to nbits (two's complement). */
GMSG_INLINE void gmsg_set_bits_signed(uint8_t* p, size_t bit_offset, unsigned nbits, int64_t value, gmsg_bit_order order) {
    gmsg_set_bits(p, bit_offset, nbits, (uint64_t)value, order);
}

GMSG_C_END

#endif /* GMSG_C_BITS_H */
