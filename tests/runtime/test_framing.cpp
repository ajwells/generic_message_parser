#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <vector>

#include "gmp/framing/delimited.hpp"
#include "gmp/framing/escape.hpp"
#include "gmp/framing/fixed.hpp"
#include "gmp/framing/length_prefixed.hpp"

using namespace gmp;

namespace {
bytes_view bv(const std::vector<std::uint8_t>& v) { return {v.data(), v.size()}; }
bytes_view sv(std::string_view s) { return as_bytes(s.data(), s.size()); }
} // namespace

TEST_CASE("fixed framer with sync") {
    constexpr FixedFrameConfig cfg{Pattern{{0xAA, 0x55}, 2}, 4};
    std::vector<std::uint8_t> d{0x00, 0x01, 0xAA, 0x55, 0x10, 0x20};
    auto s = scan_fixed(bv(d), cfg);
    CHECK(s.kind == FrameScan::Kind::skip);
    CHECK(s.size == 2);
    CHECK(s.error == Error::BadSync);
    s = scan_fixed(bv(d).subspan(2), cfg);
    CHECK(s.kind == FrameScan::Kind::frame);
    CHECK(s.size == 4);
    s = scan_fixed(bv(d).subspan(2, 3), cfg);
    CHECK(s.kind == FrameScan::Kind::need_more);
    // partial sync at end of buffer -> need more
    std::vector<std::uint8_t> p{0xAA};
    CHECK(scan_fixed(bv(p), cfg).kind == FrameScan::Kind::need_more);
}

TEST_CASE("length prefixed framer") {
    // [AA][len u16 be = body][body...][crc8]
    constexpr LengthPrefixedConfig cfg{Pattern{{0xAA}, 1}, 1, 2, Endian::big, 1 + 2 + 1, 4, 64};
    std::vector<std::uint8_t> d{0xAA, 0x00, 0x03, 1, 2, 3, 0xCC, 0xAA};
    auto s = scan_length_prefixed(bv(d), cfg);
    REQUIRE(s.kind == FrameScan::Kind::frame);
    CHECK(s.size == 7);
    CHECK(s.consume_on_error == 1);
    CHECK(scan_length_prefixed(bv(d).first(5), cfg).kind == FrameScan::Kind::need_more);

    std::vector<std::uint8_t> bad{0xAA, 0xFF, 0xFF, 0};
    s = scan_length_prefixed(bv(bad), cfg);
    CHECK(s.kind == FrameScan::Kind::skip);
    CHECK(s.error == Error::BadLength);
}

TEST_CASE("delimited text framer (NMEA style)") {
    constexpr DelimitedConfig cfg{Pattern{{'$'}, 1}, Pattern{{'\r', '\n'}, 2}, {}, 82};
    auto s = scan_delimited(sv("xx$GPGGA,1*00\r\n"), cfg);
    CHECK(s.kind == FrameScan::Kind::skip);
    CHECK(s.size == 2);
    s = scan_delimited(sv("$GPGGA,1*00\r\n$"), cfg);
    REQUIRE(s.kind == FrameScan::Kind::frame);
    CHECK(s.size == 13);
    CHECK(scan_delimited(sv("$GPGGA,1*0"), cfg).kind == FrameScan::Kind::need_more);
    // A new '$' before the terminator means the first sentence was truncated.
    s = scan_delimited(sv("$GPG$GPGGA\r\n"), cfg);
    CHECK(s.kind == FrameScan::Kind::skip);
    CHECK(s.size == 4);
    CHECK(s.error == Error::BadDelimiter);
}

TEST_CASE("delimited framer without start delimiter (line based)") {
    constexpr DelimitedConfig cfg{Pattern{}, Pattern{{'\n'}, 1}, {}, 16};
    auto s = scan_delimited(sv("hello\nworld"), cfg);
    REQUIRE(s.kind == FrameScan::Kind::frame);
    CHECK(s.size == 6);
    s = scan_delimited(sv("0123456789abcdefXYZ"), cfg);
    CHECK(s.kind == FrameScan::Kind::skip);
    CHECK(s.error == Error::BadLength);
}

TEST_CASE("HDLC style shared flags with escaping") {
    EscapeConfig esc{};
    esc.enabled = true;
    esc.esc = 0x7D;
    esc.xor_mask = 0x20;
    esc.reserved = {0x7E};
    esc.reserved_len = 1;
    const DelimitedConfig cfg{Pattern{{0x7E}, 1}, Pattern{{0x7E}, 1}, esc, 64};

    std::vector<std::uint8_t> d{0x7E, 0x01, 0x7D, 0x5E, 0x02, 0x7E, 0x03, 0x7E};
    auto s = scan_delimited(bv(d), cfg);
    REQUIRE(s.kind == FrameScan::Kind::frame);
    CHECK(s.size == 6);
    CHECK(s.consume == 5);  // closing flag left in place to open the next frame

    // back-to-back flags
    std::vector<std::uint8_t> ff{0x7E, 0x7E, 0x01, 0x7E};
    s = scan_delimited(bv(ff), cfg);
    CHECK(s.kind == FrameScan::Kind::skip);
    CHECK(s.size == 1);
    CHECK(s.error == Error::None);

    // unescape interior
    std::vector<std::uint8_t> body{0x01, 0x7D, 0x5E, 0x02};
    auto n = unescape_in_place(mut_bytes(body), esc);
    REQUIRE(n);
    CHECK(*n == 3);
    CHECK(body[1] == 0x7E);

    // escape round trip in place
    std::array<std::uint8_t, 8> out{0x7E, 0x7D, 0x01, 0x7E};
    auto m = escape_in_place(out, 0, 4, esc);
    REQUIRE(m);
    CHECK(*m == 7);
    auto back = unescape_in_place(mut_bytes(out.data(), *m), esc);
    REQUIRE(back);
    CHECK(*back == 4);
    CHECK(out[0] == 0x7E);
    CHECK(out[1] == 0x7D);
    CHECK(out[2] == 0x01);
    CHECK(out[3] == 0x7E);
}

TEST_CASE("SLIP style map escaping") {
    EscapeConfig esc{};
    esc.enabled = true;
    esc.esc = 0xDB;
    esc.map_len = 2;
    esc.map_raw = {0xC0, 0xDB};
    esc.map_code = {0xDC, 0xDD};
    std::array<std::uint8_t, 8> out{0xC0, 0x11, 0xDB};
    auto m = escape_in_place(out, 0, 3, esc);
    REQUIRE(m);
    CHECK(*m == 5);
    CHECK(out[0] == 0xDB);
    CHECK(out[1] == 0xDC);
    CHECK(out[2] == 0x11);
    CHECK(out[3] == 0xDB);
    CHECK(out[4] == 0xDD);
}
