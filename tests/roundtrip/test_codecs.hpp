#pragma once
// User-supplied extensions referenced from tests/schemas/hdlc.yaml.

#include <gmp/byte_reader.hpp>
#include <gmp/byte_writer.hpp>
#include <gmp/print.hpp>

namespace test {

struct Time {
    std::uint8_t h = 0, m = 0, s = 0;
    bool operator==(const Time&) const = default;
};

inline gmsg::Result<std::uint8_t> from_bcd(std::uint8_t b) {
    if ((b >> 4) > 9 || (b & 0xF) > 9) return gmsg::fail(gmsg::Error::InvalidValue);
    return static_cast<std::uint8_t>((b >> 4) * 10 + (b & 0xF));
}

/// Three BCD bytes: HH MM SS.
inline gmsg::Result<Time> decode_bcd_time(gmsg::ByteReader& r) {
    Time t;
    for (std::uint8_t* dst : {&t.h, &t.m, &t.s}) {
        auto b = r.read<std::uint8_t>();
        if (!b) return gmsg::fail(b.error());
        auto v = from_bcd(*b);
        if (!v) return gmsg::fail(v.error());
        *dst = *v;
    }
    return t;
}

inline gmsg::VoidResult encode_bcd_time(gmsg::ByteWriter& w, const Time& t) {
    for (std::uint8_t v : {t.h, t.m, t.s}) {
        if (v > 99) return gmsg::fail(gmsg::Error::OutOfRange);
        if (auto e = w.write<std::uint8_t>(static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10))); !e) return e;
    }
    return {};
}

/// Pretty-print hook for gmsg::print (found by ADL): HH:MM:SS.
inline void gmsg_print(gmsg::Printer& p, const Time& t) noexcept {
    const char b[8] = {static_cast<char>('0' + t.h / 10 % 10), static_cast<char>('0' + t.h % 10), ':',
                       static_cast<char>('0' + t.m / 10 % 10), static_cast<char>('0' + t.m % 10), ':',
                       static_cast<char>('0' + t.s / 10 % 10), static_cast<char>('0' + t.s % 10)};
    p.text(std::string_view(b, sizeof b));
}

/// Custom checksum: 16-bit sum of all bytes.
inline std::uint16_t add16(gmsg::bytes_view d) {
    std::uint16_t s = 0;
    for (auto b : d) s = static_cast<std::uint16_t>(s + b);
    return s;
}

} // namespace test

#include <cmath>
#include <gmp/framing/frame.hpp>
#include <gmp/text.hpp>

namespace test {

/// NMEA "ddmm.mmmm" -> decimal degrees.
inline gmsg::Result<double> decode_coord(std::string_view s) {
    auto v = gmsg::text::parse_float<double>(s);
    if (!v) return gmsg::fail(v.error());
    const double deg = std::floor(*v / 100.0);
    return deg + (*v - deg * 100.0) / 60.0;
}

inline gmsg::VoidResult encode_coord(gmsg::text::TextWriter& w, const double& degrees) {
    const double deg = std::floor(degrees);
    const double ddmm = deg * 100.0 + (degrees - deg) * 60.0;
    std::array<char, 32> buf{};
    auto [p, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), ddmm, std::chars_format::fixed, 4);
    if (ec != std::errc{}) return gmsg::fail(gmsg::Error::InvalidValue);
    return w.raw(std::string_view(buf.data(), static_cast<std::size_t>(p - buf.data())));
}

/// Frame = [n][n bytes]; a zero length byte is treated as garbage.
struct LengthByteFramer {
    static gmsg::FrameScan scan(gmsg::bytes_view b) {
        if (b.empty()) return gmsg::FrameScan::need_more(1);
        if (b[0] == 0) return gmsg::FrameScan::skip(1, gmsg::Error::BadLength);
        const std::size_t n = 1u + b[0];
        if (b.size() < n) return gmsg::FrameScan::need_more(n);
        return gmsg::FrameScan::frame(n, n, n);
    }
};

} // namespace test
