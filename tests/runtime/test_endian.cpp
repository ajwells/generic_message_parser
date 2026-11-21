#include <catch2/catch_test_macros.hpp>

#include <array>

#include "gmp/byte_reader.hpp"
#include "gmp/byte_writer.hpp"
#include "gmp/containers.hpp"
#include "gmp/endian.hpp"

using namespace gmp;

TEST_CASE("load/store integers in both byte orders") {
    std::array<std::uint8_t, 4> b{0x12, 0x34, 0x56, 0x78};
    CHECK(load<std::uint32_t>(b.data(), Endian::big) == 0x12345678u);
    CHECK(load<std::uint32_t>(b.data(), Endian::little) == 0x78563412u);
    CHECK(load<std::uint16_t>(b.data(), Endian::big) == 0x1234u);
    CHECK(load<std::int8_t>(b.data(), Endian::big) == 0x12);

    std::array<std::uint8_t, 8> o{};
    store<std::uint64_t>(o.data(), 0x0102030405060708ull, Endian::little);
    CHECK(o[0] == 0x08);
    CHECK(o[7] == 0x01);
    CHECK(load<std::uint64_t>(o.data(), Endian::little) == 0x0102030405060708ull);
}

TEST_CASE("load/store floats round trip") {
    std::array<std::uint8_t, 8> o{};
    store<float>(o.data(), 3.5f, Endian::big);
    CHECK(o[0] == 0x40);
    CHECK(o[1] == 0x60);
    CHECK(load<float>(o.data(), Endian::big) == 3.5f);
    store<double>(o.data(), -1.25, Endian::little);
    CHECK(load<double>(o.data(), Endian::little) == -1.25);
}

TEST_CASE("constexpr loads") {
    constexpr std::array<std::uint8_t, 2> b{0xAB, 0xCD};
    static_assert(load<std::uint16_t>(b.data(), Endian::big) == 0xABCD);
    static_assert(load_uint(b.data(), 2, Endian::little) == 0xCDAB);
}

TEST_CASE("ByteReader bounds checks") {
    std::array<std::uint8_t, 3> b{1, 2, 3};
    ByteReader r(b);
    auto a = r.read<std::uint16_t>(Endian::big);
    REQUIRE(a);
    CHECK(*a == 0x0102);
    auto c = r.read<std::uint16_t>(Endian::big);
    REQUIRE_FALSE(c);
    CHECK(c.error() == Error::Truncated);
    CHECK(r.position() == 2);  // failed read does not advance
    auto d = r.read<std::uint8_t>();
    REQUIRE(d);
    CHECK(*d == 3);
    CHECK(r.empty());
}

TEST_CASE("ByteWriter patching and overflow") {
    std::array<std::uint8_t, 4> b{};
    ByteWriter w(b);
    auto at = w.reserve(2);
    REQUIRE(at);
    REQUIRE(w.write<std::uint8_t>(0xEE));
    w.patch<std::uint16_t>(*at, 0xBEEF, Endian::little);
    CHECK(b[0] == 0xEF);
    CHECK(b[1] == 0xBE);
    CHECK(b[2] == 0xEE);
    REQUIRE(w.write<std::uint8_t>(1));
    auto full = w.write<std::uint8_t>(1);
    REQUIRE_FALSE(full);
    CHECK(full.error() == Error::BufferFull);
}

TEST_CASE("static_vector and static_string") {
    static_vector<int, 3> v;
    CHECK(v.push_back(1));
    CHECK(v.push_back(2));
    CHECK(v.push_back(3));
    CHECK_FALSE(v.push_back(4));
    CHECK(v.size() == 3);
    CHECK(v == static_vector<int, 3>{1, 2, 3});

    static_string<4> s("abcdef");
    CHECK(s.view() == "abcd");
    CHECK(s == "abcd");
}
