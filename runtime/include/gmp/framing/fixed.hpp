#pragma once
// gmp/framing/fixed.hpp - fixed-size frames with optional sync pattern.
//
// With escaping, `frame_size` is the unescaped size; the bytes between the sync pattern and the
// (unescaped) suffix are walked through the escaping to find the end of the frame on the wire.

#include "gmp/framing/escape.hpp"
#include "gmp/framing/frame.hpp"

namespace gmp {

struct FixedFrameConfig {
    Pattern sync{};
    std::size_t frame_size = 0;
    EscapeConfig escape{};       // optional byte stuffing of everything between sync and suffix
    std::size_t suffix_len = 0;  // trailing bytes that are not escaped (end pattern)
};

constexpr FrameScan scan_fixed(bytes_view buf, const FixedFrameConfig& cfg) noexcept {
    bool matched = false;
    auto r = detail::find_pattern_at_start(buf, cfg.sync, matched);
    if (!matched) return r;
    // Without a sync word there is no way to resync: drop the whole frame on error.
    if (!cfg.escape.enabled) {
        if (buf.size() < cfg.frame_size) return FrameScan::need_more(cfg.frame_size);
        const std::size_t on_err = cfg.sync.len ? 1 : cfg.frame_size;
        return FrameScan::frame(cfg.frame_size, cfg.frame_size, on_err);
    }
    const std::size_t s = cfg.sync.len;
    const auto wk = walk_escaped(buf, s, cfg.frame_size - s - cfg.suffix_len, cfg.escape);
    if (wk.kind == EscapeWalk::Kind::need_more) return FrameScan::need_more();
    if (wk.kind == EscapeWalk::Kind::bad) return FrameScan::skip(wk.wire > 0 ? wk.wire : 1, wk.error);
    const std::size_t n = wk.wire + cfg.suffix_len;
    if (buf.size() < n) return FrameScan::need_more(n);
    return FrameScan::frame(n, n, s ? 1 : n);
}

} // namespace gmp
