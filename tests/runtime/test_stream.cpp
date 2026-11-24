#include <catch2/catch_test_macros.hpp>

#include <random>
#include <vector>

#include "gmp/checksum.hpp"
#include "gmp/framing/length_prefixed.hpp"
#include "gmp/stream_parser.hpp"

using namespace gmsg;

namespace {

// Hand written protocol: [AA][len u8][payload...][crc8 over len+payload]
struct TestProto {
    static constexpr LengthPrefixedConfig cfg{Pattern{{0xAA}, 1}, 1, 1, Endian::big, 3, 3, 32};
    static FrameScan scan(bytes_view b) { return scan_length_prefixed(b, cfg); }

    template <class H>
    static Error process(mut_bytes f, H& h) {
        const auto covered = bytes_view(f).subspan(1, f.size() - 2);
        if (checksum::crc8::compute(covered) != f.back()) return Error::BadChecksum;
        h(bytes_view(f).subspan(2, f.size() - 3));
        return Error::None;
    }
};

std::vector<std::uint8_t> make_frame(std::vector<std::uint8_t> payload) {
    std::vector<std::uint8_t> f{0xAA, static_cast<std::uint8_t>(payload.size())};
    f.insert(f.end(), payload.begin(), payload.end());
    f.push_back(checksum::crc8::compute(bytes_view(f).subspan(1)));
    return f;
}

struct Collector {
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<Error> errors;
    void operator()(bytes_view p) { frames.emplace_back(p.begin(), p.end()); }
    void operator()(const FrameError& e) { errors.push_back(e.error); }
};

} // namespace

TEST_CASE("stream parser handles arbitrary chunking, garbage and corruption") {
    std::vector<std::uint8_t> stream;
    std::vector<std::vector<std::uint8_t>> expected;
    std::mt19937 rng(1234);
    for (int i = 0; i < 200; ++i) {
        std::vector<std::uint8_t> payload(rng() % 10);
        for (auto& b : payload) b = static_cast<std::uint8_t>(rng());
        auto f = make_frame(payload);
        const int kind = static_cast<int>(rng() % 10);
        if (kind == 0) {
            // garbage without sync bytes
            stream.insert(stream.end(), {0x01, 0x02, 0x03});
        } else if (kind == 1) {
            // corrupted frame (flip a payload/crc byte)
            f.back() ^= 0xFF;
            stream.insert(stream.end(), f.begin(), f.end());
            continue;
        }
        expected.push_back(payload);
        stream.insert(stream.end(), f.begin(), f.end());
    }

    for (std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{64}, stream.size()}) {
        StreamParser<TestProto, 32> sp;
        Collector c;
        for (std::size_t off = 0; off < stream.size(); off += chunk) {
            const auto n = std::min(chunk, stream.size() - off);
            sp.feed(bytes_view(stream.data() + off, n), c);
        }
        INFO("chunk size " << chunk);
        // Corrupt frames may cause a few extra resyncs but every good frame must be recovered,
        // unless a payload byte happens to look like a valid frame - not the case with this seed.
        REQUIRE(c.frames == expected);
        CHECK(sp.stats().frames == expected.size());
        CHECK_FALSE(c.errors.empty());
    }
}

TEST_CASE("stream parser push/next pull mode") {
    StreamParser<TestProto, 32> sp;
    auto f = make_frame({1, 2, 3});
    CHECK(sp.push(bytes_view(f).first(3)) == 3);
    int got = 0;
    auto h = [&](bytes_view) { ++got; };
    CHECK_FALSE(sp.next(h));
    CHECK(sp.push(bytes_view(f).subspan(3)) == f.size() - 3);
    CHECK(sp.next(h));
    CHECK(got == 1);
    CHECK(sp.buffered() == 0);
}
