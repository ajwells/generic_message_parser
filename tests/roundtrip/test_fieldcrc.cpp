// Field-level checksums (`crc:`) and extra conversions in less common positions (tests/schemas/fieldcrc.yaml).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <string_view>

#include "fieldcrc.gmp.hpp"

using namespace test::fieldcrc;
using Catch::Matchers::WithinAbs;

namespace {
Mixed sample(State st) {
    Mixed m{};
    m.state = st;
    (void)m.items.push_back(0x1111);
    (void)m.items.push_back(0x2222);
    m.count = 2;  // encode derives it from items.size(); set it so decode compares equal
    m.level = -6;
    if (st == State::error) m.extra = 0xBEEF;
    return m;
}
} // namespace

TEST_CASE("field CRCs round-trip around variable and conditional fields") {
    for (State st : {State::on, State::error}) {
        INFO("state " << static_cast<int>(st));
        const Mixed m = sample(st);
        std::array<std::uint8_t, 32> buf{};
        auto n = m.encode(buf);
        REQUIRE(n);
        // state+crc, count, 2 items, level+crc16, [extra+crc], magic+crc
        CHECK(*n == 2 + 1 + 4 + 4 + (st == State::error ? 3u : 0u) + 2);
        const gmsg::bytes_view wire(buf.data(), *n);

        auto back = Mixed::decode(wire);
        REQUIRE(back);
        CHECK(*back == m);
        CHECK(back->count == 2);

        auto v = MixedView::from(wire);
        REQUIRE(v);
        CHECK(v->state() == st);
        CHECK(v->level() == -6);
        CHECK(v->extra() == m.extra);
        CHECK(v->items()[1] == 0x2222);

        // every byte of the message is covered by a CRC, a const check or the length walk
        for (std::size_t i = 0; i < *n; ++i) {
            if (i == 2) continue;  // count: changing it changes the layout instead
            if (i >= 3 && i < 7) continue;  // array items have no CRC
            INFO("flipped byte " << i);
            auto bad = buf;
            bad[i] ^= 0x01;
            const gmsg::bytes_view b(bad.data(), *n);
            CHECK_FALSE(Mixed::decode(b));
            CHECK_FALSE(MixedView::from(b));
        }
    }
}

TEST_CASE("inline CRC-16 is stored in the field's (little-endian) byte order") {
    const Mixed m = sample(State::on);
    std::array<std::uint8_t, 32> buf{};
    REQUIRE(m.encode(buf));
    // level at offset 7: i16 -6 little endian, then CRC-16/CCITT-FALSE of those two bytes, little endian
    CHECK(buf[7] == 0xFA);
    CHECK(buf[8] == 0xFF);
    const std::uint8_t level[] = {0xFA, 0xFF};
    const auto crc = gmsg::checksum::crc16_ccitt::compute(gmsg::bytes_view(level, 2));
    CHECK(buf[9] == (crc & 0xFF));
    CHECK(buf[10] == (crc >> 8));
}

TEST_CASE("named conversions on binary and text fields") {
    Mixed m = sample(State::on);
    CHECK_THAT(m.level_scaled(), WithinAbs(-3.0, 1e-12));
    CHECK_THAT(m.level_percent(), WithinAbs(-1.5, 1e-12));
    CHECK_THAT(m.level_centi(), WithinAbs(-299.0, 1e-12));
    m.set_level_percent(10.0);
    CHECK(m.level == 40);
    m.set_level_centi(201.0);
    CHECK(m.level == 4);

    constexpr std::string_view rec = "1234";
    auto v = ReadingView::from(gmsg::as_bytes(rec.data(), rec.size()));
    REQUIRE(v);
    CHECK_THAT(*v->raw_scaled(), WithinAbs(123.4, 1e-9));
    CHECK_THAT(*v->raw_millis(), WithinAbs(123400.0, 1e-9));
}
