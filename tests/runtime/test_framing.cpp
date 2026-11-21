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
    constexpr EscapeConfig esc = make_xor_escape(0x7D, 0x20, {0x7E});
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
    constexpr EscapeConfig esc = make_map_escape(0xDB, {0xC0, 0xDB}, {0xDC, 0xDD});
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

TEST_CASE("Index style escaping round-trips every byte value") {
    // 0x80 is the sync byte, 0x81 the escape byte: 0x80 -> 81 00, 0x81 -> 81 01
    constexpr EscapeConfig esc = make_index_escape(0x81, {0x80, 0x81});
    static_assert(esc.needs_escape(0x80) && esc.needs_escape(0x81) && !esc.needs_escape(0x82));
    std::array<std::uint8_t, 512> buf{};
    for (std::size_t i = 0; i < 256; ++i) buf[i] = static_cast<std::uint8_t>(i);
    auto m = escape_in_place(buf, 0, 256, esc);
    REQUIRE(m);
    CHECK(*m == 258);
    CHECK(buf[0x80] == 0x81);
    CHECK(buf[0x81] == 0x00);
    CHECK(buf[0x82] == 0x81);
    CHECK(buf[0x83] == 0x01);
    for (std::size_t i = 0; i < *m; ++i) CHECK(buf[i] != 0x80);
    auto un = unescaped_size(bytes_view(buf.data(), *m), esc);
    REQUIRE(un);
    CHECK(*un == 256);
    auto back = unescape_in_place(mut_bytes(buf.data(), *m), esc);
    REQUIRE(back);
    REQUIRE(*back == 256);
    for (std::size_t i = 0; i < 256; ++i) CHECK(buf[i] == i);

    // unknown code after the escape byte
    std::array<std::uint8_t, 2> bad{0x81, 0x05};
    CHECK(unescape_in_place(bad, esc).error() == Error::InvalidValue);

    // index base moves the codes
    constexpr EscapeConfig based = make_index_escape(0x81, {0x00, 0x80, 0x81}, 0x10);
    CHECK(based.encode(0x00) == 0x10);
    CHECK(based.encode(0x81) == 0x12);
    CHECK(*based.decode(0x11) == 0x80);
}

TEST_CASE("walk_escaped") {
    constexpr EscapeConfig esc = make_index_escape(0x81, {0x80, 0x81});
    const std::vector<std::uint8_t> d{0x80, 0x01, 0x81, 0x00, 0x02, 0x81, 0x01, 0x03};
    std::array<std::uint8_t, 2> pick{};
    auto w = walk_escaped(bv(d), 1, 3, esc, 1, mut_bytes(pick));
    REQUIRE(w.kind == EscapeWalk::Kind::done);
    CHECK(w.wire == 5);
    CHECK(pick[0] == 0x80);
    CHECK(pick[1] == 0x02);
    CHECK(walk_escaped(bv(d), 1, 6, esc).kind == EscapeWalk::Kind::need_more);
    CHECK(walk_escaped(bv(d).first(6), 1, 4, esc).kind == EscapeWalk::Kind::need_more);  // escape pair split
    // an unescaped sync byte inside the frame
    const std::vector<std::uint8_t> cut{0x80, 0x01, 0x80, 0x02};
    w = walk_escaped(bv(cut), 1, 3, esc);
    CHECK(w.kind == EscapeWalk::Kind::bad);
    CHECK(w.wire == 2);
    CHECK(w.error == Error::BadDelimiter);
}

TEST_CASE("Length prefixed framing with escaping") {
    // sync 0x80, length u8 (body bytes, unescaped), body, no checksum
    constexpr LengthPrefixedConfig cfg{Pattern{{0x80}, 1}, 1, 1, Endian::big, 2, 2, 64,
                                       make_index_escape(0x81, {0x80, 0x81}), 0};
    // body {0x80, 0x05} -> length 2, escaped: 80 02 81 00 05
    const std::vector<std::uint8_t> d{0x80, 0x02, 0x81, 0x00, 0x05, 0x80};
    auto s = scan_length_prefixed(bv(d), cfg);
    REQUIRE(s.kind == FrameScan::Kind::frame);
    CHECK(s.size == 5);
    CHECK(scan_length_prefixed(bv(d).first(4), cfg).kind == FrameScan::Kind::need_more);
    // escaped length byte: length 0x81 is sent as 81 01
    constexpr LengthPrefixedConfig big{Pattern{{0x80}, 1}, 1, 1, Endian::big, 2, 2, 512,
                                       make_index_escape(0x81, {0x80, 0x81}), 0};
    std::vector<std::uint8_t> l{0x80, 0x81, 0x01};
    l.resize(3 + 0x81, 0x11);
    s = scan_length_prefixed(bv(l), big);
    REQUIRE(s.kind == FrameScan::Kind::frame);
    CHECK(s.size == 3 + 0x81);
    // a new sync byte in the middle of a frame: resync there
    const std::vector<std::uint8_t> cut{0x80, 0x03, 0x01, 0x80, 0x01, 0x07};
    s = scan_length_prefixed(bv(cut), cfg);
    CHECK(s.kind == FrameScan::Kind::skip);
    CHECK(s.size == 3);
    CHECK(s.error == Error::BadDelimiter);
}

TEST_CASE("Length prefixed framing counting wire bytes") {
    // sync 0x80 | len u8 (body bytes on the wire) | body | 1-byte trailer
    LengthPrefixedConfig cfg{Pattern{{0x80}, 1}, 1, 1, Endian::big, 0, 3, 64, make_index_escape(0x81, {0x80, 0x81}), 0};
    cfg.counts_wire = true;
    cfg.header_len = 1;
    cfg.trailer_len = 1;
    // body 81 00 05 (3 wire bytes = 0x80 0x05), trailer escaped 81 01
    const std::vector<std::uint8_t> d{0x80, 0x03, 0x81, 0x00, 0x05, 0x81, 0x01, 0x80};
    auto s = scan_length_prefixed(bv(d), cfg);
    REQUIRE(s.kind == FrameScan::Kind::frame);
    CHECK(s.size == 7);
    CHECK(scan_length_prefixed(bv(d).first(6), cfg).kind == FrameScan::Kind::need_more);
    // the length ends inside an escape pair
    const std::vector<std::uint8_t> split{0x80, 0x02, 0x05, 0x81, 0x00, 0x07};
    s = scan_length_prefixed(bv(split), cfg);
    CHECK(s.kind == FrameScan::Kind::skip);
    CHECK(s.error == Error::BadLength);
    // an escaped length byte: 0x80 wire bytes of body
    std::vector<std::uint8_t> big{0x80, 0x81, 0x00};
    big.resize(3 + 0x80 + 1, 0x11);
    LengthPrefixedConfig large = cfg;
    large.max_frame = 512;
    s = scan_length_prefixed(bv(big), large);
    REQUIRE(s.kind == FrameScan::Kind::frame);
    CHECK(s.size == 3 + 0x80 + 1);
    // a sync byte in the body: the frame was cut short
    const std::vector<std::uint8_t> cut{0x80, 0x04, 0x01, 0x80, 0x03};
    s = scan_length_prefixed(bv(cut), cfg);
    CHECK(s.kind == FrameScan::Kind::skip);
    CHECK(s.size == 3);
    CHECK(s.error == Error::BadDelimiter);
}

TEST_CASE("Length prefixed framing counting wire bytes of body and trailer") {
    // sync 0x80 | len u8 (body + trailer bytes on the wire) | body | 1-byte trailer
    LengthPrefixedConfig cfg{Pattern{{0x80}, 1}, 1, 1, Endian::big, 0, 3, 64, make_index_escape(0x81, {0x80, 0x81}), 0};
    cfg.counts_wire = true;
    cfg.header_len = 1;
    cfg.trailer_len = 1;
    cfg.wire_includes_trailer = true;
    // body 81 00 05, trailer 81 01: 5 wire bytes
    const std::vector<std::uint8_t> d{0x80, 0x05, 0x81, 0x00, 0x05, 0x81, 0x01, 0x80};
    auto s = scan_length_prefixed(bv(d), cfg);
    REQUIRE(s.kind == FrameScan::Kind::frame);
    CHECK(s.size == 7);
    // a length that splits the escaped trailer
    const std::vector<std::uint8_t> split{0x80, 0x04, 0x81, 0x00, 0x05, 0x81, 0x01};
    s = scan_length_prefixed(bv(split), cfg);
    CHECK(s.kind == FrameScan::Kind::skip);
    CHECK(s.error == Error::BadLength);
}

TEST_CASE("Fixed framing with escaping") {
    constexpr FixedFrameConfig cfg{Pattern{{0x80}, 1}, 4, make_index_escape(0x81, {0x80, 0x81}), 0};
    const std::vector<std::uint8_t> d{0x80, 0x81, 0x01, 0x02, 0x81, 0x00, 0x80};
    auto s = scan_fixed(bv(d), cfg);
    REQUIRE(s.kind == FrameScan::Kind::frame);
    CHECK(s.size == 6);
    CHECK(scan_fixed(bv(d).first(5), cfg).kind == FrameScan::Kind::need_more);
}
