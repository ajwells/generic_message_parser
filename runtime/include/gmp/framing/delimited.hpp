#pragma once
// gmp/framing/delimited.hpp - frames bounded by start and/or end delimiters (binary or text).

#include "gmp/framing/escape.hpp"
#include "gmp/framing/frame.hpp"

namespace gmsg {

struct DelimitedConfig {
    Pattern start{};       // optional (len 0 = frame begins at the stream position)
    Pattern end{};         // required
    EscapeConfig escape{}; // optional byte stuffing; escaped bytes are skipped while searching for `end`
    std::size_t max_frame = 0;
};

constexpr FrameScan scan_delimited(bytes_view buf, const DelimitedConfig& cfg) noexcept {
    bool matched = false;
    auto r = detail::find_pattern_at_start(buf, cfg.start, matched);
    if (!matched) return r;

    const bool shared_flag = cfg.start.len > 0 && cfg.start.len == cfg.end.len &&
                             detail::matches_at(cfg.end.view(), 0, cfg.start);
    const std::size_t s = cfg.start.len;

    for (std::size_t i = s; i < buf.size(); ++i) {
        if (i + 1 > cfg.max_frame) {
            // No end delimiter within max_frame bytes.
            return FrameScan::skip(s ? s : i, Error::BadLength);
        }
        if (cfg.escape.enabled && buf[i] == cfg.escape.esc) { ++i; continue; }
        if (detail::matches_at(buf, i, cfg.end)) {
            const std::size_t n = i + cfg.end.len;
            if (shared_flag) {
                // HDLC style: the closing flag may also open the next frame.
                if (i == s) return FrameScan::skip(s);   // back-to-back flags: drop one
                return FrameScan::frame(n, n - cfg.end.len, n - cfg.end.len);
            }
            return FrameScan::frame(n, n, n);
        }
        if (!shared_flag && s && detail::matches_at(buf, i, cfg.start)) {
            // A new start delimiter before the end: the previous frame was truncated.
            return FrameScan::skip(i, Error::BadDelimiter);
        }
    }
    if (buf.size() >= cfg.max_frame) return FrameScan::skip(s ? s : buf.size(), Error::BadLength);
    return FrameScan::need_more();
}

} // namespace gmsg
