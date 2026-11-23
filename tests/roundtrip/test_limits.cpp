// Value limits (min_value / max_value / min_raw / max_raw) on decode and encode (tests/schemas/limits.yaml).
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

#include "limits.gmp.hpp"

using namespace test::limits;

namespace {

using Bytes = std::vector<std::uint8_t>;

// Sample layout (big endian): humidity u16 @0, temp i16 @2, big u64 @4, neg i64 @12, ratio f32 @20, wide f64 @24,
// flags @32 (level: 3 bits, trim: 5 bits) @33 (gain), levels u16[2] @34, inner.pct @38, n @39, more i8[n] @40, opt
Sample good_sample() {
    Sample s;
    s.humidity = 30000;
    s.temp = 215;
    s.big = 0x8000000000000001ull;
    s.neg = -7;
    s.ratio = 0.25f;
    s.wide = 1.0;
    s.flags.level = 2;
    s.flags.trim = -3;
    s.flags.gain = 40;
    s.levels = {10, 1000};
    s.inner.pct = 99;
    s.n = 2;  // derived from `more` on encode; set so a decoded copy compares equal
    (void)s.more.push_back(-50);
    (void)s.more.push_back(5);
    s.opt = 2;
    return s;
}

Bytes encode(const Sample& s) {
    std::array<std::uint8_t, 128> buf{};
    const auto n = s.encode(buf);
    REQUIRE(n);
    return Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
}

template <class T>
void put(Bytes& b, std::size_t at, T v) {
    const auto x = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(v);
    for (std::size_t i = 0; i < sizeof(T); ++i) b[at + i] = x[sizeof(T) - 1 - i];  // big endian (little-endian host)
}

/// Decode `b` as struct and view: both succeed, or both fail with ValueOutOfLimits.
void expect(const Bytes& b, bool ok) {
    const gmp::bytes_view v(b.data(), b.size());
    const auto s = Sample::decode(v);
    const auto w = SampleView::from(v);
    if (ok) {
        CHECK(s);
        CHECK(w);
    } else {
        REQUIRE_FALSE(s);
        CHECK(s.error() == gmp::Error::ValueOutOfLimits);
        REQUIRE_FALSE(w);
        CHECK(w.error() == gmp::Error::ValueOutOfLimits);
    }
}

template <class T>
void expect_value(std::size_t at, T v, bool ok) {
    INFO("offset " << at << " value " << +v);
    auto b = encode(good_sample());
    put(b, at, v);
    expect(b, ok);
}

void expect_encode_fails(const Sample& s) {
    std::array<std::uint8_t, 128> buf{};
    const auto n = s.encode(buf);
    REQUIRE_FALSE(n);
    CHECK(n.error() == gmp::Error::ValueOutOfLimits);
}

} // namespace

TEST_CASE("values inside the limits decode and encode") {
    const auto b = encode(good_sample());
    const auto s = Sample::decode(b);
    REQUIRE(s);
    CHECK(*s == good_sample());
    REQUIRE(SampleView::from(b));
}

TEST_CASE("min_value / max_value compare the scaled value") {
    // humidity = raw * 0.0019073486328125 - 6 must be in [0, 100]: every raw value against the same formula
    for (std::uint32_t raw = 0; raw <= 0xFFFF; ++raw) {
        const double h = static_cast<double>(raw) * 0.0019073486328125 + -6.0;
        if (raw % 97 != 0 && raw != 3145 && raw != 3146 && raw != 55574 && raw != 55575) continue;
        expect_value<std::uint16_t>(0, static_cast<std::uint16_t>(raw), h >= 0.0 && h <= 100.0);
    }
    expect_value<std::uint16_t>(0, 3145, false);  // -0.0000...
    expect_value<std::uint16_t>(0, 3146, true);
    expect_value<std::uint16_t>(0, 55574, true);
    expect_value<std::uint16_t>(0, 55575, false);
    // temp: min_value -30.5 (scaled by 0.1) is stricter than min_raw -400
    expect_value<std::int16_t>(2, -304, true);
    expect_value<std::int16_t>(2, -306, false);
    expect_value<std::int16_t>(2, -400, false);
    // bit field gain: raw * 0.5 <= 100
    auto b = encode(good_sample());
    b[33] = 200;
    expect(b, true);
    b[33] = 201;
    expect(b, false);
    // no scale: max_value compares the raw value
    b = encode(good_sample());
    b[38] = 100;
    expect(b, true);
    b[38] = 101;
    expect(b, false);
}

TEST_CASE("min_raw / max_raw compare the wire value exactly") {
    expect_value<std::int16_t>(2, 1250, true);
    expect_value<std::int16_t>(2, 1251, false);
    expect_value<std::uint64_t>(4, 0x8000000000000000ull, true);
    expect_value<std::uint64_t>(4, 0x7FFFFFFFFFFFFFFFull, false);  // 2^63 - 1 is not representable as a double
    expect_value<std::uint64_t>(4, 0xFFFFFFFFFFFFFFFEull, true);   // neither are these two
    expect_value<std::uint64_t>(4, 0xFFFFFFFFFFFFFFFFull, false);
    expect_value<std::int64_t>(12, -1, true);
    expect_value<std::int64_t>(12, 0, false);
    expect_value<std::int64_t>(12, std::numeric_limits<std::int64_t>::min(), true);
    // bits: level (3 bits, msb first) <= 5; trim (5 signed bits) >= -10
    auto b = encode(good_sample());
    b[32] = static_cast<std::uint8_t>((5u << 5) | (b[32] & 0x1Fu));
    expect(b, true);
    b[32] = static_cast<std::uint8_t>((6u << 5) | (b[32] & 0x1Fu));
    expect(b, false);
    b = encode(good_sample());
    b[32] = static_cast<std::uint8_t>((b[32] & 0xE0u) | (static_cast<unsigned>(-10) & 0x1Fu));
    expect(b, true);
    b[32] = static_cast<std::uint8_t>((b[32] & 0xE0u) | (static_cast<unsigned>(-11) & 0x1Fu));
    expect(b, false);
}

TEST_CASE("floats: limits are inclusive and NaN fails") {
    expect_value<float>(20, 1.5f, true);
    expect_value<float>(20, -1.5f, true);
    expect_value<float>(20, std::nextafter(1.5f, 2.0f), false);
    expect_value<float>(20, std::numeric_limits<float>::quiet_NaN(), false);
    expect_value<double>(24, 1e300, true);
    expect_value<double>(24, std::numeric_limits<double>::infinity(), false);
    expect_value<double>(24, std::numeric_limits<double>::quiet_NaN(), false);
    expect_value<double>(24, -std::numeric_limits<double>::infinity(), true);  // only max_raw is set
}

TEST_CASE("array elements, derived lengths and conditional fields are checked") {
    expect_value<std::uint16_t>(34, 9, false);     // levels[0] < 10
    expect_value<std::uint16_t>(36, 1001, false);  // levels[1] > 1000
    expect_value<std::int8_t>(40, -51, false);     // more[0] < -50
    expect_value<std::uint8_t>(42, 1, false);      // opt < 2 (present because n > 1)
    expect_value<std::uint8_t>(42, 2, true);
    // n = 4 is above max_raw 3 even though `more` could hold 4 elements
    auto b = encode(good_sample());
    b[39] = 4;
    b.insert(b.begin() + 42, std::uint8_t{0});
    expect(b, false);

    // remaining array: every element <= 500
    Tail t;
    for (const std::uint16_t v : std::array<std::uint16_t, 3>{500, 1, 0}) (void)t.rest.push_back(v);
    std::array<std::uint8_t, 32> buf{};
    auto n = t.encode(buf);
    REQUIRE(n);
    CHECK(Tail::decode(gmp::bytes_view(buf.data(), *n)));
    buf[2] = 0x01;  // rest[1] = 501
    buf[3] = 0xF5;
    CHECK(Tail::decode(gmp::bytes_view(buf.data(), *n)).error() == gmp::Error::ValueOutOfLimits);
    CHECK(TailView::from(gmp::bytes_view(buf.data(), *n)).error() == gmp::Error::ValueOutOfLimits);
}

TEST_CASE("encode rejects values outside the limits") {
    auto s = good_sample();
    s.humidity = 3145;
    expect_encode_fails(s);
    s = good_sample();
    s.set_temp_scaled(-31.0);
    expect_encode_fails(s);
    s = good_sample();
    s.big = 1;
    expect_encode_fails(s);
    s = good_sample();
    s.ratio = std::numeric_limits<float>::quiet_NaN();
    expect_encode_fails(s);
    s = good_sample();
    s.flags.level = 7;
    expect_encode_fails(s);
    s = good_sample();
    s.levels[0] = 1;
    expect_encode_fails(s);
    s = good_sample();
    s.inner.pct = 200;
    expect_encode_fails(s);
    s = good_sample();
    s.more[1] = -100;
    expect_encode_fails(s);
    s = good_sample();
    s.opt = 0;
    expect_encode_fails(s);
    s = good_sample();
    (void)s.more.push_back(1);
    (void)s.more.push_back(2);  // n is derived: 4 > max_raw 3
    expect_encode_fails(s);
}

TEST_CASE("text fields: struct, view and encode check the limits") {
    const auto ok = Line::decode_text("99.5,999,,500");
    REQUIRE(ok);
    CHECK_FALSE(ok->opt);
    CHECK(Line::decode_text("0,0,-5,0"));
    for (const char* bad : {"99.6,1,,1", "-0.5,1,,1", "1,1000,,1", "1,1,-6,1", "1,1,,501", "nan,1,,1"}) {
        INFO(bad);
        const auto d = Line::decode_text(bad);
        REQUIRE_FALSE(d);
        CHECK(d.error() == gmp::Error::ValueOutOfLimits);
    }
    const std::string_view text = "1,1000,,1";
    const auto v = LineView::from(gmp::as_bytes(text.data(), text.size()));
    REQUIRE(v);  // text views parse on access
    CHECK(v->count().error() == gmp::Error::ValueOutOfLimits);
    CHECK(v->speed());

    Line l = *ok;
    l.count = 1000;
    std::array<std::uint8_t, 64> buf{};
    CHECK(l.encode(buf).error() == gmp::Error::ValueOutOfLimits);
}
