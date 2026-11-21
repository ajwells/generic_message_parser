#pragma once
// gmp/framing/escape.hpp - byte stuffing for delimited frames.
//
// Two styles are supported:
//  - XOR style (HDLC/PPP): reserved byte b is sent as {esc, b ^ xor_mask}.
//  - Map style (SLIP):     reserved byte b is sent as {esc, code(b)} using an explicit table.
// Reserved bytes are the escape byte itself plus the start/end delimiter bytes (xor style),
// or exactly the bytes listed in the map (map style).

#include <array>
#include <cstddef>
#include <cstdint>

#include "gmp/result.hpp"

namespace gmp {

struct EscapeConfig {
    bool enabled = false;
    std::uint8_t esc = 0;
    std::uint8_t xor_mask = 0x20;
    std::uint8_t map_len = 0;                                   // > 0 selects map style
    std::array<std::uint8_t, 4> map_raw{};                      // raw byte
    std::array<std::uint8_t, 4> map_code{};                     // code sent after esc
    std::array<std::uint8_t, 4> reserved{};                     // xor style: bytes that must be escaped (besides esc)
    std::uint8_t reserved_len = 0;

    [[nodiscard]] constexpr bool needs_escape(std::uint8_t b) const noexcept {
        if (map_len) {
            for (std::size_t i = 0; i < map_len; ++i) if (map_raw[i] == b) return true;
            return false;
        }
        if (b == esc) return true;
        for (std::size_t i = 0; i < reserved_len; ++i) if (reserved[i] == b) return true;
        return false;
    }
    [[nodiscard]] constexpr std::uint8_t encode(std::uint8_t b) const noexcept {
        if (map_len) {
            for (std::size_t i = 0; i < map_len; ++i) if (map_raw[i] == b) return map_code[i];
        }
        return static_cast<std::uint8_t>(b ^ xor_mask);
    }
    [[nodiscard]] constexpr Result<std::uint8_t> decode(std::uint8_t code) const noexcept {
        if (map_len) {
            for (std::size_t i = 0; i < map_len; ++i) if (map_code[i] == code) return map_raw[i];
            return fail(Error::InvalidValue);
        }
        return static_cast<std::uint8_t>(code ^ xor_mask);
    }
};

/// Unescape `buf` in place. Returns the new (shorter or equal) length.
constexpr Result<std::size_t> unescape_in_place(mut_bytes buf, const EscapeConfig& cfg) noexcept {
    std::size_t w = 0;
    for (std::size_t r = 0; r < buf.size(); ++r) {
        std::uint8_t b = buf[r];
        if (b == cfg.esc) {
            if (++r >= buf.size()) return fail(Error::Truncated);
            auto d = cfg.decode(buf[r]);
            if (!d) return fail(d.error());
            b = *d;
        }
        buf[w++] = b;
    }
    return w;
}

/// Escape the `len` bytes starting at buf[at] in place (expanding towards the end of `buf`).
/// Returns the escaped length.
constexpr Result<std::size_t> escape_in_place(mut_bytes buf, std::size_t at, std::size_t len,
                                              const EscapeConfig& cfg) noexcept {
    std::size_t extra = 0;
    for (std::size_t i = at; i < at + len; ++i)
        if (cfg.needs_escape(buf[i])) ++extra;
    if (at + len + extra > buf.size()) return fail(Error::BufferFull);
    // Walk backwards so we never overwrite unread input.
    std::size_t w = at + len + extra;
    for (std::size_t r = at + len; r-- > at;) {
        const std::uint8_t b = buf[r];
        if (cfg.needs_escape(b)) {
            buf[--w] = cfg.encode(b);
            buf[--w] = cfg.esc;
        } else {
            buf[--w] = b;
        }
    }
    return len + extra;
}

} // namespace gmp
