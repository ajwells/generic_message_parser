#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <string>
#include <string_view>

#include "nmea.gmp.hpp"

using namespace nav::nmea;
using Catch::Matchers::WithinAbs;

namespace {
constexpr std::string_view kGGA = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
constexpr std::string_view kTXT = "$GPTXT,01,01,02,ANTSTATUS=OK*3B\r\n";
gmp::bytes_view bv(std::string_view s) { return gmp::as_bytes(s.data(), s.size()); }
} // namespace

TEST_CASE("parse a real GGA sentence (lazy view)") {
    auto any = Nmea::parse(bv(kGGA));
    REQUIRE(any);
    REQUIRE(std::holds_alternative<GGAView>(*any));
    const auto& g = std::get<GGAView>(*any);
    CHECK_THAT(*g.utc_time(), WithinAbs(123519.0, 1e-9));
    auto lat = g.latitude();
    REQUIRE(lat);
    REQUIRE(lat->has_value());
    CHECK_THAT(**lat, WithinAbs(4807.038, 1e-9));
    CHECK(**g.lat_hemi() == 'N');
    CHECK(*g.quality() == FixQuality::gps);
    CHECK(*g.satellites() == 8);
    CHECK_THAT(**g.altitude(), WithinAbs(545.4, 1e-9));
    CHECK(g.present_tokens() == 14);
}

TEST_CASE("empty optional fields and struct decode") {
    constexpr std::string_view no_fix = "$GPGGA,000000,,,,,0,00,,,,,,,*66\r\n";
    auto any = Nmea::decode(bv(no_fix));
    REQUIRE(any);
    const auto& g = std::get<GGA>(*any);
    CHECK_FALSE(g.latitude);
    CHECK_FALSE(g.hdop);
    CHECK(g.quality == FixQuality::invalid);
}

TEST_CASE("checksum and unknown message type errors") {
    std::string bad(kGGA);
    bad[10] = '9';
    CHECK(Nmea::parse(bv(bad)).error() == gmp::Error::BadChecksum);
    constexpr std::string_view unknown = "$GPXYZ,1*51\r\n";  // valid checksum, unknown type id
    auto u = Nmea::parse(bv(unknown));
    REQUIRE_FALSE(u);
    CHECK(u.error() == gmp::Error::UnknownType);
    CHECK(gmp::to_string(u.error()) == "unknown message type");
}

TEST_CASE("string field and encode round trip") {
    auto any = Nmea::decode(bv(kTXT));
    REQUIRE(any);
    const auto& t = std::get<TXT>(*any);
    CHECK(t.text == "ANTSTATUS=OK");

    std::array<std::uint8_t, Nmea::max_frame> out{};
    auto n = Nmea::encode(t, out);
    REQUIRE(n);
    CHECK(std::string_view(reinterpret_cast<const char*>(out.data()), *n) == kTXT);
}

TEST_CASE("GGA encode then parse") {
    GGA g;
    g.utc_time = 123519;
    g.latitude = 4807.038;
    g.lat_hemi = 'N';
    g.longitude = 1131.0;
    g.lon_hemi = 'E';
    g.quality = FixQuality::gps;
    g.satellites = 8;
    g.hdop = 0.9f;
    g.altitude = 545.4;
    g.altitude_unit = 'M';
    std::array<std::uint8_t, Nmea::max_frame> out{};
    auto n = Nmea::encode(g, out);
    REQUIRE(n);
    const std::string_view s(reinterpret_cast<const char*>(out.data()), *n);
    CHECK(s.starts_with("$GPGGA,123519.00,4807.0380,N,01131.0000,E,1,08,0.9,545.4,M,,,,*"));
    auto back = Nmea::decode(gmp::bytes_view(out.data(), *n));
    REQUIRE(back);
    CHECK(std::get<GGA>(*back) == g);
}

TEST_CASE("NMEA stream with noise and truncated sentences") {
    std::string stream = "garbage\r\n";
    stream += kGGA;
    stream += "$GPGGA,123";  // truncated by the next '$'
    stream += kTXT;
    stream += kGGA;
    int gga = 0, txt = 0, errors = 0;
    Nmea::Stream<> sp;
    auto h = gmp::overloaded{
        [&](const GGAView&) { ++gga; },
        [&](const TXTView& v) { CHECK(v.text() == "ANTSTATUS=OK"); ++txt; },
        [&](const gmp::FrameError&) { ++errors; },
    };
    for (char c : stream) sp.feed(gmp::as_bytes(&c, 1), h);
    CHECK(gga == 2);
    CHECK(txt == 1);
    CHECK(errors >= 2);
}

TEST_CASE("open + select_message building blocks expose the type id") {
    std::string frame(kGGA);
    auto f = Nmea::open(gmp::as_bytes(frame.data(), frame.size()));
    REQUIRE(f);
    CHECK(f->type_id == "GPGGA");
    auto v = Nmea::select_message(*f);
    REQUIRE(v);
    CHECK(std::holds_alternative<GGAView>(*v));
}
