#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

#include "stuffed.gmp.hpp"

using namespace link::stuffed;

namespace {

Data make_data(std::size_t n, std::uint8_t seed) {
    Data d;
    for (std::size_t i = 0; i < n; ++i) {
        // plenty of reserved bytes (0x80 sync, 0x81 escape, 0x00 / 0x0D in some protocols)
        static constexpr std::uint8_t pool[] = {0x80, 0x81, 0x00, 0x0D, 0x7F, 0x82, 0x10, 0x11};
        (void)d.data_.push_back(i % 3 == 0 ? pool[(i + seed) % 8] : static_cast<std::uint8_t>(i * 37 + seed));
    }
    d.n = static_cast<std::uint8_t>(n);
    return d;
}

/// 0x80 may appear only as the sync byte at the start of each frame.
void check_no_reserved(gmsg::bytes_view wire) {
    REQUIRE(wire.size() > 0);
    CHECK(wire[0] == 0x80);
    for (std::size_t i = 1; i < wire.size(); ++i) CHECK(wire[i] != 0x80);
}

template <class P, class M>
void roundtrip(const M& m) {
    std::array<std::uint8_t, P::max_frame> buf{};
    auto n = P::encode(m, buf);
    REQUIRE(n);
    REQUIRE(*n <= P::max_frame);
    check_no_reserved(gmsg::bytes_view(buf.data(), *n));
    auto d = P::decode(gmsg::mut_bytes(buf.data(), *n));
    REQUIRE(d);
    REQUIRE(std::holds_alternative<M>(*d));
    CHECK(std::get<M>(*d) == m);
}

} // namespace

TEMPLATE_TEST_CASE("Stuffed protocols round-trip every payload size", "", SyncLink, WireChecksum, WireLength, CrcLength, WireCrcLength,
                   WireCrcOverWire, Delimited) {
    for (std::size_t n = 0; n <= 120; ++n) {
        CAPTURE(n);
        roundtrip<TestType>(make_data(n, static_cast<std::uint8_t>(n)));
    }
    for (std::uint32_t seq : {0u, 0x80808080u, 0x81818181u, 0x0D80810Du, 0x12345678u}) roundtrip<TestType>(Ping{seq});
}

TEST_CASE("Fixed framing with escaping round-trips") {
    for (std::uint32_t seq : {0u, 0x80808080u, 0x81008100u, 0x0D80810Du, 0x12345678u, 0xFFFFFFFFu}) {
        CAPTURE(seq);
        roundtrip<FixedLink>(Ping{seq});
    }
}

TEST_CASE("SyncLink wire format: escape byte + index byte") {
    std::array<std::uint8_t, SyncLink::max_frame> buf{};
    auto n = SyncLink::encode(Ping{0x80818283u}, buf);
    REQUIRE(n);
    // unescaped: 80 | 02 04 | 80 81 82 83 | crc16
    const std::array<std::uint8_t, 6> covered{0x02, 0x04, 0x80, 0x81, 0x82, 0x83};
    const auto crc = gmsg::checksum::crc16_ccitt::compute(covered);
    std::vector<std::uint8_t> expect{0x80, 0x02, 0x04, 0x81, 0x00, 0x81, 0x01, 0x82, 0x83};
    for (auto b : {static_cast<std::uint8_t>(crc >> 8), static_cast<std::uint8_t>(crc & 0xFF)}) {
        if (b == 0x80) expect.insert(expect.end(), {0x81, 0x00});
        else if (b == 0x81) expect.insert(expect.end(), {0x81, 0x01});
        else expect.push_back(b);
    }
    CHECK(std::vector<std::uint8_t>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)) == expect);

    // unknown index after the escape byte
    std::vector<std::uint8_t> bad = expect;
    bad[4] = 0x07;
    CHECK(SyncLink::parse(gmsg::mut_bytes(bad)).error() == gmsg::Error::InvalidValue);
}

TEMPLATE_TEST_CASE("Stuffed streams resync on the reserved sync byte", "", SyncLink, WireChecksum, WireLength, CrcLength, WireCrcLength,
                   WireCrcOverWire, Delimited) {
    std::vector<std::uint8_t> wire;
    std::array<std::uint8_t, TestType::max_frame> buf{};
    auto add = [&](const auto& m) {
        auto n = TestType::encode(m, buf);
        REQUIRE(n);
        wire.insert(wire.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    };
    add(make_data(10, 1));
    // a truncated frame: the next sync byte must start a new frame immediately
    const std::size_t cut_at = wire.size();
    add(make_data(40, 2));
    wire.resize(cut_at + 12);
    add(Ping{0x80818283u});
    wire.insert(wire.end(), {0x11, 0x22});  // garbage
    add(make_data(150, 4));

    int datas = 0, pings = 0, errors = 0;
    typename TestType::template Stream<> stream;
    for (auto b : wire) {
        stream.feed(gmsg::bytes_view(&b, 1), gmsg::overloaded{
                                                [&](const DataView&) { ++datas; },
                                                [&](const PingView& p) { CHECK(p.seq() == 0x80818283u); ++pings; },
                                                [&](const gmsg::FrameError&) { ++errors; },
                                            });
    }
    CHECK(datas == 2);
    CHECK(pings == 1);
    CHECK(errors >= 1);
    // only the truncated frame (12 bytes) and the 2 garbage bytes were dropped
    CHECK(stream.stats().bytes_skipped <= 12 + 2);
}

TEST_CASE("Escape round-trips all 256 byte values through every protocol") {
    Data d;
    for (std::size_t i = 0; i < 120; ++i) (void)d.data_.push_back(static_cast<std::uint8_t>(i + 136));
    d.n = 120;
    roundtrip<SyncLink>(d);
    roundtrip<WireChecksum>(d);
    roundtrip<WireLength>(d);
    roundtrip<CrcLength>(d);
    roundtrip<WireCrcLength>(d);
    roundtrip<WireCrcOverWire>(d);
    roundtrip<Delimited>(d);
    Data lo;
    for (std::size_t i = 0; i < 120; ++i) (void)lo.data_.push_back(static_cast<std::uint8_t>(i + 16));
    lo.n = 120;
    roundtrip<SyncLink>(lo);
    roundtrip<WireLength>(lo);
    roundtrip<Delimited>(lo);
}

TEST_CASE("WireLength: the length field counts body bytes on the wire") {
    std::array<std::uint8_t, WireLength::max_frame> buf{};
    for (std::size_t n = 0; n <= 120; ++n) {
        CAPTURE(n);
        const Data d = make_data(n, 7);
        auto len = WireLength::encode(d, buf);
        REQUIRE(len);
        // escaped body size = 1 (n) + data bytes + one extra byte per reserved value
        std::size_t wire_body = 1 + n;
        if (n == 0x80 || n == 0x81) ++wire_body;
        for (auto b : d.data_) wire_body += (b == 0x80 || b == 0x81) ? 1 : 0;
        // header: 80 | 01 | len (escaped if it is 0x80 / 0x81)
        std::size_t value = buf[2];
        if (value == 0x81) value = buf[3] == 0x00 ? 0x80 : 0x81;
        CHECK(value == wire_body);
    }
    // a frame whose length points into the middle of an escape pair is rejected
    // body: n=1, data 80 -> wire 01 81 00 (3 bytes); claim 2
    const auto crc = gmsg::checksum::crc16_ccitt::compute(std::array<std::uint8_t, 5>{0x01, 0x02, 0x01, 0x80, 0x00});
    std::vector<std::uint8_t> bad{0x80, 0x01, 0x02, 0x01, 0x81, 0x00, static_cast<std::uint8_t>(crc >> 8), static_cast<std::uint8_t>(crc)};
    CHECK_FALSE(WireLength::parse(gmsg::mut_bytes(bad)));
}

namespace {

/// Length byte of a frame (index 2, after sync and kind), read through the escaping.
std::size_t length_byte(const std::array<std::uint8_t, 512>& buf) {
    std::size_t v = buf[2];
    if (v == 0x81) v = buf[3] == 0x00 ? 0x80 : 0x81;
    return v;
}

std::size_t escaped(std::size_t v) { return (v == 0x80 || v == 0x81) ? 2 : 1; }

} // namespace

TEST_CASE("covers: body_checksum counts the checksum bytes") {
    static_assert(CrcLength::max_frame <= 512 && WireCrcLength::max_frame <= 512 && WireCrcOverWire::max_frame <= 512);
    std::array<std::uint8_t, 512> buf{};
    for (std::size_t n = 0; n <= 120; ++n) {
        CAPTURE(n);
        const Data d = make_data(n, 11);
        // unescaped: body (1 + n) + 2 checksum bytes
        REQUIRE(CrcLength::encode(d, buf));
        CHECK(length_byte(buf) == 1 + n + 2);

        // wire: escaped body + escaped checksum; the frame ends right after them
        auto len = WireCrcLength::encode(d, buf);
        REQUIRE(len);
        std::size_t header_wire = 1 + escaped(length_byte(buf));  // kind, len
        CHECK(1 + header_wire + length_byte(buf) == *len);

        len = WireCrcOverWire::encode(d, buf);
        REQUIRE(len);
        header_wire = 1 + escaped(length_byte(buf));
        CHECK(1 + header_wire + length_byte(buf) == *len);
    }
}
