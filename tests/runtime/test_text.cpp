#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

#include "gmp/text.hpp"

using namespace gmp;
using namespace gmp::text;

TEST_CASE("tokenizer keeps empty tokens") {
    Tokenizer t("a,,b,", ',');
    CHECK(*t.next() == "a");
    CHECK(*t.next() == "");
    CHECK(*t.next() == "b");
    CHECK(*t.next() == "");
    CHECK_FALSE(t.next());

    std::array<std::string_view, 3> tok{};
    CHECK(split("x,y", ',', tok) == 2);
    CHECK(tok[0] == "x");
    CHECK(tok[1] == "y");
    CHECK(tok[2].empty());
}

TEST_CASE("numeric parsing") {
    CHECK(*parse_int<int>("-42") == -42);
    CHECK(*parse_int<int>("+7") == 7);
    CHECK(*parse_int<std::uint8_t>("ff", 16) == 255);
    CHECK(parse_int<std::uint8_t>("256").error() == Error::OutOfRange);
    CHECK(parse_int<int>("12a").error() == Error::ParseError);
    CHECK(parse_int<int>("").error() == Error::MissingField);
    CHECK(*parse_float<double>("4807.038") == 4807.038);
    CHECK(*parse_float<float>("-0.5") == -0.5f);
    CHECK(*parse_hex_byte("4F") == 0x4F);
    CHECK(*parse_char("N") == 'N');
}

TEST_CASE("text writer formatting") {
    std::array<std::uint8_t, 64> buf{};
    TextWriter w(buf, ',');
    REQUIRE(w.token("GPGGA"));
    REQUIRE(w.float_token(123519.0, 0, 6));
    REQUIRE(w.float_token(4807.038, 3, 8));
    REQUIRE(w.token('N'));
    REQUIRE(w.int_token(8, 10, 2));
    REQUIRE(w.empty_token());
    REQUIRE(w.int_token(0x4F, 16, 2));
    std::string_view out(reinterpret_cast<const char*>(buf.data()), w.position());
    CHECK(out == "GPGGA,123519,4807.038,N,08,,4F");
}

TEST_CASE("text writer overflow") {
    std::array<std::uint8_t, 4> buf{};
    TextWriter w(buf, ',');
    REQUIRE(w.token("abc"));
    CHECK_FALSE(w.token("d"));
}
