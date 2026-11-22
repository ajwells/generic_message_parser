// js_vectors: reference results from the C++ runtime (and the C runtime's run-time CRC) for the JavaScript runtime
// cross-check. Prints one JSON object; tests/runtime_js/test_runtime.js runs it and compares runtime-js/gmsg.js.
//
// Doubles and floats are passed as their bit patterns (hex), 64-bit integers as decimal strings.

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "gmp/bits.hpp"
#include "gmp/c/checksum.h"
#include "gmp/checksum.hpp"
#include "gmp/framing/delimited.hpp"
#include "gmp/framing/fixed.hpp"
#include "gmp/framing/length_prefixed.hpp"
#include "gmp/print.hpp"
#include "gmp/stream_parser.hpp"
#include "gmp/text.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

// ---- a tiny JSON writer ---------------------------------------------------------------------------------
std::string out;
bool first_item = true;
void raw(std::string_view s) { out += s; }
void sep() {
    if (!first_item) out += ',';
    first_item = false;
}
void str(std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20 || c >= 0x7f) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
        } else out += static_cast<char>(c);
    }
    out += '"';
}
void num(std::uint64_t v) { out += std::to_string(v); }
void bytes(const Bytes& b) {
    out += '"';
    static const char* hex = "0123456789abcdef";
    for (auto x : b) { out += hex[x >> 4]; out += hex[x & 15]; }
    out += '"';
}
void hex64(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "\"%016llx\"", static_cast<unsigned long long>(v));
    out += b;
}
void begin_array(std::string_view key) {
    sep();
    str(key);
    raw(":[");
    first_item = true;
}
void end_array() {
    raw("]");
    first_item = false;
}

Bytes random_bytes(std::mt19937& rng, std::size_t n) {
    Bytes b(n);
    for (auto& x : b) x = static_cast<std::uint8_t>(rng());
    return b;
}

// ---- checksums -------------------------------------------------------------------------------------------
template <class A>
void checksum_cases(std::mt19937& rng, std::string_view name) {
    for (int i = 0; i < 25; ++i) {
        const auto b = random_bytes(rng, rng() % 40);
        sep();
        raw("[");
        str(name);
        raw(",");
        bytes(b);
        raw(",\"");
        num(static_cast<std::uint64_t>(A::compute(gmp::bytes_view(b.data(), b.size()))));
        raw("\"]");
    }
}

void checksums(std::mt19937& rng) {
    begin_array("checksums");
    checksum_cases<gmp::checksum::crc8>(rng, "crc8");
    checksum_cases<gmp::checksum::crc8_maxim>(rng, "crc8_maxim");
    checksum_cases<gmp::checksum::crc8_autosar>(rng, "crc8_autosar");
    checksum_cases<gmp::checksum::crc8_nrsc5>(rng, "crc8_nrsc5");
    checksum_cases<gmp::checksum::crc15_can>(rng, "crc15_can");
    checksum_cases<gmp::checksum::crc16_ccitt>(rng, "crc16_ccitt");
    checksum_cases<gmp::checksum::crc16_xmodem>(rng, "crc16_xmodem");
    checksum_cases<gmp::checksum::crc16_kermit>(rng, "crc16_kermit");
    checksum_cases<gmp::checksum::crc16_x25>(rng, "crc16_x25");
    checksum_cases<gmp::checksum::crc16_modbus>(rng, "crc16_modbus");
    checksum_cases<gmp::checksum::crc16_arc>(rng, "crc16_arc");
    checksum_cases<gmp::checksum::crc24_openpgp>(rng, "crc24_openpgp");
    checksum_cases<gmp::checksum::crc32>(rng, "crc32");
    checksum_cases<gmp::checksum::crc32c>(rng, "crc32c");
    checksum_cases<gmp::checksum::crc32_mpeg2>(rng, "crc32_mpeg2");
    checksum_cases<gmp::checksum::crc64_ecma>(rng, "crc64_ecma");
    checksum_cases<gmp::checksum::xor8>(rng, "xor8");
    checksum_cases<gmp::checksum::sum8>(rng, "sum8");
    checksum_cases<gmp::checksum::lrc8>(rng, "lrc8");
    checksum_cases<gmp::checksum::sum16>(rng, "sum16");
    checksum_cases<gmp::checksum::fletcher16>(rng, "fletcher16");
    checksum_cases<gmp::checksum::fletcher8_ubx>(rng, "fletcher8_ubx");
    end_array();

    // generic CRCs with random parameters (the C runtime's run-time CRC, checked against C++ elsewhere)
    begin_array("crc_generic");
    for (int i = 0; i < 400; ++i) {
        gmsg_crc_params p{};
        p.width = static_cast<unsigned>(1 + rng() % 64);
        const std::uint64_t mask = p.width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << p.width) - 1;
        const std::uint64_t r64 = (std::uint64_t{rng()} << 32) | rng();
        p.poly = (r64 | 1u) & mask;
        p.init = ((std::uint64_t{rng()} << 32) | rng()) & mask;
        p.xorout = (rng() % 3 == 0) ? 0 : (((std::uint64_t{rng()} << 32) | rng()) & mask);
        p.refin = rng() & 1u;
        p.refout = rng() & 1u;
        const auto b = random_bytes(rng, rng() % 30);
        sep();
        raw("[");
        num(p.width);
        raw(",\"");
        num(p.poly);
        raw("\",\"");
        num(p.init);
        raw("\",");
        raw(p.refin ? "true" : "false");
        raw(",");
        raw(p.refout ? "true" : "false");
        raw(",\"");
        num(p.xorout);
        raw("\",");
        bytes(b);
        raw(",\"");
        num(gmsg_crc_compute(&p, b.data(), b.size()));
        raw("\"]");
    }
    end_array();
}

// ---- bits ------------------------------------------------------------------------------------------------
void bits(std::mt19937& rng) {
    begin_array("bits");
    for (int i = 0; i < 3000; ++i) {
        const auto b = random_bytes(rng, 9);
        const unsigned n = 1 + rng() % 64;
        const std::size_t off = rng() % (72 - n + 1);
        const auto order = (rng() & 1u) ? gmp::BitOrder::lsb_first : gmp::BitOrder::msb_first;
        const std::uint64_t v = gmp::extract_bits(b.data(), off, n, order);
        const std::int64_t sv = gmp::sign_extend(v, n);
        const std::uint64_t value = (std::uint64_t{rng()} << 32) | rng();
        Bytes w = b;
        gmp::set_bits<std::uint64_t>(w.data(), off, n, value, order);
        sep();
        raw("[");
        bytes(b);
        raw(",");
        num(off);
        raw(",");
        num(n);
        raw(order == gmp::BitOrder::lsb_first ? ",\"lsb_first\",\"" : ",\"msb_first\",\"");
        num(v);
        raw("\",\"");
        out += std::to_string(sv);
        raw("\",\"");
        num(value);
        raw("\",");
        bytes(w);
        raw("]");
    }
    end_array();
}

// ---- float text --------------------------------------------------------------------------------------------
double random_double(std::mt19937& rng) {
    switch (rng() % 6) {
        case 0: return std::bit_cast<double>((std::uint64_t{rng()} << 32) | rng());
        case 1: return static_cast<double>(static_cast<std::int32_t>(rng())) / 1000.0;
        case 2: return static_cast<double>(rng() % 100000) * std::pow(10.0, static_cast<int>(rng() % 40) - 20);
        case 3: return static_cast<double>((std::uint64_t{rng()} << 32) | rng());
        case 4: return static_cast<double>(static_cast<std::int64_t>(rng() % 2000001) - 1000000) / 8.0;
        default: return std::ldexp(static_cast<double>(rng() % 4096), static_cast<int>(rng() % 200) - 100);
    }
}
float random_float(std::mt19937& rng) {
    switch (rng() % 4) {
        case 0: return std::bit_cast<float>(static_cast<std::uint32_t>(rng()));
        case 1: return static_cast<float>(static_cast<std::int32_t>(rng() % 2000001) - 1000000) / 100.0f;
        case 2: return static_cast<float>(rng());
        default: return static_cast<float>(random_double(rng));
    }
}

template <class F>
std::string to_chars_text(F v, int mode, int precision) {
    char buf[1200];
    std::to_chars_result r{};
    if (mode == 0) r = std::to_chars(buf, buf + sizeof buf, v);
    else if (mode == 1) r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, precision);
    else r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::general, precision);
    if (r.ec != std::errc{}) return "<error>";
    return std::string(buf, r.ptr);
}

std::string random_number_text(std::mt19937& rng) {
    static const char* pieces[] = {"0", "1", "5", "9", "00", "123", "999999999", "7", "-", "+", ".", "e", "E", "e-",
                                   "e+", "inf", "nan", "x", " ", "e308", "e-320", "e39", "e-46", "25", "0000000000"};
    std::string s;
    const int parts = 1 + static_cast<int>(rng() % 5);
    for (int i = 0; i < parts; ++i) s += pieces[rng() % (sizeof pieces / sizeof *pieces)];
    if (rng() % 3 == 0) s = to_chars_text(random_double(rng), 0, 0);
    if (rng() % 5 == 0) s = to_chars_text(random_float(rng), 0, 0);
    if (rng() % 7 == 0) s = to_chars_text(random_double(rng), 2, 1 + static_cast<int>(rng() % 25));
    return s;
}

void floats(std::mt19937& rng) {
    begin_array("float_format");  // [kind f32|f64, bits, mode 0 shortest | 1 fixed | 2 general, precision, text]
    for (int i = 0; i < 6000; ++i) {
        const bool f32 = rng() & 1u;
        const int mode = static_cast<int>(rng() % 3);
        const int precision = mode == 1 ? static_cast<int>(rng() % 25) : 1 + static_cast<int>(rng() % 17);
        std::string text;
        std::uint64_t bits = 0;
        if (f32) {
            const float v = random_float(rng);
            bits = std::bit_cast<std::uint32_t>(v);
            text = mode == 2 ? to_chars_text(static_cast<double>(v), 2, precision) : to_chars_text(v, mode, precision);
        } else {
            const double v = random_double(rng);
            bits = std::bit_cast<std::uint64_t>(v);
            text = to_chars_text(v, mode, precision);
        }
        if (text == "<error>") continue;
        sep();
        raw(f32 ? "[\"f32\"," : "[\"f64\",");
        hex64(bits);
        raw(",");
        num(static_cast<unsigned>(mode));
        raw(",");
        num(static_cast<unsigned>(precision));
        raw(",");
        str(text);
        raw("]");
    }
    end_array();

    begin_array("float_parse");  // [text, f64 error, f64 bits, f32 error, f32 bits]
    for (int i = 0; i < 6000; ++i) {
        const std::string s = random_number_text(rng);
        const auto d = gmp::text::parse_float<double>(s);
        const auto f = gmp::text::parse_float<float>(s);
        sep();
        raw("[");
        str(s);
        raw(",");
        num(d ? 0u : static_cast<unsigned>(d.error()));
        raw(",");
        hex64(d ? std::bit_cast<std::uint64_t>(*d) : 0);
        raw(",");
        num(f ? 0u : static_cast<unsigned>(f.error()));
        raw(",");
        hex64(f ? std::bit_cast<std::uint32_t>(*f) : 0);
        raw("]");
    }
    end_array();
}

// ---- integer text ------------------------------------------------------------------------------------------
template <class T>
void int_parse_case(std::string_view type, std::string_view s, int base) {
    const auto r = gmp::text::parse_int<T>(s, base);
    sep();
    raw("[");
    str(type);
    raw(",");
    str(s);
    raw(",");
    num(static_cast<unsigned>(base));
    raw(",");
    num(r ? 0u : static_cast<unsigned>(r.error()));
    raw(",\"");
    out += r ? std::to_string(*r) : std::string("0");
    raw("\"]");
}

void ints(std::mt19937& rng) {
    static const char* pieces[] = {"0", "1", "9", "a", "F", "ff", "7f", "80", "-", "+", "255", "256", "65535", "65536",
                                   "-128", "-129", "2147483648", "4294967296", "9223372036854775807",
                                   "18446744073709551616", "x", " ", "00", "12", "z"};
    begin_array("int_parse");
    for (int i = 0; i < 4000; ++i) {
        std::string s;
        const int parts = 1 + static_cast<int>(rng() % 3);
        for (int k = 0; k < parts; ++k) s += pieces[rng() % (sizeof pieces / sizeof *pieces)];
        if (rng() % 8 == 0) s.clear();
        const int base = (rng() % 3 == 0) ? 16 : 10;
        switch (rng() % 8) {
            case 0: int_parse_case<std::uint8_t>("u8", s, base); break;
            case 1: int_parse_case<std::uint16_t>("u16", s, base); break;
            case 2: int_parse_case<std::uint32_t>("u32", s, base); break;
            case 3: int_parse_case<std::uint64_t>("u64", s, base); break;
            case 4: int_parse_case<std::int8_t>("i8", s, base); break;
            case 5: int_parse_case<std::int16_t>("i16", s, base); break;
            case 6: int_parse_case<std::int32_t>("i32", s, base); break;
            default: int_parse_case<std::int64_t>("i64", s, base); break;
        }
    }
    end_array();

    begin_array("text_writer");  // [kind, value, base | precision, width, upper, text]
    for (int i = 0; i < 2000; ++i) {
        std::array<std::uint8_t, 256> buf{};
        gmp::text::TextWriter w(gmp::mut_bytes(buf.data(), buf.size()), ',');
        const auto mark = out.size();
        const bool was_first = first_item;
        bool ok = true;
        sep();
        if (rng() & 1u) {
            const std::int64_t v = (rng() & 1u) ? static_cast<std::int32_t>(rng()) : static_cast<std::int64_t>((std::uint64_t{rng()} << 32) | rng());
            const int base = (rng() & 1u) ? 16 : 10;
            const int width = static_cast<int>(rng() % 12);
            const bool upper = rng() & 1u;
            ok = w.int_token(v, base, width, upper).has_value();
            raw("[\"int\",\"");
            out += std::to_string(v);
            raw("\",");
            num(static_cast<unsigned>(base));
            raw(",");
            num(static_cast<unsigned>(width));
            raw(upper ? ",true," : ",false,");
        } else {
            const bool f32 = rng() & 1u;
            const int precision = static_cast<int>(rng() % 8) - 1;
            const int width = static_cast<int>(rng() % 12);
            std::uint64_t bits;
            if (f32) {
                const float v = random_float(rng);
                bits = std::bit_cast<std::uint32_t>(v);
                ok = w.float_token(v, precision, width).has_value();
            } else {
                const double v = random_double(rng);
                bits = std::bit_cast<std::uint64_t>(v);
                ok = w.float_token(v, precision, width).has_value();
            }
            raw(f32 ? "[\"f32\"," : "[\"f64\",");
            hex64(bits);
            raw(",");
            out += std::to_string(precision);
            raw(",");
            num(static_cast<unsigned>(width));
            raw(",false,");
        }
        // C++ formats a float token in a 128-character buffer and fails beyond it (C and JS print the exact text)
        if (!ok) {
            out.resize(mark);
            first_item = was_first;
            continue;
        }
        str(std::string_view(reinterpret_cast<const char*>(buf.data()), w.position()));
        raw("]");
    }
    end_array();
}

// ---- framing (the same configs as tests/runtime_c/test_framing_c_vs_cpp.cpp) -------------------------------
const gmp::EscapeConfig& escape_cfg(int which) {
    static const gmp::EscapeConfig xor_cfg = gmp::make_xor_escape(0x7D, 0x20, {0x7E});
    static const gmp::EscapeConfig index_cfg = gmp::make_index_escape(0x81, {0x80, 0x81});
    static const gmp::EscapeConfig map_cfg = gmp::make_map_escape(0xDB, {0xC0, 0xDB}, {0xDC, 0xDD});
    return which == 0 ? xor_cfg : which == 1 ? index_cfg : map_cfg;
}

constexpr int kScanConfigs = 9;
gmp::FrameScan cpp_scan(int which, gmp::bytes_view b) {
    const gmp::Pattern aa55{{0xAA, 0x55}, 2}, flag{{0x7E}, 1}, dollar{{0x24}, 1}, crlf{{0x0D, 0x0A}, 2}, x80{{0x80}, 1};
    switch (which) {
        case 0: return gmp::scan_fixed(b, gmp::FixedFrameConfig{aa55, 6});
        case 1: return gmp::scan_fixed(b, gmp::FixedFrameConfig{gmp::Pattern{}, 4});
        case 2: return gmp::scan_fixed(b, gmp::FixedFrameConfig{x80, 5, escape_cfg(1), 0});
        case 3: return gmp::scan_length_prefixed(b, gmp::LengthPrefixedConfig{aa55, 2, 1, gmp::Endian::big, 5, 5, 24});
        case 4: return gmp::scan_length_prefixed(b, gmp::LengthPrefixedConfig{gmp::Pattern{}, 0, 2, gmp::Endian::little, -1, 3, 20});
        case 5: return gmp::scan_length_prefixed(b, gmp::LengthPrefixedConfig{flag, 1, 1, gmp::Endian::big, 4, 4, 32, escape_cfg(0), 0});
        case 6:
            return gmp::scan_length_prefixed(
                b, gmp::LengthPrefixedConfig{flag, 1, 1, gmp::Endian::big, 0, 4, 32, escape_cfg(0), 0, true, 2, 1, false});
        case 7: return gmp::scan_delimited(b, gmp::DelimitedConfig{flag, flag, escape_cfg(0), 24});
        default: return gmp::scan_delimited(b, gmp::DelimitedConfig{dollar, crlf, gmp::EscapeConfig{}, 16});
    }
}

Bytes structured(std::mt19937& rng, std::size_t n) {
    static const std::uint8_t special[] = {0xAA, 0x55, 0x7E, 0x7D, 0x80, 0x81, 0xC0, 0xDB, 0xDC, 0xDD, 0x24, 0x0D, 0x0A, 0x5E, 0x5D};
    static const std::vector<Bytes> tokens = {{0xAA, 0x55}, {0x0D, 0x0A}, {0x24}, {0x7E}, {0x7D, 0x5E}, {0x80}, {0x81, 0x01}};
    Bytes b;
    while (b.size() < n) {
        const auto r = rng() % 20;
        if (r < 4) {
            const auto& t = tokens[rng() % tokens.size()];
            b.insert(b.end(), t.begin(), t.end());
        } else if (r < 10) {
            b.push_back(static_cast<std::uint8_t>(rng() % 8));
        } else if (r < 16) {
            b.push_back(special[rng() % sizeof special]);
        } else {
            b.push_back(static_cast<std::uint8_t>(rng()));
        }
    }
    b.resize(n);
    return b;
}

void scans(std::mt19937& rng) {
    begin_array("scan");  // [config, bytes, kind, size, consume, consume_on_error, error]
    for (int round = 0; round < 1500; ++round) {
        const auto b = structured(rng, rng() % 40);
        for (int which = 0; which < kScanConfigs; ++which) {
            const auto s = cpp_scan(which, gmp::bytes_view(b.data(), b.size()));
            sep();
            raw("[");
            num(static_cast<unsigned>(which));
            raw(",");
            bytes(b);
            raw(",");
            num(static_cast<unsigned>(s.kind));
            raw(",");
            num(s.size);
            raw(",");
            num(s.consume);
            raw(",");
            num(s.consume_on_error);
            raw(",");
            num(static_cast<unsigned>(s.error));
            raw("]");
        }
    }
    end_array();

    begin_array("escape");  // [config, bytes, unescape error, unescaped, escaped (2 bytes prefix), walk count, pick_at, walk kind, wire, error, pick]
    for (int round = 0; round < 1500; ++round) {
        const auto b = structured(rng, rng() % 24);
        for (int which = 0; which < 3; ++which) {
            const auto& cfg = escape_cfg(which);
            auto x = b;
            const auto r = gmp::unescape_in_place(gmp::mut_bytes(x.data(), x.size()), cfg);
            Bytes ex(b.size() * 2 + 2, 0xEE);
            std::copy(b.begin(), b.end(), ex.begin() + 2);
            const auto er = gmp::escape_in_place(gmp::mut_bytes(ex.data(), ex.size()), 2, b.size(), cfg);
            ex.resize(2 + *er);
            const std::size_t count = rng() % 12, pick_at = rng() % 4;
            std::array<std::uint8_t, 4> px{};
            const auto w = gmp::walk_escaped(gmp::bytes_view(b.data(), b.size()), 0, count, cfg, pick_at,
                                             gmp::mut_bytes(px.data(), px.size()));
            sep();
            raw("[");
            num(static_cast<unsigned>(which));
            raw(",");
            bytes(b);
            raw(",");
            num(r ? 0u : static_cast<unsigned>(r.error()));
            raw(",");
            bytes(r ? Bytes(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(*r)) : Bytes{});
            raw(",");
            bytes(ex);
            raw(",");
            num(count);
            raw(",");
            num(pick_at);
            raw(",");
            num(static_cast<unsigned>(w.kind));
            raw(",");
            num(w.wire);
            raw(",");
            num(static_cast<unsigned>(w.error));
            raw(",");
            bytes(w.kind == gmp::EscapeWalk::Kind::done ? Bytes(px.begin(), px.end()) : Bytes{});
            raw("]");
        }
    }
    end_array();
}

// ---- stream -------------------------------------------------------------------------------------------------
int g_cfg = 0;
struct Recorder {
    std::string* ev;
    void operator()(gmp::bytes_view frame) const { *ev += "F" + std::to_string(frame.size()) + " "; }
    void operator()(const gmp::FrameError& e) const {
        *ev += "E" + std::to_string(static_cast<int>(e.error)) + ":" + std::to_string(e.data.size()) + " ";
    }
};
struct TestProtocol {
    static gmp::FrameScan scan(gmp::bytes_view b) noexcept { return cpp_scan(g_cfg, b); }
    template <class H>
    static gmp::Error process(gmp::mut_bytes frame, H& h) {
        if (frame.empty() || (frame.back() & 1u) != 0) return gmp::Error::BadChecksum;
        h(gmp::bytes_view(frame));
        return gmp::Error::None;
    }
};

template <std::size_t Cap>
void stream_case(int which, const Bytes& data, std::size_t chunk) {
    g_cfg = which;
    std::string ev;
    gmp::StreamParser<TestProtocol, Cap> p;
    for (std::size_t pos = 0; pos < data.size(); pos += chunk)
        (void)p.feed(gmp::bytes_view(data.data() + pos, std::min(chunk, data.size() - pos)), Recorder{&ev});
    sep();
    raw("[");
    num(static_cast<unsigned>(which));
    raw(",");
    num(Cap);
    raw(",");
    num(chunk);
    raw(",");
    bytes(data);
    raw(",");
    str(ev);
    raw(",[");
    num(p.stats().frames);
    raw(",");
    num(p.stats().frame_errors);
    raw(",");
    num(p.stats().bytes_skipped);
    raw(",");
    num(p.stats().overflows);
    raw("]]");
}

void streams(std::mt19937& rng) {
    begin_array("stream");  // [config, capacity, chunk, bytes, events, stats]
    for (int round = 0; round < 40; ++round) {
        const auto data = structured(rng, 50 + rng() % 400);
        for (int which = 0; which < kScanConfigs; ++which) {
            for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, data.size()}) {
                stream_case<32>(which, data, chunk);
                stream_case<256>(which, data, chunk);
            }
        }
    }
    end_array();
}

// ---- printer: one scripted scenario under many options (the same script is in test_runtime.js) ------------------
enum class Mode : std::uint8_t { idle = 0, run = 1 };
constexpr std::string_view to_string(Mode m) { return m == Mode::idle ? "idle" : m == Mode::run ? "run" : ""; }

struct Inner {
    int id;
    void print_to(gmp::Printer& p) const {
        p.begin("Inner");
        p.field("id", id);
        p.end();
    }
};
struct Empty {
    void print_to(gmp::Printer& p) const {
        p.begin("Empty");
        p.end();
    }
};

struct Scenario {
    void print_to(gmp::Printer& p) const {
        p.begin("Scenario");
        p.field("u", 4000000000u);
        p.field("i", -12);
        p.key("big");
        p.value(std::uint64_t{18446744073709551615ull});
        p.key("f32");
        p.value(0.9f);
        p.key("f64");
        p.value(1e21);
        p.key("tiny");
        p.value(5e-324);
        p.field("flag", true);
        p.field("ch", '\'');
        p.field("ch2", '\x07');
        p.field("mode", Mode::run);
        p.field("bad_mode", static_cast<Mode>(7));
        p.key("raw");
        p.value(std::uint16_t{12600});
        p.unit("mV");
        p.key("scaled");
        p.value(std::uint16_t{12600});
        p.conversion(12.6, "V");
        p.conversion(1234567885.0, "x");
        p.conversion(0.000012345678901, "");
        p.key("bytes");
        const std::uint8_t b[] = {0, 1, 0xAB, 0xFF, 0x10};
        p.value(gmp::hex(gmp::bytes_view(b, 5)));
        p.key("text");
        p.value(std::string_view("a\"b\\c\n\x01"));
        p.absent("maybe");
        p.key("ints");
        const std::array<int, 5> ints{1, 2, 3, 4, 5};
        p.list(ints);
        p.key("empty_list");
        p.list(std::array<int, 0>{});
        p.key("nested");
        const std::array<Inner, 3> inner{Inner{1}, Inner{2}, Inner{3}};
        p.list(inner);
        p.key("empty");
        Empty{}.print_to(p);
        p.key("bits");
        p.begin("");
        p.field("a", 1);
        p.field("b", false);
        p.end();
        p.computed("total", 42, "ms");
        p.key("err");
        p.value(gmp::Result<int>(gmp::fail(gmp::Error::ParseError)));
        p.end();
    }
};

void printer() {
    begin_array("print");  // [style, indent, max_items, max_bytes, precision, text]
    for (int style = 0; style < 2; ++style)
        for (unsigned indent : {0u, 2u, 4u})
            for (std::size_t items : {std::size_t{0}, std::size_t{2}, std::size_t{64}})
                for (std::size_t mb : {std::size_t{0}, std::size_t{3}, std::size_t{64}})
                    for (int prec : {1, 6, 9, 17}) {
                        gmp::PrintOptions o{style ? gmp::PrintStyle::single_line : gmp::PrintStyle::multi_line,
                                            static_cast<std::uint8_t>(indent), items, mb, prec};
                        std::string text;
                        (void)gmp::print(Scenario{}, text, o);
                        sep();
                        raw(style ? "[\"single_line\"," : "[\"multi_line\",");
                        num(indent);
                        raw(",");
                        num(items);
                        raw(",");
                        num(mb);
                        raw(",");
                        num(static_cast<unsigned>(prec));
                        raw(",");
                        str(text);
                        raw("]");
                    }
    end_array();
}

} // namespace

int main() {
    std::mt19937 rng(20261122);
    raw("{");
    first_item = true;
    checksums(rng);
    bits(rng);
    floats(rng);
    ints(rng);
    scans(rng);
    streams(rng);
    printer();
    raw("}\n");
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
