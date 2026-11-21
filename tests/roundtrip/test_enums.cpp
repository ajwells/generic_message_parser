// Strict enums reject undeclared values on decode (tests/schemas/enums.yaml).
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

#include "enums.gmp.hpp"

using namespace test::enums;

namespace {
// color, open, flags(mode in bits 7..6), levels[2] (u16), inner.color, n, more[n]
constexpr std::array<std::uint8_t, 11> kGood{0x01, 0x07, 0x40, 0x00, 0x0A, 0x00, 0x14, 0x03, 0x02, 0x02, 0x03};

void expect_unknown_enum(std::size_t index, std::uint8_t value) {
    INFO("byte " << index << " = " << int(value));
    auto bad = kGood;
    bad[index] = value;
    CHECK(Sample::decode(bad).error() == gmp::Error::UnknownEnum);
    CHECK(SampleView::from(bad).error() == gmp::Error::UnknownEnum);
}
} // namespace

TEST_CASE("is_known() reports declared values") {
    CHECK(is_known(Color::green));
    CHECK_FALSE(is_known(static_cast<Color>(9)));
    CHECK(is_known(Level::high));
    CHECK_FALSE(is_known(static_cast<Open>(7)));  // generated for every enum, strict or not
}

TEST_CASE("valid data decodes; a non-strict enum accepts any value") {
    auto s = Sample::decode(kGood);
    REQUIRE(s);
    CHECK(s->color == Color::red);
    CHECK(static_cast<int>(s->open) == 7);  // Open is strict: false
    CHECK(s->flags.mode == Mode::run);
    CHECK(s->levels[1] == Level::high);
    CHECK(s->more.size() == 2);
    REQUIRE(SampleView::from(kGood));
    std::array<std::uint8_t, 16> out{};
    REQUIRE(s->encode(out) == kGood.size());
}

TEST_CASE("undeclared values fail with UnknownEnum in struct and view") {
    expect_unknown_enum(0, 0x00);   // scalar
    expect_unknown_enum(2, 0xC0);   // enum inside a bits group (mode = 3)
    expect_unknown_enum(4, 0x01);   // fixed-count array element (levels[0] = 1)
    expect_unknown_enum(7, 0x09);   // nested message field
    expect_unknown_enum(10, 0x00);  // variable-count array element
    const std::array<std::uint8_t, 3> tail{0x01, 0x04, 0x02};
    CHECK(Tail::decode(tail).error() == gmp::Error::UnknownEnum);
    CHECK(TailView::from(tail).error() == gmp::Error::UnknownEnum);
    CHECK(gmp::to_string(gmp::Error::UnknownEnum) == "unknown enum value");
}

TEST_CASE("strict enums in text messages") {
    auto ok = Line::decode_text("2,9");
    REQUIRE(ok);
    CHECK(ok->color == Color::green);
    CHECK(Line::decode_text("4,1").error() == gmp::Error::UnknownEnum);
    constexpr std::string_view bad = "4,1";
    auto v = LineView::from(bad);
    REQUIRE(v);  // text views parse lazily...
    CHECK(v->color().error() == gmp::Error::UnknownEnum);  // ...so the accessor reports it
    CHECK(v->open());
}

TEST_CASE("encoding an undeclared strict enum value fails with UnknownEnum") {
    const Sample good = *Sample::decode(kGood);
    std::array<std::uint8_t, 16> out{};
    auto expect_fail = [&](const char* what, auto mutate) {
        INFO(what);
        Sample s = good;
        mutate(s);
        CHECK(s.encode(out).error() == gmp::Error::UnknownEnum);
    };
    expect_fail("scalar", [](Sample& s) { s.color = static_cast<Color>(0); });
    expect_fail("bits group", [](Sample& s) { s.flags.mode = static_cast<Mode>(3); });
    expect_fail("fixed-count array", [](Sample& s) { s.levels[1] = static_cast<Level>(11); });
    expect_fail("nested message", [](Sample& s) { s.inner.color = static_cast<Color>(9); });
    expect_fail("variable-count array", [](Sample& s) { s.more[0] = static_cast<Color>(4); });

    Sample lenient = good;
    lenient.open = static_cast<Open>(200);  // Open is strict: false
    CHECK(lenient.encode(out));

    Tail t{};
    (void)t.rest.push_back(Color::red);
    (void)t.rest.push_back(static_cast<Color>(7));
    CHECK(t.encode(out).error() == gmp::Error::UnknownEnum);

    Line l{};
    l.color = static_cast<Color>(5);
    l.open = Open::a;
    std::array<std::uint8_t, 16> txt{};
    CHECK(l.encode(txt).error() == gmp::Error::UnknownEnum);
    l.color = Color::blue;
    REQUIRE(l.encode(txt));
}
