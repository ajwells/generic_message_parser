// Computed values (tests/schemas/computed.yaml): accessors on struct and view, automatic and explicit setters.
#include <catch2/catch_test_macros.hpp>

#include <array>

#include "computed.gmp.hpp"

using namespace test::computed;

namespace {
template <class T>
concept HasSetSum = requires(T t) { t.set_sum(1); };
template <class T>
concept HasSetLevelX2 = requires(T t) { t.set_level_x2(1); };
template <class T>
concept HasSetOverlapping = requires(T t) { t.set_overlapping(1); };
template <class T>
concept HasSetRgb = requires(T t) { t.set_rgb(1u); };
template <class V>
concept ViewHasSetter = requires(V v) { v.set_rgb(1u); };
} // namespace

TEST_CASE("computed values read the same on struct and view") {
    Packed p{};
    p.b0 = 0x12;
    p.b1 = 0x34;
    p.b2 = 0x56;
    p.wide = 0xAABBCCDD;
    p.lo = 1234;
    p.flags.level = 21;
    p.unit = Unit::cm;
    (void)p.items.push_back(7);
    std::array<std::uint8_t, 32> buf{};
    auto n = p.encode(buf);
    REQUIRE(n);
    auto v = PackedView::from(gmp::bytes_view(buf.data(), *n));
    REQUIRE(v);

    CHECK(p.rgb() == 0x123456u);
    CHECK(v->rgb() == 0x123456u);
    CHECK(p.big() == 0xAABBCCDD04D2ull);
    CHECK(v->big() == p.big());
    CHECK(p.sum() == 0x12 + 0x34 - 0x56);
    CHECK(v->sum() == p.sum());
    CHECK(p.level_x2() == 42);
    CHECK(v->level_x2() == 42);
    CHECK(p.length_mm() == 12340);
    CHECK(v->length_mm() == 12340);
    CHECK(p.ratio() == 308.0);  // integer expression, converted to the declared type
    CHECK(p.first_two() == 0x1234);
    STATIC_REQUIRE(Packed{}.rgb() == 0);  // struct getters are constexpr
}

TEST_CASE("automatic setters split packed values back into fields") {
    Packed p{};
    p.set_rgb(0xA1B2C3u);
    CHECK(p.b0 == 0xA1);
    CHECK(p.b1 == 0xB2);
    CHECK(p.b2 == 0xC3);
    CHECK(p.rgb() == 0xA1B2C3u);
    p.set_big(0x1122334455ull);
    CHECK(p.wide == 0x112233u);
    CHECK(p.lo == 0x4455);
    CHECK(p.big() == 0x1122334455ull);
    p.set_first_two(0xBEEF);
    CHECK(p.b0 == 0xBE);
    CHECK(p.b1 == 0xEF);
    CHECK(p.b2 == 0xC3);  // untouched
}

TEST_CASE("explicit set: assigns the listed fields") {
    Packed p{};
    p.unit = Unit::cm;
    p.lo = 1;
    p.set_length_mm(500);
    CHECK(p.unit == Unit::mm);
    CHECK(p.lo == 500);
    CHECK(p.length_mm() == 500);
}

TEST_CASE("setters exist only where the value can be split, and only on the struct") {
    STATIC_REQUIRE(HasSetRgb<Packed>);
    STATIC_REQUIRE_FALSE(HasSetSum<Packed>);          // b0 + b1 - b2
    STATIC_REQUIRE_FALSE(HasSetLevelX2<Packed>);      // multiplies a bits member
    STATIC_REQUIRE_FALSE(HasSetOverlapping<Packed>);  // (b0 << 4) | b1 overlap
    STATIC_REQUIRE_FALSE(ViewHasSetter<PackedView>);  // views are read-only
}
