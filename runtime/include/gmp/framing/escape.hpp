#pragma once
// gmp/framing/escape.hpp - byte stuffing ("reserved byte" replacement) for frames.
//
// A set of reserved bytes (sync / delimiter bytes, plus the escape byte itself) must never appear
// inside the escaped part of a frame. Each reserved byte b is sent as the pair {esc, code(b)}:
//  - xor style (HDLC/PPP): code = b ^ xor_mask; any code is accepted on decode.
//  - table style:          code comes from an explicit table. This covers SLIP-like maps and the
//                          "index" scheme, where code is the byte's position in the reserved list
//                          (plus an optional base), e.g. reserved {0x80, 0x81}: 0x80 -> 81 00, 0x81 -> 81 01.
//
// Up to `EscapeConfig::max_reserved` reserved bytes are supported; membership is an O(1) bitmask test.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "gmp/result.hpp"

namespace gmp {

enum class EscapeStyle : std::uint8_t { xor_mask, table };

struct EscapeConfig {
    static constexpr std::size_t max_reserved = 16;

    bool enabled = false;
    std::uint8_t esc = 0;
    EscapeStyle style = EscapeStyle::xor_mask;
    std::uint8_t xor_mask = 0x20;
    std::uint8_t count = 0;                          // entries used in raw / code
    std::array<std::uint8_t, max_reserved> raw{};    // reserved bytes
    std::array<std::uint8_t, max_reserved> code{};   // table style: code sent after esc
    std::array<std::uint32_t, 8> mask{};             // 256-bit set: raw bytes + esc

    constexpr void reserve(std::uint8_t b) noexcept { mask[b >> 5] |= std::uint32_t{1} << (b & 31u); }

    /// True for bytes that must not appear unescaped (the reserved bytes and the escape byte).
    [[nodiscard]] constexpr bool needs_escape(std::uint8_t b) const noexcept {
        return ((mask[b >> 5] >> (b & 31u)) & 1u) != 0;
    }
    [[nodiscard]] constexpr std::uint8_t encode(std::uint8_t b) const noexcept {
        if (style == EscapeStyle::table) {
            for (std::size_t i = 0; i < count; ++i)
                if (raw[i] == b) return code[i];
        }
        return static_cast<std::uint8_t>(b ^ xor_mask);
    }
    [[nodiscard]] constexpr Result<std::uint8_t> decode(std::uint8_t c) const noexcept {
        if (style == EscapeStyle::table) {
            for (std::size_t i = 0; i < count; ++i)
                if (code[i] == c) return raw[i];
            return fail(Error::InvalidValue);
        }
        return static_cast<std::uint8_t>(c ^ xor_mask);
    }
};

/// XOR style: every byte in `reserved` (and `esc`) is sent as {esc, b ^ xor_mask}.
constexpr EscapeConfig make_xor_escape(std::uint8_t esc, std::uint8_t xor_mask,
                                       std::initializer_list<std::uint8_t> reserved) noexcept {
    EscapeConfig c{};
    c.enabled = true;
    c.esc = esc;
    c.style = EscapeStyle::xor_mask;
    c.xor_mask = xor_mask;
    c.reserve(esc);
    for (auto b : reserved) {
        if (c.count < EscapeConfig::max_reserved) c.raw[c.count++] = b;
        c.reserve(b);
    }
    return c;
}

/// Table style: raw[i] is sent as {esc, code[i]}. `raw` should include `esc`.
constexpr EscapeConfig make_map_escape(std::uint8_t esc, std::initializer_list<std::uint8_t> raw,
                                       std::initializer_list<std::uint8_t> code) noexcept {
    EscapeConfig c{};
    c.enabled = true;
    c.esc = esc;
    c.style = EscapeStyle::table;
    c.reserve(esc);
    for (auto b : raw) {
        if (c.count < EscapeConfig::max_reserved) c.raw[c.count++] = b;
        c.reserve(b);
    }
    std::size_t i = 0;
    for (auto b : code)
        if (i < EscapeConfig::max_reserved) c.code[i++] = b;
    return c;
}

/// Index style: reserved[i] is sent as {esc, base + i}. `reserved` should include `esc`.
constexpr EscapeConfig make_index_escape(std::uint8_t esc, std::initializer_list<std::uint8_t> reserved,
                                         std::uint8_t base = 0) noexcept {
    EscapeConfig c{};
    c.enabled = true;
    c.esc = esc;
    c.style = EscapeStyle::table;
    c.reserve(esc);
    for (auto b : reserved) {
        if (c.count < EscapeConfig::max_reserved) {
            c.code[c.count] = static_cast<std::uint8_t>(base + c.count);
            c.raw[c.count++] = b;
        }
        c.reserve(b);
    }
    return c;
}

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

/// Number of bytes produced by unescaping `wire`.
constexpr Result<std::size_t> unescaped_size(bytes_view wire, const EscapeConfig& cfg) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < wire.size(); ++i, ++n) {
        if (wire[i] == cfg.esc && ++i >= wire.size()) return fail(Error::Truncated);
    }
    return n;
}

/// Check the escape pairs of `wire` without modifying it: the same errors as unescape_in_place, in the same order.
/// Returns the unescaped size.
constexpr Result<std::size_t> check_escaped(bytes_view wire, const EscapeConfig& cfg) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < wire.size(); ++i, ++n) {
        if (wire[i] != cfg.esc) continue;
        if (++i >= wire.size()) return fail(Error::Truncated);
        if (auto d = cfg.decode(wire[i]); !d) return fail(d.error());
    }
    return n;
}

/// Pass the unescaped bytes with index in [from, to) of `wire` to fn(bytes_view), without modifying `wire`: runs of
/// plain bytes as views into `wire`, each escape pair as its decoded byte. `wire` must pass check_escaped().
/// Used to verify a checksum before a frame is unescaped in place.
template <class F>
constexpr void for_each_unescaped(bytes_view wire, const EscapeConfig& cfg, std::size_t from, std::size_t to,
                                  F&& fn) {
    std::size_t u = 0, run = 0, run_n = 0;
    for (std::size_t i = 0; i < wire.size() && u < to; ++u) {
        if (wire[i] == cfg.esc && i + 1 < wire.size()) {
            if (run_n) fn(wire.subspan(run, run_n));
            run_n = 0;
            if (u >= from) {
                const std::uint8_t b = cfg.decode(wire[i + 1]).value_or(0);
                fn(bytes_view(&b, 1));
            }
            i += 2;
        } else {
            if (u >= from && run_n++ == 0) run = i;
            ++i;
        }
    }
    if (run_n) fn(wire.subspan(run, run_n));
}

/// Number of escape pairs needed for `raw`.
constexpr std::size_t escape_overhead(bytes_view raw, const EscapeConfig& cfg) noexcept {
    std::size_t n = 0;
    for (auto b : raw) n += cfg.needs_escape(b) ? 1 : 0;
    return n;
}

/// Escape the `len` bytes starting at buf[at] in place (expanding towards the end of `buf`).
/// Returns the escaped length.
constexpr Result<std::size_t> escape_in_place(mut_bytes buf, std::size_t at, std::size_t len,
                                              const EscapeConfig& cfg) noexcept {
    const std::size_t extra = escape_overhead(bytes_view(buf.data() + at, len), cfg);
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

/// Result of walking an escaped byte stream.
struct EscapeWalk {
    enum class Kind : std::uint8_t { done, need_more, bad };
    Kind kind = Kind::done;
    std::size_t wire = 0;       // done: wire position just after the n-th unescaped byte; bad: offending position
    Error error = Error::None;  // bad: reason
};

/// Walk the escaped bytes of `buf` starting at `at` until `n` unescaped bytes have been produced.
/// Unescaped bytes with index in [pick_at, pick_at + pick.size()) are copied into `pick`.
/// A reserved byte that appears unescaped (e.g. the sync byte of the next frame) is reported as `bad`.
constexpr EscapeWalk walk_escaped(bytes_view buf, std::size_t at, std::size_t n, const EscapeConfig& cfg,
                                  std::size_t pick_at = 0, mut_bytes pick = {}) noexcept {
    std::size_t i = at;
    for (std::size_t u = 0; u < n; ++u) {
        if (i >= buf.size()) return {EscapeWalk::Kind::need_more, i, Error::None};
        std::uint8_t b = buf[i];
        if (b == cfg.esc) {
            if (i + 1 >= buf.size()) return {EscapeWalk::Kind::need_more, i, Error::None};
            auto d = cfg.decode(buf[i + 1]);
            if (!d) return {EscapeWalk::Kind::bad, i, Error::InvalidValue};
            b = *d;
            i += 2;
        } else if (cfg.needs_escape(b)) {
            return {EscapeWalk::Kind::bad, i, Error::BadDelimiter};
        } else {
            ++i;
        }
        if (u >= pick_at && u - pick_at < pick.size()) pick[u - pick_at] = b;
    }
    return {EscapeWalk::Kind::done, i, Error::None};
}

} // namespace gmp
