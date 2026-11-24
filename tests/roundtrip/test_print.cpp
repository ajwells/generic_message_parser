// Pretty printing (gmp/print.hpp + generated print_to): layout, enums, sinks, views, text messages, computed values.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "computed.gmp.hpp"
#include "hdlc.gmp.hpp"
#include "nmea.gmp.hpp"
#include "telemetry.gmp.hpp"

namespace tel = acme::telemetry;

namespace {

tel::Status make_status(tel::Mode mode) {
    tel::Status s;
    s.voltage = 1234;
    s.flags.armed = true;
    s.flags.mode = mode;
    s.flags.level = -3;
    s.count = 3;
    s.samples = {10, -20, 30};
    if (mode == tel::Mode::fault) s.ext = tel::Extended{-125, 101325};
    s.label = "abc";
    return s;
}

template <class T>
std::string to_text(const T& v, const gmsg::PrintOptions& o = {}) {
    std::string s;
    REQUIRE(gmsg::print(v, s, o));
    return s;
}

constexpr gmsg::PrintOptions kOneLine{.style = gmsg::PrintStyle::single_line};

} // namespace

TEST_CASE("print: multi-line struct with enum names, units, nested and absent fields") {
    CHECK(to_text(make_status(tel::Mode::run)) ==
          "Status {\n"
          "  voltage: 1234 (12.34 V)\n"
          "  flags: {\n"
          "    armed: true\n"
          "    mode: run (0x01)\n"
          "    level: -3\n"
          "  }\n"
          "  count: 3\n"
          "  samples: [10, -20, 30]\n"
          "  ext: <absent>\n"
          "  label: \"abc\"\n"
          "}");
    CHECK(to_text(make_status(tel::Mode::fault)) ==
          "Status {\n"
          "  voltage: 1234 (12.34 V)\n"
          "  flags: {\n"
          "    armed: true\n"
          "    mode: fault (0x02)\n"
          "    level: -3\n"
          "  }\n"
          "  count: 3\n"
          "  samples: [10, -20, 30]\n"
          "  ext: Extended {\n"
          "    temperature: -125 (-12.5 degC)\n"
          "    pressure: 101325\n"
          "  }\n"
          "  label: \"abc\"\n"
          "}");
}

TEST_CASE("print: single line") {
    CHECK(to_text(make_status(tel::Mode::fault), kOneLine) ==
          "Status { voltage: 1234 (12.34 V), flags: { armed: true, mode: fault (0x02), level: -3 }, count: 3, "
          "samples: [10, -20, 30], ext: Extended { temperature: -125 (-12.5 degC), pressure: 101325 }, label: \"abc\" }");
}

TEST_CASE("print: view prints exactly like the struct") {
    for (auto mode : {tel::Mode::run, tel::Mode::fault}) {
        const auto s = make_status(mode);
        std::array<std::uint8_t, 64> buf{};
        auto n = s.encode(buf);
        REQUIRE(n);
        auto v = tel::StatusView::from(gmsg::bytes_view(buf.data(), *n));
        REQUIRE(v);
        CHECK(to_text(*v) == to_text(s));
        CHECK(to_text(*v, kOneLine) == to_text(s, kOneLine));
    }
}

TEST_CASE("print: enums show name and hex value, unknown values stay visible") {
    CHECK(to_text(tel::Mode::idle) == "idle (0x00)");
    CHECK(to_text(static_cast<tel::Mode>(7)) == "<unknown> (0x07)");
    // floats print as the shortest text of their own type (not widened to double first)
    CHECK(to_text(0.9f) == "0.9");
    CHECK(to_text(0.1) == "0.1");
    CHECK(to_text('\n') == "'\\n'");
    auto s = make_status(tel::Mode::run);
    s.flags.mode = static_cast<tel::Mode>(5);
    CHECK(to_text(s).find("    mode: <unknown> (0x05)\n") != std::string::npos);
}

TEST_CASE("print: bytes as hex, strings escaped, limits") {
    tel::Command c;
    c.opcode = 0x0102;
    c.args = {0x00, 0x7F, 0xAB};
    CHECK(to_text(c, kOneLine) == "Command { opcode: 258, arg_len: 0, args: [00 7F AB] }");

    tel::Log l;
    l.level = 2;
    l.text = "a\"b\r\n";
    l.tail = {1, 2, 3, 4, 5};
    gmsg::PrintOptions o = kOneLine;
    o.max_bytes = 2;
    CHECK(to_text(l, o) == "Log { level: 2, text: \"a\\\"b\\r\\n\", tail: [01 02 ... (+3 more)] }");

    auto s = make_status(tel::Mode::run);
    s.samples = {1, 2, 3, 4};
    o.max_items = 3;
    CHECK(to_text(s, o).find("samples: [1, 2, 3, ... (+1 more)]") != std::string::npos);
}

TEST_CASE("print: protocol parse result (variant of views)") {
    const auto s = make_status(tel::Mode::run);
    std::array<std::uint8_t, tel::BinaryLink::max_frame> buf{};
    auto n = tel::BinaryLink::encode(s, buf);
    REQUIRE(n);
    auto any = tel::BinaryLink::parse(gmsg::bytes_view(buf.data(), *n));
    REQUIRE(any);
    CHECK(to_text(*any) == to_text(s));
}

TEST_CASE("print: sinks") {
    const auto s = make_status(tel::Mode::run);
    const std::string expected = to_text(s, kOneLine);

    SECTION("char buffer, NUL terminated, truncation reported") {
        char big[512];
        auto n = gmsg::print(s, big, kOneLine);
        REQUIRE(n);
        CHECK(std::string(big) == expected);
        CHECK(*n == expected.size());

        char small[16];
        auto t = gmsg::print(s, small, kOneLine);
        REQUIRE_FALSE(t);
        CHECK(t.error() == gmsg::Error::BufferFull);
        CHECK(std::string(small) == expected.substr(0, 15));

        std::array<char, 512> arr{};
        REQUIRE(gmsg::print(s, std::span<char>(arr), kOneLine));
        CHECK(std::string(arr.data()) == expected);
    }
    SECTION("static_string appends and reports truncation") {
        gmsg::static_string<512> str = "rx: ";
        REQUIRE(gmsg::println(s, str, kOneLine));
        CHECK(str.view() == "rx: " + expected + "\n");

        gmsg::static_string<10> tiny;
        auto t = gmsg::print(s, tiny, kOneLine);
        REQUIRE_FALSE(t);
        CHECK(t.error() == gmsg::Error::BufferFull);
        CHECK(tiny.view() == expected.substr(0, 10));
    }
    SECTION("std::ostream") {
        std::ostringstream os;
        REQUIRE(gmsg::print(s, os, kOneLine));
        CHECK(os.str() == expected);
    }
    SECTION("callback, which can stop the output") {
        std::string got;
        REQUIRE(gmsg::print(s, [&](std::string_view t) { got += t; }, kOneLine));
        CHECK(got == expected);

        std::string limited;
        auto t = gmsg::print(s, [&](std::string_view x) { limited += x; return limited.size() < 8; }, kOneLine);
        REQUIRE_FALSE(t);
        CHECK(t.error() == gmsg::Error::BufferFull);
    }
    SECTION("FILE*") {
        std::FILE* f = std::tmpfile();
        REQUIRE(f);
        auto n = gmsg::println(s, f, kOneLine);
        REQUIRE(n);
        CHECK(*n == expected.size() + 1);
        std::rewind(f);
        std::vector<char> back(expected.size() + 1);
        CHECK(std::fread(back.data(), 1, back.size(), f) == back.size());
        CHECK(std::string(back.data(), back.size()) == expected + "\n");
        std::fclose(f);
    }
}

TEST_CASE("print: text messages, struct and view") {
    nav::nmea::GGA g;
    g.utc_time = 123519.0;
    g.latitude = 4807.038;
    g.lat_hemi = 'N';
    g.quality = nav::nmea::FixQuality::gps;
    g.satellites = 8;
    CHECK(to_text(g, kOneLine) ==
          "GGA { utc_time: 123519, latitude: 4807.038, lat_hemi: 'N', longitude: <absent>, lon_hemi: <absent>, "
          "quality: gps (0x01), satellites: 8, hdop: <absent>, altitude: <absent>, altitude_unit: <absent> }");

    // the view parses each field on access (the message type token is not part of the message)
    auto vv = nav::nmea::GGAView::from(std::string_view("123519.00,4807.0380,N,,,1,08,,,,,,,"));
    REQUIRE(vv);
    CHECK(to_text(*vv, kOneLine) == to_text(g, kOneLine));

    auto bad = nav::nmea::GGAView::from(std::string_view("12x,,,,,1,08"));
    REQUIRE(bad);
    CHECK(to_text(*bad, kOneLine).find("utc_time: <error: parse error>") != std::string::npos);
}

TEST_CASE("print: computed values") {
    test::computed::Packed p;
    p.b0 = 1;
    p.b1 = 2;
    p.b2 = 3;
    p.unit = test::computed::Unit::cm;
    const auto s = to_text(p);
    CHECK(s.find("  unit: cm (0x01)\n") != std::string::npos);
    CHECK(s.find("  rgb: 66051 color (computed)\n") != std::string::npos);
    CHECK(s.find("  sum: 0 (computed)\n") != std::string::npos);
}

TEST_CASE("print: arrays of messages one per line, custom types through gmsg_print") {
    link::hdlc::Batch b;
    b.when = test::Time{12, 34, 56};
    b.n = 2;
    for (std::uint8_t i = 0; i < 2; ++i) {
        link::hdlc::Reading r;
        r.channel = i;
        r.name = i ? "temp" : "p";
        r.name_len = static_cast<std::uint8_t>(r.name.size());
        r.value = 1.5f * static_cast<float>(i + 1);
        (void)b.readings.push_back(r);
    }
    const std::string expected =
        "Batch {\n"
        "  when: 12:34:56\n"
        "  n: 2\n"
        "  readings: [\n"
        "    Reading {\n"
        "      channel: 0\n"
        "      name_len: 1\n"
        "      name: \"p\"\n"
        "      value: 1.5\n"
        "    }\n"
        "    Reading {\n"
        "      channel: 1\n"
        "      name_len: 4\n"
        "      name: \"temp\"\n"
        "      value: 3\n"
        "    }\n"
        "  ]\n"
        "  crc_in_payload: <absent>\n"
        "}";
    CHECK(to_text(b) == expected);
    CHECK(to_text(b, kOneLine) ==
          "Batch { when: 12:34:56, n: 2, readings: [Reading { channel: 0, name_len: 1, name: \"p\", value: 1.5 }, "
          "Reading { channel: 1, name_len: 4, name: \"temp\", value: 3 }], crc_in_payload: <absent> }");

    std::array<std::uint8_t, 128> buf{};
    auto n = b.encode(buf);
    REQUIRE(n);
    auto v = link::hdlc::BatchView::from(gmsg::bytes_view(buf.data(), *n));
    REQUIRE(v);
    CHECK(to_text(*v) == expected);

    gmsg::PrintOptions o;
    o.max_items = 1;
    CHECK(to_text(b, o).find("    }\n    ... (+1 more)\n  ]\n") != std::string::npos);
}
