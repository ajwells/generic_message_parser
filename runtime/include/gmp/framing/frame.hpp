#pragma once
// gmp/framing/frame.hpp - common types for framers.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "gmp/result.hpp"

namespace gmp {

/// Result of scanning a buffer that starts at the current stream position.
struct FrameScan {
    enum class Kind : std::uint8_t { need_more, frame, skip };

    Kind kind = Kind::need_more;
    std::size_t size = 0;      // frame: frame length; skip: bytes to discard; need_more: hint (0 = unknown)
    std::size_t consume = 0;   // frame: bytes to consume after a successfully processed frame
    std::size_t consume_on_error = 0;  // frame: bytes to consume when frame validation fails (resync)
    Error error = Error::None; // skip: reason (None = silent skip)

    static constexpr FrameScan need_more(std::size_t hint = 0) noexcept { return {Kind::need_more, hint, 0, 0, Error::None}; }
    static constexpr FrameScan frame(std::size_t n, std::size_t consume, std::size_t consume_on_error) noexcept {
        return {Kind::frame, n, consume, consume_on_error, Error::None};
    }
    static constexpr FrameScan skip(std::size_t n, Error e = Error::None) noexcept { return {Kind::skip, n, 0, 0, e}; }
};

/// Fixed byte pattern (sync word or delimiter), up to 8 bytes.
struct Pattern {
    std::array<std::uint8_t, 8> bytes{};
    std::uint8_t len = 0;

    [[nodiscard]] constexpr bytes_view view() const noexcept { return {bytes.data(), len}; }
};

namespace detail {

/// Locate `pat` at the start of `buf`.
///  - returns nullopt-equivalent (kind==frame, size==0) when matched at offset 0,
///  - need_more when buf is a strict prefix of the pattern,
///  - skip(n) to the first possible match otherwise.
constexpr FrameScan find_pattern_at_start(bytes_view buf, const Pattern& pat, bool& matched) noexcept {
    matched = false;
    if (pat.len == 0) { matched = true; return {}; }
    for (std::size_t i = 0; i < buf.size(); ++i) {
        const std::size_t avail = std::min<std::size_t>(pat.len, buf.size() - i);
        bool ok = true;
        for (std::size_t k = 0; k < avail; ++k) {
            if (buf[i + k] != pat.bytes[k]) { ok = false; break; }
        }
        if (!ok) continue;
        if (i > 0) return FrameScan::skip(i, Error::BadSync);
        if (avail < pat.len) return FrameScan::need_more(pat.len);
        matched = true;
        return {};
    }
    // No candidate anywhere: discard everything.
    return buf.empty() ? FrameScan::need_more(pat.len) : FrameScan::skip(buf.size(), Error::BadSync);
}

constexpr bool matches_at(bytes_view buf, std::size_t at, const Pattern& pat) noexcept {
    if (at + pat.len > buf.size()) return false;
    for (std::size_t k = 0; k < pat.len; ++k)
        if (buf[at + k] != pat.bytes[k]) return false;
    return true;
}

} // namespace detail

} // namespace gmp
