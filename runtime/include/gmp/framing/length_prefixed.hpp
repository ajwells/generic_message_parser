#pragma once
// gmp/framing/length_prefixed.hpp - frames whose total size is derived from a length field.
//
// total_frame_size = length_value + add      (the generator folds header/trailer sizes into `add`)
//
// With escaping, everything between the sync pattern and the suffix is byte-stuffed. The length
// field is read through the escaping. By default it counts unescaped bytes and the end of the frame
// on the wire is found by walking the escaped bytes. With `counts_wire` it gives the size of the
// body on the wire (after escaping): body wire size = value + add, and the header and trailer
// (header_len / trailer_len unescaped bytes) are walked through the escaping around it.

#include <cstdint>

#include "gmp/endian.hpp"
#include "gmp/framing/escape.hpp"
#include "gmp/framing/frame.hpp"

namespace gmp {

struct LengthPrefixedConfig {
    Pattern sync{};
    std::size_t length_offset = 0;  // byte offset of the length field from frame start (unescaped)
    std::uint8_t length_size = 1;   // 1..8 bytes
    Endian length_endian = Endian::big;
    std::int64_t add = 0;           // total = value + add
    std::size_t min_frame = 0;
    std::size_t max_frame = 0;
    EscapeConfig escape{};          // optional byte stuffing of everything between sync and suffix
    std::size_t suffix_len = 0;     // trailing bytes that are not escaped (end pattern)
    bool counts_wire = false;       // escape: the length is the body size on the wire
    std::size_t header_len = 0;     // counts_wire: unescaped header size
    std::size_t trailer_len = 0;    // counts_wire: unescaped trailer (checksum) size
    bool wire_includes_trailer = false;  // counts_wire: the value also counts the escaped trailer
};

/// Copy the `out.size()` header bytes that follow the sync pattern at the start of `buf` (unescaped when the frame is
/// escaped) into `out`. False while they have not all arrived, or when the escaping is invalid (the scanner reports
/// that). Used by generated scanners to check the length against the message type before the body arrives.
constexpr bool read_frame_header(bytes_view buf, std::size_t prefix, const EscapeConfig& escape, mut_bytes out) noexcept {
    if (!escape.enabled) {
        if (buf.size() < prefix + out.size()) return false;
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = buf[prefix + i];
        return true;
    }
    return walk_escaped(buf, prefix, out.size(), escape, 0, out).kind == EscapeWalk::Kind::done;
}

namespace detail {

/// counts_wire: the body is a fixed number of wire bytes between the escaped header and trailer.
constexpr FrameScan scan_length_prefixed_wire(bytes_view buf, const LengthPrefixedConfig& cfg) noexcept {
    const std::size_t s = cfg.sync.len;
    std::array<std::uint8_t, 8> lb{};
    const auto hw = walk_escaped(buf, s, cfg.header_len, cfg.escape, cfg.length_offset - s,
                                 mut_bytes(lb.data(), cfg.length_size));
    if (hw.kind == EscapeWalk::Kind::need_more) return FrameScan::need_more();
    if (hw.kind == EscapeWalk::Kind::bad) return FrameScan::skip(hw.wire > 0 ? hw.wire : 1, hw.error);
    const auto value = load_uint(lb.data(), cfg.length_size, cfg.length_endian);
    const std::int64_t body = static_cast<std::int64_t>(value) + cfg.add;
    if (body < 0 || hw.wire + static_cast<std::uint64_t>(body) + cfg.suffix_len > cfg.max_frame)
        return FrameScan::skip(1, Error::BadLength);
    const std::size_t body_end = hw.wire + static_cast<std::size_t>(body);
    // the counted bytes must be complete escape pairs with no unescaped reserved byte
    for (std::size_t i = hw.wire; i < body_end; ++i) {
        if (i >= buf.size()) return FrameScan::need_more(body_end);
        if (buf[i] == cfg.escape.esc) {
            if (i + 1 >= body_end) return FrameScan::skip(1, Error::BadLength);  // pair split by the length
            ++i;
        } else if (cfg.escape.needs_escape(buf[i])) {
            return FrameScan::skip(i, Error::BadDelimiter);
        }
    }
    std::size_t end = body_end;  // wire_includes_trailer: the value already counted the escaped trailer
    if (!cfg.wire_includes_trailer) {
        const auto tw = walk_escaped(buf, body_end, cfg.trailer_len, cfg.escape);
        if (tw.kind == EscapeWalk::Kind::need_more) return FrameScan::need_more();
        if (tw.kind == EscapeWalk::Kind::bad) return FrameScan::skip(tw.wire, tw.error);
        end = tw.wire;
    }
    const std::size_t n = end + cfg.suffix_len;
    if (n < cfg.min_frame || n > cfg.max_frame) return FrameScan::skip(1, Error::BadLength);
    if (buf.size() < n) return FrameScan::need_more(n);
    return FrameScan::frame(n, n, 1);
}

} // namespace detail

constexpr FrameScan scan_length_prefixed(bytes_view buf, const LengthPrefixedConfig& cfg) noexcept {
    bool matched = false;
    auto r = detail::find_pattern_at_start(buf, cfg.sync, matched);
    if (!matched) return r;
    if (cfg.escape.enabled && cfg.counts_wire) return detail::scan_length_prefixed_wire(buf, cfg);
    const std::size_t need = cfg.length_offset + cfg.length_size;
    std::uint64_t value = 0;
    if (!cfg.escape.enabled) {
        if (buf.size() < need) return FrameScan::need_more(need);
        value = load_uint(buf.data() + cfg.length_offset, cfg.length_size, cfg.length_endian);
    } else {
        std::array<std::uint8_t, 8> lb{};
        const std::size_t s = cfg.sync.len;
        const auto wk = walk_escaped(buf, s, need - s, cfg.escape, cfg.length_offset - s,
                                     mut_bytes(lb.data(), cfg.length_size));
        if (wk.kind == EscapeWalk::Kind::need_more) return FrameScan::need_more();
        if (wk.kind == EscapeWalk::Kind::bad) return FrameScan::skip(wk.wire > 0 ? wk.wire : 1, wk.error);
        value = load_uint(lb.data(), cfg.length_size, cfg.length_endian);
    }
    const std::int64_t total = static_cast<std::int64_t>(value) + cfg.add;
    if (total < static_cast<std::int64_t>(cfg.min_frame) || total > static_cast<std::int64_t>(cfg.max_frame) ||
        total <= 0)
        return FrameScan::skip(1, Error::BadLength);
    auto n = static_cast<std::size_t>(total);
    if (cfg.escape.enabled) {
        const std::size_t s = cfg.sync.len;
        if (n < s + cfg.suffix_len) return FrameScan::skip(1, Error::BadLength);
        const auto wk = walk_escaped(buf, s, n - s - cfg.suffix_len, cfg.escape);
        if (wk.kind == EscapeWalk::Kind::need_more) {
            if (wk.wire > cfg.max_frame) return FrameScan::skip(1, Error::BadLength);
            return FrameScan::need_more();
        }
        if (wk.kind == EscapeWalk::Kind::bad) return FrameScan::skip(wk.wire > 0 ? wk.wire : 1, wk.error);
        n = wk.wire + cfg.suffix_len;
        if (n > cfg.max_frame) return FrameScan::skip(1, Error::BadLength);
    }
    if (buf.size() < n) return FrameScan::need_more(n);
    return FrameScan::frame(n, n, 1);
}

} // namespace gmp
