#include <catch2/catch_test_macros.hpp>

#include "schema/expr.hpp"

using gmpc::Diagnostics;
using gmpc::expr_to_string;
using gmpc::parse_expr;

TEST_CASE("expression precedence and literals") {
    Diagnostics d;
    auto e = parse_expr("a + 2 * b.c == 0x10 && !(x | 0b11)", {}, d);
    REQUIRE(e);
    CHECK_FALSE(d.has_errors());
    CHECK(expr_to_string(*e) == "(((a + (2 * b.c)) == 16) && !((x | 3)))");

    auto s = parse_expr("n << 2 >> 1", {}, d);
    REQUIRE(s);
    CHECK(expr_to_string(*s) == "((n << 2) >> 1)");
}

TEST_CASE("expression syntax errors are reported") {
    for (const char* bad : {"a +", "(a", "a $ b", "0x", "a..b", ""}) {
        Diagnostics d;
        INFO(bad);
        CHECK_FALSE(parse_expr(bad, {}, d));
        CHECK(d.has_errors());
    }
}
