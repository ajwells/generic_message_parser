#pragma once
// gmp/checksum.hpp - constexpr checksum and CRC algorithms.
//
// Every algorithm exposes the same static interface so generated code can use them uniformly:
//     using value_type = <unsigned type>;
//     static constexpr std::size_t size;                       // bytes on the wire
//     static constexpr value_type compute(gmsg::bytes_view);    // whole buffer
//     static constexpr value_type init();                      // incremental API
//     static constexpr value_type update(value_type, gmsg::bytes_view);
//     static constexpr value_type finish(value_type);

#include <array>
#include <cstddef>
#include <cstdint>

#include "gmp/result.hpp"

namespace gmsg::checksum {

namespace detail {

template <class T>
constexpr T reflect(T v, unsigned width) noexcept {
    T r = 0;
    for (unsigned i = 0; i < width; ++i) {
        r = static_cast<T>((r << 1) | (v & 1u));
        v = static_cast<T>(v >> 1);
    }
    return r;
}

template <class T, unsigned W>
constexpr T mask() noexcept {
    if constexpr (W >= sizeof(T) * 8) return static_cast<T>(~T{0});
    else return static_cast<T>((T{1} << W) - 1);
}

} // namespace detail

/// Generic parameterised CRC (Rocksoft model). Width W in bits (1..64), stored in T.
template <class T, unsigned W, T Poly, T Init, bool RefIn, bool RefOut, T XorOut>
struct Crc {
    using value_type = T;
    static constexpr unsigned width = W;
    static constexpr std::size_t size = (W + 7) / 8;
    static constexpr T kMask = detail::mask<T, W>();

private:
    static constexpr bool kUseTable = (W >= 8);

    static constexpr std::array<T, 256> make_table() noexcept {
        std::array<T, 256> t{};
        if constexpr (kUseTable) {
            if constexpr (RefIn) {
                const T rpoly = detail::reflect<T>(Poly, W);
                for (unsigned i = 0; i < 256; ++i) {
                    T c = static_cast<T>(i);
                    for (int k = 0; k < 8; ++k) c = (c & 1u) ? static_cast<T>((c >> 1) ^ rpoly) : static_cast<T>(c >> 1);
                    t[i] = c;
                }
            } else {
                const T top = static_cast<T>(T{1} << (W - 1));
                for (unsigned i = 0; i < 256; ++i) {
                    T c = static_cast<T>(static_cast<T>(i) << (W - 8));
                    for (int k = 0; k < 8; ++k)
                        c = (c & top) ? static_cast<T>(((c << 1) ^ Poly) & kMask) : static_cast<T>((c << 1) & kMask);
                    t[i] = c;
                }
            }
        }
        return t;
    }

    static constexpr std::array<T, 256> kTable = make_table();

public:
    static constexpr T init() noexcept {
        if constexpr (RefIn) return detail::reflect<T>(Init, W);
        else return Init;
    }

    static constexpr T update(T crc, bytes_view data) noexcept {
        if constexpr (kUseTable) {
            if constexpr (RefIn) {
                for (auto b : data) crc = static_cast<T>(kTable[(crc ^ b) & 0xFFu] ^ (crc >> 8));
            } else {
                for (auto b : data)
                    crc = static_cast<T>((kTable[((crc >> (W - 8)) ^ b) & 0xFFu] ^ (crc << 8)) & kMask);
            }
        } else {
            // Bitwise fallback for widths < 8.
            if constexpr (RefIn) {
                const T rpoly = detail::reflect<T>(Poly, W);
                for (auto b : data) {
                    crc = static_cast<T>(crc ^ b);
                    for (int k = 0; k < 8; ++k) crc = (crc & 1u) ? static_cast<T>((crc >> 1) ^ rpoly) : static_cast<T>(crc >> 1);
                    crc &= kMask;
                }
            } else {
                for (auto b : data) {
                    for (int k = 7; k >= 0; --k) {
                        const unsigned bit = ((crc >> (W - 1)) & 1u) ^ ((b >> k) & 1u);
                        crc = static_cast<T>((crc << 1) & kMask);
                        if (bit) crc = static_cast<T>(crc ^ Poly);
                    }
                }
            }
        }
        return crc;
    }

    static constexpr T finish(T crc) noexcept {
        if constexpr (RefIn != RefOut) crc = detail::reflect<T>(crc, W);
        return static_cast<T>((crc ^ XorOut) & kMask);
    }

    static constexpr T compute(bytes_view data) noexcept { return finish(update(init(), data)); }
};

// --- CRC presets (names follow the reveng.sourceforge.io catalogue) ----------------------------
using crc8          = Crc<std::uint8_t, 8, 0x07, 0x00, false, false, 0x00>;          // CRC-8/SMBUS
using crc8_maxim    = Crc<std::uint8_t, 8, 0x31, 0x00, true, true, 0x00>;            // CRC-8/MAXIM-DOW (1-Wire)
using crc8_autosar  = Crc<std::uint8_t, 8, 0x2F, 0xFF, false, false, 0xFF>;          // CRC-8/AUTOSAR
using crc8_nrsc5    = Crc<std::uint8_t, 8, 0x31, 0xFF, false, false, 0x00>;          // CRC-8/NRSC-5 (Sensirion SHT4x/SCD4x/SGP4x word CRC)
using crc15_can     = Crc<std::uint16_t, 15, 0x4599, 0x0000, false, false, 0x0000>;  // CRC-15/CAN
using crc16_ccitt   = Crc<std::uint16_t, 16, 0x1021, 0xFFFF, false, false, 0x0000>;  // CRC-16/IBM-3740 ("CCITT-FALSE")
using crc16_xmodem  = Crc<std::uint16_t, 16, 0x1021, 0x0000, false, false, 0x0000>;  // CRC-16/XMODEM
using crc16_kermit  = Crc<std::uint16_t, 16, 0x1021, 0x0000, true, true, 0x0000>;    // CRC-16/KERMIT
using crc16_x25     = Crc<std::uint16_t, 16, 0x1021, 0xFFFF, true, true, 0xFFFF>;    // CRC-16/IBM-SDLC (X.25, HDLC)
using crc16_modbus  = Crc<std::uint16_t, 16, 0x8005, 0xFFFF, true, true, 0x0000>;    // CRC-16/MODBUS
using crc16_arc     = Crc<std::uint16_t, 16, 0x8005, 0x0000, true, true, 0x0000>;    // CRC-16/ARC
using crc24_openpgp = Crc<std::uint32_t, 24, 0x864CFB, 0xB704CE, false, false, 0x000000>; // CRC-24/OPENPGP
using crc32         = Crc<std::uint32_t, 32, 0x04C11DB7, 0xFFFFFFFF, true, true, 0xFFFFFFFF>; // CRC-32/ISO-HDLC
using crc32c        = Crc<std::uint32_t, 32, 0x1EDC6F41, 0xFFFFFFFF, true, true, 0xFFFFFFFF>; // CRC-32/ISCSI
using crc32_mpeg2   = Crc<std::uint32_t, 32, 0x04C11DB7, 0xFFFFFFFF, false, false, 0x00000000>; // CRC-32/MPEG-2
using crc64_ecma    = Crc<std::uint64_t, 64, 0x42F0E1EBA9EA3693ull, 0, false, false, 0>;  // CRC-64/ECMA-182

// --- Simple checksums ----------------------------------------------------------------------------

/// XOR of all bytes (NMEA 0183, many serial protocols).
struct xor8 {
    using value_type = std::uint8_t;
    static constexpr std::size_t size = 1;
    static constexpr value_type init() noexcept { return 0; }
    static constexpr value_type update(value_type s, bytes_view d) noexcept {
        for (auto b : d) s = static_cast<value_type>(s ^ b);
        return s;
    }
    static constexpr value_type finish(value_type s) noexcept { return s; }
    static constexpr value_type compute(bytes_view d) noexcept { return update(init(), d); }
};

/// Modulo-256 sum of all bytes.
struct sum8 {
    using value_type = std::uint8_t;
    static constexpr std::size_t size = 1;
    static constexpr value_type init() noexcept { return 0; }
    static constexpr value_type update(value_type s, bytes_view d) noexcept {
        for (auto b : d) s = static_cast<value_type>(s + b);
        return s;
    }
    static constexpr value_type finish(value_type s) noexcept { return s; }
    static constexpr value_type compute(bytes_view d) noexcept { return update(init(), d); }
};

/// Two's complement of the modulo-256 sum (LRC, e.g. Modbus ASCII, Intel HEX).
struct lrc8 {
    using value_type = std::uint8_t;
    static constexpr std::size_t size = 1;
    static constexpr value_type init() noexcept { return 0; }
    static constexpr value_type update(value_type s, bytes_view d) noexcept { return sum8::update(s, d); }
    static constexpr value_type finish(value_type s) noexcept { return static_cast<value_type>(-s); }
    static constexpr value_type compute(bytes_view d) noexcept { return finish(update(init(), d)); }
};

/// Modulo-65536 sum of all bytes.
struct sum16 {
    using value_type = std::uint16_t;
    static constexpr std::size_t size = 2;
    static constexpr value_type init() noexcept { return 0; }
    static constexpr value_type update(value_type s, bytes_view d) noexcept {
        for (auto b : d) s = static_cast<value_type>(s + b);
        return s;
    }
    static constexpr value_type finish(value_type s) noexcept { return s; }
    static constexpr value_type compute(bytes_view d) noexcept { return update(init(), d); }
};

/// Fletcher-16 (sum2 << 8 | sum1).
struct fletcher16 {
    using value_type = std::uint16_t;
    static constexpr std::size_t size = 2;
    static constexpr value_type init() noexcept { return 0; }
    static constexpr value_type update(value_type s, bytes_view d) noexcept {
        unsigned s1 = s & 0xFFu, s2 = (s >> 8) & 0xFFu;
        for (auto b : d) {
            s1 = (s1 + b) % 255u;
            s2 = (s2 + s1) % 255u;
        }
        return static_cast<value_type>((s2 << 8) | s1);
    }
    static constexpr value_type finish(value_type s) noexcept { return s; }
    static constexpr value_type compute(bytes_view d) noexcept { return update(init(), d); }
};

/// 8-bit Fletcher as used by u-blox UBX (CK_A, CK_B); value is (CK_A << 8) | CK_B, write big endian.
struct fletcher8_ubx {
    using value_type = std::uint16_t;
    static constexpr std::size_t size = 2;
    static constexpr value_type init() noexcept { return 0; }
    static constexpr value_type update(value_type s, bytes_view d) noexcept {
        std::uint8_t a = static_cast<std::uint8_t>(s >> 8), b = static_cast<std::uint8_t>(s);
        for (auto x : d) {
            a = static_cast<std::uint8_t>(a + x);
            b = static_cast<std::uint8_t>(b + a);
        }
        return static_cast<value_type>((a << 8) | b);
    }
    static constexpr value_type finish(value_type s) noexcept { return s; }
    static constexpr value_type compute(bytes_view d) noexcept { return update(init(), d); }
};

} // namespace gmsg::checksum
