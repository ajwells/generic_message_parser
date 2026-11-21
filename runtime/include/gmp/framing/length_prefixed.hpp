#pragma once
// gmp/framing/length_prefixed.hpp - frames whose total size is derived from a length field.
//
// total_frame_size = length_value + add      (the generator folds header/trailer sizes into `add`)

#include <cstdint>

#include "gmp/endian.hpp"
#include "gmp/framing/frame.hpp"

namespace gmp {

struct LengthPrefixedConfig {
    Pattern sync{};
    std::size_t length_offset = 0;  // byte offset of the length field from frame start
    std::uint8_t length_size = 1;   // 1..8 bytes
    Endian length_endian = Endian::big;
    std::int64_t add = 0;           // total = value + add
    std::size_t min_frame = 0;
    std::size_t max_frame = 0;
};

constexpr FrameScan scan_length_prefixed(bytes_view buf, const LengthPrefixedConfig& cfg) noexcept {
    bool matched = false;
    auto r = detail::find_pattern_at_start(buf, cfg.sync, matched);
    if (!matched) return r;
    const std::size_t need = cfg.length_offset + cfg.length_size;
    if (buf.size() < need) return FrameScan::need_more(need);
    const auto value = load_uint(buf.data() + cfg.length_offset, cfg.length_size, cfg.length_endian);
    const std::int64_t total = static_cast<std::int64_t>(value) + cfg.add;
    if (total < static_cast<std::int64_t>(cfg.min_frame) || total > static_cast<std::int64_t>(cfg.max_frame) ||
        total <= 0)
        return FrameScan::skip(1, Error::BadLength);
    const auto n = static_cast<std::size_t>(total);
    if (buf.size() < n) return FrameScan::need_more(n);
    return FrameScan::frame(n, n, 1);
}

} // namespace gmp
