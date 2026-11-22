// Cross-check of generated C text messages against generated C++: valid records, every truncation, character
// substitutions (digits, separators, signs, letters, empty tokens) and random text. For every input both languages
// must report the same error; on success every field must match, the view accessors (parsed on demand) must give
// the same values or errors, re-encoding must give the same text, and print must give the same text as gmp::print
// (also for views whose fields fail to parse: `<error: ...>`).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// generated C++
#include "custom.gmp.hpp"
#include "enums.gmp.hpp"
#include "fieldcrc.gmp.hpp"
#include "nmea.gmp.hpp"
#include "print_compare.hpp"
// generated C
#include "custom.gmp.h"
#include "enums.gmp.h"
#include "fieldcrc.gmp.h"
#include "nmea.gmp.h"

namespace {

int code(gmp::Error e) { return static_cast<int>(e); }
std::string_view sv(const char* p, std::size_t n) { return {p, n}; }
std::string_view sv(gmsg_str s) { return {s.data, s.size}; }

template <class T>
bool same(T a, T b) {
    if constexpr (std::is_floating_point_v<T>) return std::isnan(a) ? std::isnan(b) : std::memcmp(&a, &b, sizeof a) == 0;
    else return a == b;
}

/// Compare a C++ view accessor (gmp::Result<T>) with a C accessor (error code + out parameter).
template <class T, class CFn>
void same_access(const gmp::Result<T>& cpp, CFn c) {
    T v{};
    const int e = static_cast<int>(c(&v));
    REQUIRE(e == (cpp ? 0 : code(cpp.error())));
    if (cpp) CHECK(same(*cpp, v));
}

/// Same for an optional field: C++ gmp::Result<std::optional<T>>, C has_<f>() + accessor.
template <class T, class CFn>
void same_optional(const gmp::Result<std::optional<T>>& cpp, bool c_has, CFn c) {
    T v{};
    const int e = static_cast<int>(c(&v));
    if (!cpp) {
        REQUIRE(c_has);
        CHECK(e == code(cpp.error()));
        return;
    }
    REQUIRE(cpp->has_value() == c_has);
    if (*cpp) {
        REQUIRE(e == 0);
        CHECK(same(**cpp, v));
    } else {
        CHECK(e == static_cast<int>(GMSG_ERR_MISSING_FIELD));
    }
}

std::vector<std::string> mutations(const std::vector<std::string>& valid, unsigned seed) {
    std::vector<std::string> out;
    std::mt19937 rng(seed);
    const std::string subst = "0123456789,.-+ ENSWxe";
    for (const auto& v : valid) {
        out.push_back(v);
        for (std::size_t n = 0; n < v.size(); ++n) out.push_back(v.substr(0, n));
        for (std::size_t i = 0; i < v.size(); ++i)
            for (int k = 0; k < 3; ++k) {
                std::string m = v;
                m[i] = subst[rng() % subst.size()];
                out.push_back(m);
            }
        out.push_back(v + ",9");
    }
    for (int k = 0; k < 400; ++k) {
        std::string r;
        for (auto n = rng() % 40; n > 0; --n) r += subst[rng() % subst.size()];
        out.push_back(r);
    }
    return out;
}

template <class T>
std::string enc(const T& m) {
    std::array<std::uint8_t, 256> buf{};
    const auto n = m.encode(buf);
    REQUIRE(n);
    return std::string(reinterpret_cast<const char*>(buf.data()), *n);
}

template <class C>
struct CApi {
    gmsg_error (*decode)(const uint8_t*, size_t, C*);
    gmsg_error (*encode)(const C*, uint8_t*, size_t, size_t*);
    void (*print)(const C*, gmsg_printer*) = nullptr;
};

template <class CV>
struct CViewApi {
    gmsg_error (*from)(const uint8_t*, size_t, CV*);
    void (*print)(const CV*, gmsg_printer*) = nullptr;
};

template <class Cpp, class CppView, class C, class CV, class SameStruct, class SameView>
void cross_check(const std::vector<std::string>& valid, CApi<C> capi, CViewApi<CV> vapi, SameStruct same_struct,
                 SameView same_view, unsigned seed) {
    int ok = 0, bad = 0;
    for (const auto& in : mutations(valid, seed)) {
        INFO("text: \"" << in << "\"");
        const auto* data = reinterpret_cast<const std::uint8_t*>(in.data());
        const auto cpp = Cpp::decode(gmp::bytes_view(data, in.size()));
        C c;
        const auto ce = capi.decode(data, in.size(), &c);
        REQUIRE(static_cast<int>(ce) == (cpp ? 0 : code(cpp.error())));
        if (cpp) {
            ++ok;
            same_struct(*cpp, c);
            print_compare::same(*cpp, capi.print, c);
            std::array<std::uint8_t, 256> a{}, b{};
            const auto n = cpp->encode(a);
            std::size_t cn = 0;
            const auto e = capi.encode(&c, b.data(), b.size(), &cn);
            REQUIRE(static_cast<int>(e) == (n ? 0 : code(n.error())));
            if (n) {
                REQUIRE(cn == *n);
                CHECK(std::memcmp(a.data(), b.data(), cn) == 0);
                if (cn > 0) {
                    const auto small = cpp->encode(gmp::mut_bytes(a.data(), *n - 1));
                    CHECK(static_cast<int>(capi.encode(&c, b.data(), cn - 1, nullptr)) == code(small.error()));
                }
            }
        } else {
            ++bad;
        }
        // views tokenise and always succeed; accessors parse on demand
        const auto cppv = CppView::from(gmp::bytes_view(data, in.size()));
        REQUIRE(cppv);
        CV v;
        REQUIRE(vapi.from(data, in.size(), &v) == GMSG_OK);
        CHECK(v.ntok == cppv->present_tokens());
        same_view(*cppv, v);
        print_compare::same(*cppv, vapi.print, v);
    }
    CHECK(ok > 0);
    CHECK(bad > 0);
}

} // namespace

namespace nm = nav::nmea;

TEST_CASE("generated C text messages match C++: NMEA GGA (floats with precision and width, optional fields)") {
    nm::GGA g;
    g.utc_time = 123519.25;
    g.latitude = 4807.0381;
    g.lat_hemi = 'N';
    g.longitude = 1131.0004;
    g.lon_hemi = 'E';
    g.quality = nm::FixQuality::gps;
    g.satellites = 8;
    g.hdop = 0.9f;
    g.altitude = 545.4;
    g.altitude_unit = 'M';
    nm::GGA g2;
    g2.utc_time = 0.5;
    g2.quality = nm::FixQuality::dgps;
    g2.satellites = 12;
    g2.altitude = -12.25;
    cross_check<nm::GGA, nm::GGAView>(
        {enc(g), enc(g2), "092725.00,4717.11399,N,00833.91590,E,1,08,1.01,499.6,M,48.0,M,,"},
        CApi<nav_nmea_gga>{nav_nmea_gga_decode, nav_nmea_gga_encode, nav_nmea_gga_print}, CViewApi<nav_nmea_gga_view>{nav_nmea_gga_view_from, nav_nmea_gga_view_print},
        [](const nm::GGA& a, const nav_nmea_gga& b) {
            CHECK(same(a.utc_time, b.utc_time));
            REQUIRE(a.latitude.has_value() == b.has_latitude);
            if (a.latitude) CHECK(same(*a.latitude, b.latitude));
            REQUIRE(a.lat_hemi.has_value() == b.has_lat_hemi);
            if (a.lat_hemi) CHECK(*a.lat_hemi == b.lat_hemi);
            REQUIRE(a.longitude.has_value() == b.has_longitude);
            if (a.longitude) CHECK(same(*a.longitude, b.longitude));
            CHECK(std::to_underlying(a.quality) == b.quality);
            CHECK(a.satellites == b.satellites);
            REQUIRE(a.hdop.has_value() == b.has_hdop);
            if (a.hdop) CHECK(same(*a.hdop, b.hdop));
            REQUIRE(a.altitude.has_value() == b.has_altitude);
            if (a.altitude) CHECK(same(*a.altitude, b.altitude));
        },
        [](const nm::GGAView& a, const nav_nmea_gga_view& b) {
            same_access(a.utc_time(), [&](double* o) { return nav_nmea_gga_view_utc_time(&b, o); });
            same_optional(a.latitude(), nav_nmea_gga_view_has_latitude(&b), [&](double* o) { return nav_nmea_gga_view_latitude(&b, o); });
            same_optional(a.lat_hemi(), nav_nmea_gga_view_has_lat_hemi(&b), [&](char* o) { return nav_nmea_gga_view_lat_hemi(&b, o); });
            same_optional(a.hdop(), nav_nmea_gga_view_has_hdop(&b), [&](float* o) { return nav_nmea_gga_view_hdop(&b, o); });
            const auto q = a.quality();
            nav_nmea_fix_quality cq = 0;
            REQUIRE(static_cast<int>(nav_nmea_gga_view_quality(&b, &cq)) == (q ? 0 : code(q.error())));
            if (q) CHECK(std::to_underlying(*q) == cq);
            same_access(a.satellites(), [&](std::uint8_t* o) { return nav_nmea_gga_view_satellites(&b, o); });
            for (std::size_t i = 0; i < 16; ++i) CHECK(a.token(i) == sv(nav_nmea_gga_view_token(&b, i)));
        },
        1);
}

TEST_CASE("generated C text messages match C++: NMEA TXT (strings, zero-padded integers)") {
    nm::TXT t;
    t.total = 1;
    t.number = 1;
    t.severity = 2;
    t.text = "hello world";
    cross_check<nm::TXT, nm::TXTView>(
        {enc(t), "01,01,02,"}, CApi<nav_nmea_txt>{nav_nmea_txt_decode, nav_nmea_txt_encode, nav_nmea_txt_print},
        CViewApi<nav_nmea_txt_view>{nav_nmea_txt_view_from, nav_nmea_txt_view_print},
        [](const nm::TXT& a, const nav_nmea_txt& b) {
            CHECK(a.total == b.total);
            CHECK(a.number == b.number);
            CHECK(a.severity == b.severity);
            CHECK(a.text.view() == sv(b.text, b.text_len));
        },
        [](const nm::TXTView& a, const nav_nmea_txt_view& b) {
            same_access(a.total(), [&](std::uint8_t* o) { return nav_nmea_txt_view_total(&b, o); });
            CHECK(a.text() == sv(nav_nmea_txt_view_text(&b)));
        },
        2);
}

TEST_CASE("generated C text messages match C++: strict enum tokens, conversions, custom text type") {
    test::enums::Line l;
    l.color = test::enums::Color::blue;
    l.open = static_cast<test::enums::Open>(7);  // not strict: any value
    cross_check<test::enums::Line, test::enums::LineView>(
        {enc(l), "2,1"}, CApi<test_enums_line>{test_enums_line_decode, test_enums_line_encode, test_enums_line_print},
        CViewApi<test_enums_line_view>{test_enums_line_view_from, test_enums_line_view_print},
        [](const test::enums::Line& a, const test_enums_line& b) {
            CHECK(std::to_underlying(a.color) == b.color);
            CHECK(std::to_underlying(a.open) == b.open);
        },
        [](const test::enums::LineView& a, const test_enums_line_view& b) {
            const auto c = a.color();
            test_enums_color cc = 0;
            REQUIRE(static_cast<int>(test_enums_line_view_color(&b, &cc)) == (c ? 0 : code(c.error())));
            if (c) CHECK(std::to_underlying(*c) == cc);
        },
        3);

    test::fieldcrc::Reading r;
    r.raw_ = -12345;
    cross_check<test::fieldcrc::Reading, test::fieldcrc::ReadingView>(
        {enc(r), "+77"}, CApi<test_fieldcrc_reading>{test_fieldcrc_reading_decode, test_fieldcrc_reading_encode, test_fieldcrc_reading_print},
        CViewApi<test_fieldcrc_reading_view>{test_fieldcrc_reading_view_from, test_fieldcrc_reading_view_print},
        [](const test::fieldcrc::Reading& a, const test_fieldcrc_reading& b) { CHECK(a.raw_ == b.raw); },
        [](const test::fieldcrc::ReadingView& a, const test_fieldcrc_reading_view& b) {
            same_access(a.raw_(), [&](std::int32_t* o) { return test_fieldcrc_reading_view_raw(&b, o); });
            same_access(a.raw_scaled(), [&](double* o) { return test_fieldcrc_reading_view_raw_scaled(&b, o); });
            same_access(a.raw_millis(), [&](double* o) { return test_fieldcrc_reading_view_raw_millis(&b, o); });
        },
        4);

    ext::Pos p;
    p.lat = 48.1173;
    p.lon = 11.5166;
    cross_check<ext::Pos, ext::PosView>(
        {enc(p), "4807.0380"}, CApi<ext_pos>{ext_pos_decode, ext_pos_encode, ext_pos_print}, CViewApi<ext_pos_view>{ext_pos_view_from, ext_pos_view_print},
        [](const ext::Pos& a, const ext_pos& b) {
            CHECK(same(a.lat, b.lat));
            REQUIRE(a.lon.has_value() == b.has_lon);
            if (a.lon) CHECK(same(*a.lon, b.lon));
        },
        [](const ext::PosView& a, const ext_pos_view& b) {
            same_access(a.lat(), [&](double* o) { return ext_pos_view_lat(&b, o); });
            same_optional(a.lon(), ext_pos_view_has_lon(&b), [&](double* o) { return ext_pos_view_lon(&b, o); });
        },
        5);
}
