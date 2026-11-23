// Cross-check of the C text runtime (crosscheck_text_c.c, C11) against C++: float formatting (shortest, fixed and
// general with a digit count) against std::to_chars, the C printer (gmp/c/print.h) against gmp::Printer, float parsing against gmp::text::parse_float (std::from_chars), integer parsing against
// gmp::text::parse_int, and the text writer's tokens against gmp::text::TextWriter.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "crosscheck_c.h"
#include "gmp/print.hpp"
#include "gmp/text.hpp"

namespace {

template <class T>
std::string cpp_format(T v, int precision) {
    std::array<char, 1200> buf{};
    const auto r = precision >= 0 ? std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, precision)
                                  : std::to_chars(buf.data(), buf.data() + buf.size(), v);
    REQUIRE(r.ec == std::errc{});
    return std::string(buf.data(), r.ptr);
}

std::string c_format(double v, int precision) {
    std::array<char, 1200> buf{};
    std::size_t n = 0;
    REQUIRE(gmpct_format_double(v, precision, buf.data(), buf.size(), &n) == 0);
    return std::string(buf.data(), n);
}

std::string c_format(float v, int precision) {
    std::array<char, 1200> buf{};
    std::size_t n = 0;
    REQUIRE(gmpct_format_float(v, precision, buf.data(), buf.size(), &n) == 0);
    return std::string(buf.data(), n);
}

bool same_value(double a, double b) { return std::isnan(a) ? std::isnan(b) && std::signbit(a) == std::signbit(b) : std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }
bool same_value(float a, float b) { return std::isnan(a) ? std::isnan(b) && std::signbit(a) == std::signbit(b) : std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }

int code(gmp::Error e) { return static_cast<int>(e); }

void check_parse(std::string_view s) {
    INFO("text: \"" << std::string(s) << "\"");
    {
        const auto cpp = gmp::text::parse_float<double>(s);
        double v = 0;
        const int e = gmpct_parse_double(s.data(), s.size(), &v);
        REQUIRE(e == (cpp ? 0 : code(cpp.error())));
        if (cpp) CHECK(same_value(v, *cpp));
    }
    {
        const auto cpp = gmp::text::parse_float<float>(s);
        float v = 0;
        const int e = gmpct_parse_float(s.data(), s.size(), &v);
        REQUIRE(e == (cpp ? 0 : code(cpp.error())));
        if (cpp) CHECK(same_value(v, *cpp));
    }
}

double random_double(std::mt19937_64& rng) { return std::bit_cast<double>(rng()); }
float random_float(std::mt19937_64& rng) { return std::bit_cast<float>(static_cast<std::uint32_t>(rng())); }

/// Values that exercise boundaries: powers of two and ten, halfway cases, subnormals, limits.
std::vector<double> special_doubles() {
    std::vector<double> v = {0.0, -0.0, 1.0, -1.0, 0.1, 0.2, 0.3, 0.5, 1.5, 2.5, 0.125, 0.375, 1e-4, 1e-5, 123456.0,
                             1e15, 1e16, 1e17, 1e21, 1e22, 1e23, 9007199254740992.0, 9007199254740993.0,
                             123456789012345680000.0, 5e-324, 2.2250738585072014e-308, 2.2250738585072009e-308,
                             1.7976931348623157e308, 4.35, 0.15, 2.675, 1e-7, 33.3333,
                             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN(), -std::numeric_limits<double>::quiet_NaN()};
    for (int e = -1074; e <= 1023; e += 7) v.push_back(std::ldexp(1.0, e));
    for (int e = -300; e <= 300; e += 3) v.push_back(std::pow(10.0, e));
    return v;
}

} // namespace

TEST_CASE("C float formatting matches std::to_chars (shortest)") {
    for (const double d : special_doubles()) {
        INFO("double " << c_format(d, -1));
        CHECK(c_format(d, -1) == cpp_format(d, -1));
        const auto f = static_cast<float>(d);
        CHECK(c_format(f, -1) == cpp_format(f, -1));
    }
    std::mt19937_64 rng(1);
    for (int i = 0; i < 200000; ++i) {
        const double d = random_double(rng);
        const float f = random_float(rng);
        INFO("bits " << std::bit_cast<std::uint64_t>(d) << " / " << std::bit_cast<std::uint32_t>(f));
        CHECK(c_format(d, -1) == cpp_format(d, -1));
        CHECK(c_format(f, -1) == cpp_format(f, -1));
    }
    // short decimals and integers, as text protocols carry them
    for (int i = 0; i < 100000; ++i) {
        const double d = static_cast<double>(static_cast<std::int64_t>(rng() % 2000000001) - 1000000000) / std::pow(10.0, static_cast<int>(rng() % 9));
        CHECK(c_format(d, -1) == cpp_format(d, -1));
        CHECK(c_format(static_cast<float>(d), -1) == cpp_format(static_cast<float>(d), -1));
    }
}

TEST_CASE("C float formatting matches std::to_chars (fixed precision)") {
    std::mt19937_64 rng(2);
    for (const double d : special_doubles()) {
        if (std::abs(d) > 1e30) continue;
        for (int p = 0; p <= 17; ++p) {
            INFO("value " << c_format(d, -1) << ", precision " << p);
            CHECK(c_format(d, p) == cpp_format(d, p));
        }
    }
    for (int i = 0; i < 100000; ++i) {
        const double d = (static_cast<double>(rng() % 20000001) - 10000000.0) / std::pow(10.0, static_cast<int>(rng() % 12));
        const int p = static_cast<int>(rng() % 12);
        INFO("value " << c_format(d, -1) << ", precision " << p);
        CHECK(c_format(d, p) == cpp_format(d, p));
        const auto f = static_cast<float>(d);
        CHECK(c_format(f, p) == cpp_format(f, p));
    }
    for (int i = 0; i < 20000; ++i) {
        const double d = random_double(rng);
        if (!std::isfinite(d) || std::abs(d) > 1e40) continue;
        const int p = static_cast<int>(rng() % 30);
        CHECK(c_format(d, p) == cpp_format(d, p));
    }
}

namespace {

std::string cpp_general(double v, int digits) {
    std::array<char, 64> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::general, digits);
    REQUIRE(r.ec == std::errc{});
    return std::string(buf.data(), r.ptr);
}

std::string c_general(double v, int digits) {
    std::array<char, 64> buf{};
    std::size_t n = 0;
    REQUIRE(gmpct_format_general(v, digits, buf.data(), buf.size(), &n) == 0);
    return std::string(buf.data(), n);
}

} // namespace

TEST_CASE("C float formatting matches std::to_chars (general, significant digits)") {
    std::mt19937_64 rng(3);
    for (const double d : special_doubles()) {
        for (int p = 1; p <= 20; ++p) {
            INFO("value " << c_format(d, -1) << ", digits " << p);
            CHECK(c_general(d, p) == cpp_general(d, p));
        }
    }
    for (int i = 0; i < 200000; ++i) {
        const double d = random_double(rng);
        const int p = 1 + static_cast<int>(rng() % 20);
        INFO("bits " << std::bit_cast<std::uint64_t>(d) << ", digits " << p);
        CHECK(c_general(d, p) == cpp_general(d, p));
    }
    // scaled sensor values (print's conversions), exact halfway cases, carries into a new digit
    for (int i = 0; i < 200000; ++i) {
        const double d = static_cast<double>(static_cast<std::int64_t>(rng() % 2000001) - 1000000) *
                         std::pow(10.0, static_cast<int>(rng() % 16) - 8);
        const int p = 1 + static_cast<int>(rng() % 12);
        INFO("value " << c_format(d, -1) << ", digits " << p);
        CHECK(c_general(d, p) == cpp_general(d, p));
    }
    for (const double d : {0.5, 1.5, 2.5, 0.125, 0.375, 9.5, 99.5, 999.5, 9.999999, 0.00009999, 1e-5, 99999.5, 1e16, 9.5e-5}) {
        for (int p = 1; p <= 6; ++p) {
            CHECK(c_general(d, p) == cpp_general(d, p));
            CHECK(c_general(-d, p) == cpp_general(-d, p));
        }
    }
}

namespace {

enum class E8 : std::uint8_t { run = 1 };
enum class E16 : std::int16_t {};
enum class E32 : std::uint32_t { big = 0xDEADBEEF };
enum class E64 : std::int64_t {};
std::string_view to_string(E8 e) { return e == E8::run ? "run" : ""; }
std::string_view to_string(E16) { return ""; }
std::string_view to_string(E32 e) { return e == E32::big ? "big" : ""; }
std::string_view to_string(E64) { return ""; }

struct Blk {
    std::int64_t v;
    void print_to(gmp::Printer& p) const noexcept {
        p.begin("Blk");
        p.field("v", v);
        p.end();
    }
};
struct Empty {
    void print_to(gmp::Printer& p) const noexcept {
        p.begin("Empty");
        p.end();
    }
};
struct Group {
    void print_to(gmp::Printer& p) const noexcept {
        p.begin({});
        p.end();
    }
};
struct Custom {
    void print_to(gmp::Printer& p) const noexcept { p.text("12:34:56"); }
};

/// The C++ twin of gmpct_print_scenario (crosscheck_text_c.c).
struct Scenario {
    void print_to(gmp::Printer& p) const noexcept {
        static const std::uint8_t bytes[6] = {0x00, 0x0A, 0x7F, 0x80, 0xFE, 0xFF};
        static const char text[] = "a\"b'c\\\n\r\t\x01\x7F\x80 z";
        p.begin("Scenario");
        p.field("empty", Empty{});
        p.field("group", Group{});
        p.field("flag", true);
        p.field("off", false);
        p.key("chars");
        for (const char c : {'x', '\'', '"', '\n', static_cast<char>(0x01), static_cast<char>(0x7F), static_cast<char>(0xC3)}) p.value(c);
        p.field("text", std::string_view(text, sizeof text - 1));
        p.field("min", std::numeric_limits<std::int64_t>::min());
        p.field("max", std::numeric_limits<std::uint64_t>::max());
        p.field("zero", std::uint64_t{0});
        p.field("e8", E8::run);
        p.field("e16", static_cast<E16>(-2));
        p.field("e32", E32::big);
        p.field("e64", static_cast<E64>(-2));
        p.field("raw", gmp::hex(gmp::bytes_view(bytes, sizeof bytes)));
        p.field("none", gmp::hex(gmp::bytes_view(bytes, 0)));
        p.field("list", std::array<std::int64_t, 5>{-2, -1, 0, 1, 2});
        p.field("nothing", std::array<std::int64_t, 0>{});
        p.field("blocks", std::array<Blk, 3>{Blk{0}, Blk{10}, Blk{20}});
        p.field("no_blocks", std::array<Blk, 0>{});
        p.field("volts", std::uint64_t{12600});
        p.conversion(12.6, "V");
        p.conversion(1.0 / 3.0);
        p.conversion(123456789.0, "mV");
        p.field("temp", 0.9f);
        p.unit("degC");
        p.field("sum", 0.1 + 0.2);
        p.unit("m");
        p.note("computed");
        p.absent("gone");
        p.field("bad", gmp::Result<int>(gmp::fail(gmp::Error::ParseError)));
        p.field("custom", Custom{});
        p.key("nested");
        p.begin("Outer");
        p.field("inner", Blk{-7});
        p.field("last", std::uint64_t{1});
        p.conversion(1e-7, "s");
        p.end();
        p.end();
        p.value(std::uint64_t{5});
        p.note("tail");
    }
};

} // namespace

TEST_CASE("C printer matches gmp::Printer (every value kind, notes, lists, options, truncation)") {
    for (const int style : {0, 1})
        for (const unsigned indent : {0u, 2u, 3u})
            for (const std::size_t items : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{64}})
                for (const std::size_t bytes : {std::size_t{0}, std::size_t{1}, std::size_t{3}})
                    for (const int precision : {0, 1, 3, 9, 17}) {
                        gmp::PrintOptions o;
                        o.style = style ? gmp::PrintStyle::single_line : gmp::PrintStyle::multi_line;
                        o.indent = static_cast<std::uint8_t>(indent);
                        o.max_items = items;
                        o.max_bytes = bytes;
                        o.precision = precision;
                        std::array<char, 4096> a{}, b{};
                        const auto r = gmp::println(Scenario{}, std::span<char>(a), o);
                        REQUIRE(r);
                        std::size_t n = 0;
                        REQUIRE(gmpct_print_scenario(style, indent, items, bytes, precision, b.data(), b.size(), &n) == 0);
                        CHECK(std::string(a.data()) == std::string(b.data()));
                        CHECK(n == *r);
                        // every buffer size: the same cut text and BufferFull
                        if (style == 1 && indent == 2 && items == 2 && bytes == 3 && precision == 9) {
                            for (std::size_t cap = 0; cap <= *r + 1; ++cap) {
                                std::array<char, 4096> x{}, y{};
                                const auto rr = gmp::println(Scenario{}, std::span<char>(x.data(), cap), o);
                                std::size_t nn = 0;
                                const int e = gmpct_print_scenario(style, indent, items, bytes, precision, y.data(), cap, &nn);
                                INFO("capacity " << cap);
                                CHECK(e == (rr ? 0 : code(rr.error())));
                                CHECK(std::string(x.data()) == std::string(y.data()));
                                if (rr) CHECK(nn == *rr);
                            }
                        }
                    }
}

TEST_CASE("C float parsing matches gmp::text::parse_float (std::from_chars)") {
    // fixed strings: syntax corners, infinities, NaNs, limits
    for (const char* s : {"", "+", "-", ".", "+.", "-.", "e5", "1e", "1e+", "1e-", "1.5e3", "1.5E-3", " 1", "1 ", "+-1",
                          "-+1", "++1", "0x10", "1.2.3", "..1", "1..", "inf", "INF", "-inf", "+inf", "infinity", "Infinity",
                          "infin", "infx", "nan", "NaN", "-nan", "nan()", "nan(123abc_)", "nan(", "nan(!)", "nanx", "0",
                          "-0", "0.000", "00012.5000", ".5", "5.", "1e400", "-1e400", "1e-400", "1e-330", "4.9e-324",
                          "2.4703282292062327e-324", "2.4703282292062328e-324", "1.7976931348623157e308",
                          "1.7976931348623159e308", "3.4028235e38", "3.4028236e38", "1.4e-45", "7e-46", "1e-46",
                          "9007199254740993", "9007199254740993.0000000000001", "0.1", "123456789012345678901234567890"}) {
        check_parse(s);
    }
    std::mt19937_64 rng(3);
    // round trips of formatted values
    for (int i = 0; i < 100000; ++i) {
        const double d = random_double(rng);
        check_parse(cpp_format(d, -1));
        check_parse(cpp_format(static_cast<float>(d), -1));
        if (std::isfinite(d) && std::abs(d) < 1e20) check_parse(cpp_format(d, static_cast<int>(rng() % 20)));
    }
    // random decimal strings, including digits beyond any precision and exponents near the limits
    const std::string digits = "0123456789";
    for (int i = 0; i < 200000; ++i) {
        std::string s;
        if (rng() % 8 == 0) s += "+-"[rng() % 2];
        const int before = static_cast<int>(rng() % 25), after = static_cast<int>(rng() % 25);
        for (int k = 0; k < before; ++k) s += digits[rng() % 10];
        if (rng() % 2) s += '.';
        for (int k = 0; k < after; ++k) s += digits[rng() % 10];
        if (rng() % 2) {
            s += "eE"[rng() % 2];
            if (rng() % 2) s += "+-"[rng() % 2];
            s += std::to_string(rng() % 360);
        }
        if (rng() % 50 == 0) s += "x";
        check_parse(s);
    }
    // very long digit strings (more significant digits than any rounding needs)
    for (int i = 0; i < 300; ++i) {
        std::string s = "0.";
        const int n = 700 + static_cast<int>(rng() % 400);
        for (int k = 0; k < n; ++k) s += digits[rng() % 10];
        check_parse(s);
        s = std::string(1, digits[1 + rng() % 9]);
        for (int k = 0; k < n; ++k) s += digits[rng() % 10];
        s += "e-" + std::to_string(rng() % 800);
        check_parse(s);
    }
    // halfway cases between adjacent doubles / floats: exact midpoints written out in full
    for (int i = 0; i < 2000; ++i) {
        const double a = std::abs(random_double(rng));
        if (!std::isfinite(a) || a == 0 || a > 1e300) continue;
        const double b = std::nextafter(a, std::numeric_limits<double>::infinity());
        // midpoint = (a + b) / 2 printed with enough digits: long double is exact enough for this on x86
        const long double mid = (static_cast<long double>(a) + static_cast<long double>(b)) / 2;
        char buf[128];
        std::snprintf(buf, sizeof buf, "%.40Le", mid);
        check_parse(buf);
    }
}

TEST_CASE("C integer parsing matches gmp::text::parse_int") {
    std::mt19937_64 rng(4);
    const std::string chars = "0123456789abcdefABCDEF+- xz";
    for (int i = 0; i < 200000; ++i) {
        std::string s;
        const int n = static_cast<int>(rng() % 24);
        for (int k = 0; k < n; ++k) s += rng() % 3 ? chars[rng() % 10] : chars[rng() % chars.size()];
        for (const int base : {10, 16, 2, 8}) {
            INFO("\"" << s << "\" base " << base);
            auto check_i = [&]<class T>(T) {
                const auto cpp = gmp::text::parse_int<T>(s, base);
                std::int64_t v = 0;
                const int e = gmpct_parse_i64(s.data(), s.size(), base, std::numeric_limits<T>::min(), std::numeric_limits<T>::max(), &v);
                REQUIRE(e == (cpp ? 0 : code(cpp.error())));
                if (cpp) CHECK(v == static_cast<std::int64_t>(*cpp));
            };
            auto check_u = [&]<class T>(T) {
                const auto cpp = gmp::text::parse_int<T>(s, base);
                std::uint64_t v = 0;
                const int e = gmpct_parse_u64(s.data(), s.size(), base, std::numeric_limits<T>::max(), &v);
                REQUIRE(e == (cpp ? 0 : code(cpp.error())));
                if (cpp) CHECK(v == static_cast<std::uint64_t>(*cpp));
            };
            check_i(std::int8_t{});
            check_i(std::int16_t{});
            check_i(std::int32_t{});
            check_i(std::int64_t{});
            check_u(std::uint8_t{});
            check_u(std::uint16_t{});
            check_u(std::uint32_t{});
            check_u(std::uint64_t{});
        }
    }
}

TEST_CASE("C and C++ text writers write huge fixed-notation floats (no internal size limit)") {
    std::mt19937_64 rng(6);
    int long_tokens = 0;
    for (int i = 0; i < 3000; ++i) {
        const double dv = std::ldexp(static_cast<double>(rng() >> 11) / 9007199254740992.0 + 0.5, static_cast<int>(rng() % 1100) - 50) *
                          (rng() & 1u ? -1.0 : 1.0);
        const auto fv = static_cast<float>(dv);
        const int precision = static_cast<int>(rng() % 40);
        const int width = static_cast<int>(rng() % 400);
        for (int which = 2; which < 4; ++which) {
            std::array<std::uint8_t, 1024> a{};
            std::array<char, 1024> b{};
            gmp::text::TextWriter w(gmp::mut_bytes(a.data(), a.size()), ',');
            const auto r = which == 2 ? w.float_token(dv, precision, width) : w.float_token(fv, precision, width);
            REQUIRE(r);  // fits the buffer: must not fail
            const std::size_t len = w.position();
            if (len > 128) ++long_tokens;
            std::size_t n = 0;
            REQUIRE(gmpct_text_tokens(which, 0, 0, dv, fv, 10, precision, width, b.data(), b.size(), &n) == 0);
            // C writes the extra 'x' token after the float: compare the float part
            REQUIRE(n == len + 2);
            CHECK(std::string_view(b.data(), len) == std::string_view(reinterpret_cast<const char*>(a.data()), len));
            // one byte short: both report BufferFull
            gmp::text::TextWriter small(gmp::mut_bytes(a.data(), len - 1), ',');
            const auto rs = which == 2 ? small.float_token(dv, precision, width) : small.float_token(fv, precision, width);
            REQUIRE(!rs);
            CHECK(rs.error() == gmp::Error::BufferFull);
            CHECK(gmpct_text_tokens(which, 0, 0, dv, fv, 10, precision, width, b.data(), len - 1, &n) == code(gmp::Error::BufferFull));
        }
    }
    CHECK(long_tokens > 1000);
}

TEST_CASE("C text writer tokens match gmp::text::TextWriter") {
    std::mt19937_64 rng(5);
    for (int i = 0; i < 50000; ++i) {
        const auto iv = static_cast<std::int64_t>(rng()) >> (rng() % 64);
        const std::uint64_t uv = rng() >> (rng() % 64);
        const double dv = (static_cast<double>(rng() % 2000001) - 1000000.0) / std::pow(10.0, static_cast<int>(rng() % 6));
        const auto fv = static_cast<float>(dv);
        const int base = std::array{10, 16, 2}[rng() % 3];
        const int precision = static_cast<int>(rng() % 6) - 1;
        const int width = static_cast<int>(rng() % 12);
        for (int which = 0; which < 4; ++which) {
            const std::size_t cap = rng() % 4 == 0 ? rng() % 12 : 64;  // sometimes too small
            std::array<std::uint8_t, 64> a{};
            std::array<char, 64> b{};
            gmp::text::TextWriter w(gmp::mut_bytes(a.data(), cap), ',');
            gmp::VoidResult r;
            switch (which) {
                case 0: r = w.int_token(iv, base, width); break;
                case 1: r = w.int_token(uv, base, width); break;
                case 2: r = w.float_token(dv, precision, width); break;
                default: r = w.float_token(fv, precision, width); break;
            }
            if (r) r = w.token('x');
            std::size_t n = 0;
            const int e = gmpct_text_tokens(which, iv, uv, dv, fv, base, precision, width, b.data(), cap, &n);
            INFO("which " << which << ", cap " << cap << ", width " << width << ", precision " << precision);
            REQUIRE(e == (r ? 0 : code(r.error())));
            if (r) {
                REQUIRE(n == w.position());
                CHECK(std::string_view(b.data(), n) == std::string_view(reinterpret_cast<const char*>(a.data()), n));
            }
        }
    }
}
