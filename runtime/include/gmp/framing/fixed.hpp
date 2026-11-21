#pragma once
// gmp/framing/fixed.hpp - fixed-size frames with optional sync pattern.

#include "gmp/framing/frame.hpp"

namespace gmp {

struct FixedFrameConfig {
    Pattern sync{};
    std::size_t frame_size = 0;
};

constexpr FrameScan scan_fixed(bytes_view buf, const FixedFrameConfig& cfg) noexcept {
    bool matched = false;
    auto r = detail::find_pattern_at_start(buf, cfg.sync, matched);
    if (!matched) return r;
    if (buf.size() < cfg.frame_size) return FrameScan::need_more(cfg.frame_size);
    // Without a sync word there is no way to resync: drop the whole frame on error.
    const std::size_t on_err = cfg.sync.len ? 1 : cfg.frame_size;
    return FrameScan::frame(cfg.frame_size, cfg.frame_size, on_err);
}

} // namespace gmp
