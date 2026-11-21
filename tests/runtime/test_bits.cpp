#include <catch2/catch_test_macros.hpp>

#include <array>

#include "gmp/bits.hpp"

using namespace gmp;

TEST_CASE("msb_first bit extraction") {
    std::array<std::uint8_t, 2> b{0b1011'0011, 0b0100'0000};
    CHECK(extract_bits(b.data(), 0, 1, BitOrder::msb_first) == 1);
    CHECK(extract_bits(b.data(), 1, 3, BitOrder::msb_first) == 0b011);
    CHECK(extract_bits(b.data(), 4, 6, BitOrder::msb_first) == 0b0011'01);
    CHECK(extract_bits(b.data(), 0, 16, BitOrder::msb_first) == 0b1011'0011'0100'0000);
}

TEST_CASE("lsb_first bit extraction (CAN Intel style)") {
    std::array<std::uint8_t, 2> b{0b1011'0011, 0b0000'0001};
    CHECK(extract_bits(b.data(), 0, 1, BitOrder::lsb_first) == 1);
    CHECK(extract_bits(b.data(), 0, 4, BitOrder::lsb_first) == 0b0011);
    CHECK(extract_bits(b.data(), 4, 4, BitOrder::lsb_first) == 0b1011);
    // 12-bit value spanning bytes: low byte then low nibble of next
    CHECK(extract_bits(b.data(), 0, 12, BitOrder::lsb_first) == 0x1B3);
}

TEST_CASE("insert/extract round trip for every width and offset") {
    for (auto order : {BitOrder::msb_first, BitOrder::lsb_first}) {
        for (unsigned width = 1; width <= 20; ++width) {
            for (std::size_t off = 0; off < 12; ++off) {
                std::array<std::uint8_t, 8> b{};
                b.fill(0xA5);
                const std::uint64_t v = (0x5A5A5ull >> (20 - width)) & ((1ull << width) - 1);
                insert_bits(b.data(), off, width, v, order);
                REQUIRE(extract_bits(b.data(), off, width, order) == v);
            }
        }
    }
}

TEST_CASE("signed bit fields are sign extended") {
    std::array<std::uint8_t, 1> b{};
    set_bits<std::int8_t>(b.data(), 0, 4, -3, BitOrder::msb_first);
    CHECK(get_bits<std::int8_t>(b.data(), 0, 4, BitOrder::msb_first) == -3);
    CHECK(get_bits<std::uint8_t>(b.data(), 0, 4, BitOrder::msb_first) == 0b1101);
}

TEST_CASE("BitReader/BitWriter sequential") {
    std::array<std::uint8_t, 2> b{};
    BitWriter w(b, BitOrder::msb_first);
    REQUIRE(w.write(0b101, 3));
    REQUIRE(w.write(0x1FF, 9));
    REQUIRE(w.write(0, 4));
    REQUIRE_FALSE(w.write(1, 1));
    BitReader r(b, BitOrder::msb_first);
    CHECK(*r.read(3) == 0b101);
    CHECK(*r.read(9) == 0x1FF);
    CHECK(*r.read(4) == 0);
    CHECK_FALSE(r.read(1));
}
