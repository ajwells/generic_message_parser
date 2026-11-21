#pragma once
// gmp/endian.hpp - constexpr endian-aware loads and stores for arithmetic types.

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace gmp {

enum class Endian : std::uint8_t { big, little };

enum class BitOrder : std::uint8_t { msb_first, lsb_first };

namespace detail {

template <std::size_t N>
struct uint_of_size;
template <> struct uint_of_size<1> { using type = std::uint8_t; };
template <> struct uint_of_size<2> { using type = std::uint16_t; };
template <> struct uint_of_size<4> { using type = std::uint32_t; };
template <> struct uint_of_size<8> { using type = std::uint64_t; };

template <std::size_t N>
using uint_of_size_t = typename uint_of_size<N>::type;

} // namespace detail

template <class T>
concept Arithmetic = std::is_arithmetic_v<T> || std::is_enum_v<T>;

/// Load an unsigned integer of `n` bytes (1..8) - used for odd widths (e.g. u24 length fields).
constexpr std::uint64_t load_uint(const std::uint8_t* p, std::size_t n, Endian e) noexcept {
    std::uint64_t v = 0;
    if (e == Endian::big) {
        for (std::size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
    } else {
        for (std::size_t i = n; i-- > 0;) v = (v << 8) | p[i];
    }
    return v;
}

constexpr void store_uint(std::uint8_t* p, std::size_t n, std::uint64_t v, Endian e) noexcept {
    if (e == Endian::big) {
        for (std::size_t i = n; i-- > 0;) { p[i] = static_cast<std::uint8_t>(v); v >>= 8; }
    } else {
        for (std::size_t i = 0; i < n; ++i) { p[i] = static_cast<std::uint8_t>(v); v >>= 8; }
    }
}

/// Load a T (integral, enum, bool, float, double) from raw bytes. No alignment requirement.
template <Arithmetic T>
constexpr T load(const std::uint8_t* p, Endian e) noexcept {
    if constexpr (std::is_same_v<T, bool>) {
        return p[0] != 0;
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(load<std::underlying_type_t<T>>(p, e));
    } else {
        using U = detail::uint_of_size_t<sizeof(T)>;
        const U raw = static_cast<U>(load_uint(p, sizeof(T), e));
        return std::bit_cast<T>(raw);
    }
}

template <Arithmetic T>
constexpr void store(std::uint8_t* p, T value, Endian e) noexcept {
    if constexpr (std::is_same_v<T, bool>) {
        p[0] = value ? 1 : 0;
    } else if constexpr (std::is_enum_v<T>) {
        store(p, static_cast<std::underlying_type_t<T>>(value), e);
    } else {
        using U = detail::uint_of_size_t<sizeof(T)>;
        store_uint(p, sizeof(T), static_cast<std::uint64_t>(std::bit_cast<U>(value)), e);
    }
}

/// Convert enums/bools/integers to int64 for use in generated expressions.
template <class T>
constexpr std::int64_t to_int(T v) noexcept {
    if constexpr (std::is_enum_v<T>) return static_cast<std::int64_t>(static_cast<std::underlying_type_t<T>>(v));
    else return static_cast<std::int64_t>(v);
}

} // namespace gmp
