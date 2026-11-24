// Compares the text of the C printer (gmp/c/print.h) with gmsg::print for the same value, under several print options
// and with a small char buffer (truncation). Used by the generated-C cross-checks.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <gmp/c/runtime.h>
#include <gmp/print.hpp>

namespace print_compare {

struct Case {
    gmsg::PrintOptions cpp;
    gmsg_print_options c;
};

inline std::vector<Case> cases() {
    std::vector<Case> out;
    auto add = [&](gmsg::PrintStyle style, std::uint8_t indent, std::size_t items, std::size_t bytes, int precision) {
        Case k;
        k.cpp.style = style;
        k.cpp.indent = indent;
        k.cpp.max_items = items;
        k.cpp.max_bytes = bytes;
        k.cpp.precision = precision;
        k.c = gmsg_print_defaults();
        k.c.style = style == gmsg::PrintStyle::multi_line ? GMSG_PRINT_MULTI_LINE : GMSG_PRINT_SINGLE_LINE;
        k.c.indent = indent;
        k.c.max_items = items;
        k.c.max_bytes = bytes;
        k.c.precision = precision;
        out.push_back(k);
    };
    add(gmsg::PrintStyle::multi_line, 2, 64, 64, 9);   // defaults
    add(gmsg::PrintStyle::single_line, 2, 64, 64, 9);
    add(gmsg::PrintStyle::multi_line, 4, 2, 3, 3);     // cut arrays and bytes, 3 significant digits
    add(gmsg::PrintStyle::single_line, 0, 0, 0, 0);    // everything, shortest conversions
    add(gmsg::PrintStyle::multi_line, 1, 1, 1, 17);
    return out;
}

inline bool append(void* ctx, const char* s, std::size_t n) {
    static_cast<std::string*>(ctx)->append(s, n);
    return true;
}

/// C text of `v` printed by `print` with options `o`.
template <class C>
std::string c_text(void (*print)(const C*, gmsg_printer*), const C& v, const gmsg_print_options& o) {
    std::string s;
    gmsg_printer p;
    gmsg_printer_init(&p, append, &s, &o);
    print(&v, &p);
    REQUIRE(gmsg_printer_finish(&p, false, nullptr) == GMSG_OK);
    return s;
}

/// gmsg::print(cpp) and the C print of `c` give the same text for every option set, and the same truncated text and
/// error with a small buffer.
template <class Cpp, class C>
void same(const Cpp& cpp, void (*print)(const C*, gmsg_printer*), const C& c) {
    if (!print) return;
    static const auto all = cases();
    for (const auto& k : all) {
        std::string a;
        REQUIRE(gmsg::print(cpp, a, k.cpp));
        const auto b = c_text(print, c, k.c);
        CHECK(a == b);
        if (a != b) return;
    }
    // bounded buffer: same cut text, BufferFull from both
    char ca[23], cb[23];
    const auto r = gmsg::println(cpp, ca);
    gmsg_printer p;
    gmsg_printer_init_buffer(&p, cb, sizeof cb, nullptr);
    print(&c, &p);
    std::size_t n = 0;
    const auto e = gmsg_printer_finish(&p, true, &n);
    CHECK(std::string(ca) == std::string(cb));
    CHECK(static_cast<int>(e) == (r ? 0 : static_cast<int>(r.error())));
    if (r) CHECK(n == *r);
}

} // namespace print_compare
