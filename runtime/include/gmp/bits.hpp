#pragma once
// gmp/bits.hpp - bit level field extraction / insertion.
//
// Bit numbering: bit offset 0 is the first bit of the first byte of the group.
//  - msb_first: the first bit is the most significant bit of byte 0 (network / Motorola order);
//               multi-bit values are read most-significant bit first.
//  - lsb_first: the first bit is the least significant bit of byte 0 (Intel / CAN-Intel order);
//               multi-bit values are read least-significant bit first.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "gmp/endian.hpp"
#include "gmp/result.hpp"

namespace gmp {

/// Extract an unsigned value of `nbits` (1..64) starting at `bit_offset`. Caller ensures bounds.
constexpr std::uint64_t extract_bits(const std::uint8_t* p, std::size_t bit_offset, unsigned nbits,
                                     BitOrder order) noexcept {
    std::uint64_t v = 0;
    if (order == BitOrder::msb_first) {
        for (unsigned i = 0; i < nbits; ++i) {
            const std::size_t b = bit_offset + i;
            const unsigned bit = (p[b >> 3] >> (7u - (b & 7u))) & 1u;
            v = (v << 1) | bit;
        }
    } else {
        for (unsigned i = 0; i < nbits; ++i) {
            const std::size_t b = bit_offset + i;
            const std::uint64_t bit = (p[b >> 3] >> (b & 7u)) & 1u;
            v |= bit << i;
        }
    }
    return v;
}

constexpr void insert_bits(std::uint8_t* p, std::size_t bit_offset, unsigned nbits, std::uint64_t value,
                           BitOrder order) noexcept {
    if (order == BitOrder::msb_first) {
        for (unsigned i = 0; i < nbits; ++i) {
            const std::size_t b = bit_offset + i;
            const unsigned bit = static_cast<unsigned>((value >> (nbits - 1 - i)) & 1u);
            const auto mask = static_cast<std::uint8_t>(1u << (7u - (b & 7u)));
            p[b >> 3] = static_cast<std::uint8_t>(bit ? (p[b >> 3] | mask) : (p[b >> 3] & ~mask));
        }
    } else {
        for (unsigned i = 0; i < nbits; ++i) {
            const std::size_t b = bit_offset + i;
            const unsigned bit = static_cast<unsigned>((value >> i) & 1u);
            const auto mask = static_cast<std::uint8_t>(1u << (b & 7u));
            p[b >> 3] = static_cast<std::uint8_t>(bit ? (p[b >> 3] | mask) : (p[b >> 3] & ~mask));
        }
    }
}

/// Sign-extend the low `nbits` of v.
constexpr std::int64_t sign_extend(std::uint64_t v, unsigned nbits) noexcept {
    if (nbits >= 64) return static_cast<std::int64_t>(v);
    const std::uint64_t m = std::uint64_t{1} << (nbits - 1);
    v &= (std::uint64_t{1} << nbits) - 1;
    return static_cast<std::int64_t>((v ^ m) - m);
}

/// Extract a bit field and convert it to T (unsigned, signed, bool or enum).
template <class T>
constexpr T get_bits(const std::uint8_t* p, std::size_t bit_offset, unsigned nbits, BitOrder order) noexcept {
    const std::uint64_t raw = extract_bits(p, bit_offset, nbits, order);
    if constexpr (std::is_same_v<T, bool>) {
        return raw != 0;
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(raw));
    } else if constexpr (std::is_signed_v<T>) {
        return static_cast<T>(sign_extend(raw, nbits));
    } else {
        return static_cast<T>(raw);
    }
}

template <class T>
constexpr void set_bits(std::uint8_t* p, std::size_t bit_offset, unsigned nbits, T value, BitOrder order) noexcept {
    std::uint64_t raw;
    if constexpr (std::is_enum_v<T>) raw = static_cast<std::uint64_t>(static_cast<std::underlying_type_t<T>>(value));
    else raw = static_cast<std::uint64_t>(value);
    if (nbits < 64) raw &= (std::uint64_t{1} << nbits) - 1;
    insert_bits(p, bit_offset, nbits, raw, order);
}

/// Sequential bit reader over a byte span (for streaming bit fields not tied to a group).
class BitReader {
public:
    constexpr BitReader(bytes_view data, BitOrder order) noexcept : data_(data), order_(order) {}
    [[nodiscard]] constexpr std::size_t bit_position() const noexcept { return bit_; }
    [[nodiscard]] constexpr std::size_t bits_remaining() const noexcept { return data_.size() * 8 - bit_; }

    [[nodiscard]] constexpr Result<std::uint64_t> read(unsigned nbits) noexcept {
        if (nbits > 64 || bits_remaining() < nbits) return fail(Error::Truncated);
        auto v = extract_bits(data_.data(), bit_, nbits, order_);
        bit_ += nbits;
        return v;
    }

private:
    bytes_view data_;
    BitOrder order_;
    std::size_t bit_ = 0;
};

/// Sequential bit writer into a byte span. Untouched bits are left as-is (zero the buffer first).
class BitWriter {
public:
    constexpr BitWriter(mut_bytes data, BitOrder order) noexcept : data_(data), order_(order) {}
    [[nodiscard]] constexpr std::size_t bit_position() const noexcept { return bit_; }

    [[nodiscard]] constexpr VoidResult write(std::uint64_t value, unsigned nbits) noexcept {
        if (nbits > 64 || data_.size() * 8 - bit_ < nbits) return fail(Error::BufferFull);
        if (nbits < 64) value &= (std::uint64_t{1} << nbits) - 1;
        insert_bits(data_.data(), bit_, nbits, value, order_);
        bit_ += nbits;
        return {};
    }

private:
    mut_bytes data_;
    BitOrder order_;
    std::size_t bit_ = 0;
};

} // namespace gmp
