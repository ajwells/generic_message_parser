// Cross-check of the C framing runtime (crosscheck_framing_c.c, compiled as C11) against the C++ runtime:
// every scanner, the escape functions and the stream parser give the same results on structured random input.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "crosscheck_c.h"
#include "gmp/framing/delimited.hpp"
#include "gmp/framing/fixed.hpp"
#include "gmp/framing/length_prefixed.hpp"
#include "gmp/stream_parser.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

// Same configs as crosscheck_framing_c.c.
const gmp::EscapeConfig& escape_cfg(int which) {
    static const gmp::EscapeConfig xor_cfg = gmp::make_xor_escape(0x7D, 0x20, {0x7E});
    static const gmp::EscapeConfig index_cfg = gmp::make_index_escape(0x81, {0x80, 0x81});
    static const gmp::EscapeConfig map_cfg = gmp::make_map_escape(0xDB, {0xC0, 0xDB}, {0xDC, 0xDD});
    return which == 0 ? xor_cfg : which == 1 ? index_cfg : map_cfg;
}

gmp::FrameScan cpp_scan(int which, gmp::bytes_view b) {
    const gmp::Pattern aa55{{0xAA, 0x55}, 2}, flag{{0x7E}, 1}, dollar{{0x24}, 1}, crlf{{0x0D, 0x0A}, 2}, x80{{0x80}, 1};
    switch (which) {
        case 0: return gmp::scan_fixed(b, gmp::FixedFrameConfig{aa55, 6});
        case 1: return gmp::scan_fixed(b, gmp::FixedFrameConfig{gmp::Pattern{}, 4});
        case 2: return gmp::scan_fixed(b, gmp::FixedFrameConfig{x80, 5, escape_cfg(1), 0});
        case 3: return gmp::scan_length_prefixed(b, gmp::LengthPrefixedConfig{aa55, 2, 1, gmp::Endian::big, 5, 5, 24});
        case 4: return gmp::scan_length_prefixed(b, gmp::LengthPrefixedConfig{gmp::Pattern{}, 0, 2, gmp::Endian::little, -1, 3, 20});
        case 5: return gmp::scan_length_prefixed(b, gmp::LengthPrefixedConfig{flag, 1, 1, gmp::Endian::big, 4, 4, 32, escape_cfg(0), 0});
        case 6:
            return gmp::scan_length_prefixed(
                b, gmp::LengthPrefixedConfig{flag, 1, 1, gmp::Endian::big, 0, 4, 32, escape_cfg(0), 0, true, 2, 1, false});
        case 7: return gmp::scan_delimited(b, gmp::DelimitedConfig{flag, flag, escape_cfg(0), 24});
        default: return gmp::scan_delimited(b, gmp::DelimitedConfig{dollar, crlf, gmp::EscapeConfig{}, 16});
    }
}

/// Random bytes biased towards the bytes the framers care about, so frames and near-frames appear often.
/// Whole sync words and delimiters are inserted as tokens, so multi-byte patterns (AA 55, CR LF) occur often.
Bytes structured(std::mt19937& rng, std::size_t n) {
    static const std::uint8_t special[] = {0xAA, 0x55, 0x7E, 0x7D, 0x80, 0x81, 0xC0, 0xDB, 0xDC, 0xDD, 0x24, 0x0D, 0x0A, 0x5E, 0x5D};
    static const std::vector<Bytes> tokens = {{0xAA, 0x55}, {0x0D, 0x0A}, {0x24}, {0x7E}, {0x7D, 0x5E}, {0x80}, {0x81, 0x01}};
    Bytes b;
    while (b.size() < n) {
        const auto r = rng() % 20;
        if (r < 4) {
            const auto& t = tokens[rng() % tokens.size()];
            b.insert(b.end(), t.begin(), t.end());
        } else if (r < 10) {
            b.push_back(static_cast<std::uint8_t>(rng() % 8));
        } else if (r < 16) {
            b.push_back(special[rng() % sizeof special]);
        } else {
            b.push_back(static_cast<std::uint8_t>(rng()));
        }
    }
    b.resize(n);
    return b;
}

// --- stream: a test protocol over the same scanners -------------------------------------------------------
int g_cfg = 0;

struct Event {
    std::size_t kind, code, size;
    bool operator==(const Event&) const = default;
};

struct Recorder {
    std::vector<Event>* events;
    void operator()(gmp::bytes_view frame) const { events->push_back({0, 0, frame.size()}); }
    void operator()(const gmp::FrameError& e) const {
        events->push_back({1, static_cast<std::size_t>(e.error), e.data.size()});
    }
};

struct TestProtocol {
    static gmp::FrameScan scan(gmp::bytes_view b) noexcept { return cpp_scan(g_cfg, b); }
    template <class H>
    static gmp::Error process(gmp::mut_bytes frame, H& h) {
        if (frame.empty() || (frame.back() & 1u) != 0) return gmp::Error::BadChecksum;
        h(gmp::bytes_view(frame));
        return gmp::Error::None;
    }
};

template <std::size_t Cap>
void stream_case(int which, const Bytes& data, std::size_t chunk) {
    g_cfg = which;
    std::vector<Event> expected;
    gmp::StreamParser<TestProtocol, Cap> p;
    for (std::size_t pos = 0; pos < data.size(); pos += chunk)
        (void)p.feed(gmp::bytes_view(data.data() + pos, std::min(chunk, data.size() - pos)), Recorder{&expected});

    std::vector<std::size_t> ev(3 * 4096);
    std::array<std::uint32_t, 4> st{};
    const std::size_t n = gmpct_stream(which, Cap, data.data(), data.size(), chunk, ev.data(), 4096, st.data());
    REQUIRE(n == expected.size());
    for (std::size_t i = 0; i < n; ++i) {
        INFO("event " << i);
        CHECK(Event{ev[3 * i], ev[3 * i + 1], ev[3 * i + 2]} == expected[i]);
    }
    CHECK(st[0] == p.stats().frames);
    CHECK(st[1] == p.stats().frame_errors);
    CHECK(st[2] == p.stats().bytes_skipped);
    CHECK(st[3] == p.stats().overflows);
}

} // namespace

TEST_CASE("C framing scanners match the C++ runtime") {
    std::mt19937 rng(2024);
    std::array<int, GMPCT_SCAN_COUNT> frames{};
    for (int round = 0; round < 3000; ++round) {
        const auto b = structured(rng, rng() % 40);
        for (int which = 0; which < GMPCT_SCAN_COUNT; ++which) {
            INFO("config " << which << ", round " << round);
            const auto s = cpp_scan(which, gmp::bytes_view(b.data(), b.size()));
            std::size_t out[5]{};
            gmpct_scan(which, b.data(), b.size(), out);
            CHECK(out[0] == static_cast<std::size_t>(s.kind));
            CHECK(out[1] == s.size);
            CHECK(out[2] == s.consume);
            CHECK(out[3] == s.consume_on_error);
            CHECK(out[4] == static_cast<std::size_t>(s.error));
            if (s.kind == gmp::FrameScan::Kind::frame) ++frames[static_cast<std::size_t>(which)];
        }
    }
    for (int which = 0; which < GMPCT_SCAN_COUNT; ++which) {
        INFO("config " << which << " found " << frames[static_cast<std::size_t>(which)] << " frames");
        CHECK(frames[static_cast<std::size_t>(which)] > 10);  // the input exercises complete frames, not only skips
    }
}

TEST_CASE("C escaping matches the C++ runtime") {
    std::mt19937 rng(77);
    for (int round = 0; round < 2000; ++round) {
        const auto b = structured(rng, rng() % 24);
        for (int which = 0; which < GMPCT_ESCAPE_COUNT; ++which) {
            INFO("config " << which << ", round " << round);
            const auto& cfg = escape_cfg(which);
            // unescape in place
            auto x = b, y = b;
            const auto r = gmp::unescape_in_place(gmp::mut_bytes(x.data(), x.size()), cfg);
            std::size_t n = 0;
            const int e = gmpct_unescape(which, y.data(), y.size(), &n);
            REQUIRE(e == (r ? 0 : static_cast<int>(r.error())));
            if (r) {
                REQUIRE(n == *r);
                CHECK(std::equal(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n), y.begin()));
            }
            // escape in place (room for every byte to double, and too little room)
            for (const std::size_t extra : {b.size(), std::size_t{1}}) {
                Bytes ex(b.size() + 2 + extra, 0xEE), ey(b.size() + 2 + extra, 0xEE);
                std::copy(b.begin(), b.end(), ex.begin() + 2);
                std::copy(b.begin(), b.end(), ey.begin() + 2);
                const auto er = gmp::escape_in_place(gmp::mut_bytes(ex.data(), ex.size()), 2, b.size(), cfg);
                std::size_t en = 0;
                const int ee = gmpct_escape(which, ey.data(), ey.size(), 2, b.size(), &en);
                REQUIRE(ee == (er ? 0 : static_cast<int>(er.error())));
                if (er) {
                    REQUIRE(en == *er);
                    CHECK(ex == ey);
                }
            }
            // walk with a pick window
            const std::size_t count = rng() % 12, pick_at = rng() % 4;
            std::array<std::uint8_t, 4> px{}, py{};
            const auto w = gmp::walk_escaped(gmp::bytes_view(b.data(), b.size()), 0, count, cfg, pick_at,
                                             gmp::mut_bytes(px.data(), px.size()));
            std::size_t wo[3]{};
            gmpct_walk(which, b.data(), b.size(), 0, count, pick_at, py.data(), py.size(), wo);
            CHECK(wo[0] == static_cast<std::size_t>(w.kind));
            CHECK(wo[1] == w.wire);
            CHECK(wo[2] == static_cast<std::size_t>(w.error));
            if (w.kind == gmp::EscapeWalk::Kind::done) CHECK(px == py);
        }
    }
}

TEST_CASE("C stream matches the C++ StreamParser (events, resync, statistics)") {
    std::mt19937 rng(9);
    for (int round = 0; round < 150; ++round) {
        const auto data = structured(rng, 50 + rng() % 400);
        for (int which = 0; which < GMPCT_SCAN_COUNT; ++which) {
            for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, data.size()}) {
                INFO("config " << which << ", chunk " << chunk << ", round " << round);
                stream_case<32>(which, data, chunk);
                stream_case<256>(which, data, chunk);
            }
        }
    }
}
