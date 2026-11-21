#pragma once
// User-supplied extensions referenced from tests/schemas/hdlc.yaml.

#include <gmp/byte_reader.hpp>
#include <gmp/byte_writer.hpp>

namespace test {

struct Time {
    std::uint8_t h = 0, m = 0, s = 0;
    bool operator==(const Time&) const = default;
};

inline gmp::Result<std::uint8_t> from_bcd(std::uint8_t b) {
    if ((b >> 4) > 9 || (b & 0xF) > 9) return gmp::fail(gmp::Error::InvalidValue);
    return static_cast<std::uint8_t>((b >> 4) * 10 + (b & 0xF));
}

/// Three BCD bytes: HH MM SS.
inline gmp::Result<Time> decode_bcd_time(gmp::ByteReader& r) {
    Time t;
    for (std::uint8_t* dst : {&t.h, &t.m, &t.s}) {
        auto b = r.read<std::uint8_t>();
        if (!b) return gmp::fail(b.error());
        auto v = from_bcd(*b);
        if (!v) return gmp::fail(v.error());
        *dst = *v;
    }
    return t;
}

inline gmp::VoidResult encode_bcd_time(gmp::ByteWriter& w, const Time& t) {
    for (std::uint8_t v : {t.h, t.m, t.s}) {
        if (v > 99) return gmp::fail(gmp::Error::OutOfRange);
        if (auto e = w.write<std::uint8_t>(static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10))); !e) return e;
    }
    return {};
}

/// Custom checksum: 16-bit sum of all bytes.
inline std::uint16_t add16(gmp::bytes_view d) {
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
inline gmp::Result<double> decode_coord(std::string_view s) {
    auto v = gmp::text::parse_float<double>(s);
    if (!v) return gmp::fail(v.error());
    const double deg = std::floor(*v / 100.0);
    return deg + (*v - deg * 100.0) / 60.0;
}

inline gmp::VoidResult encode_coord(gmp::text::TextWriter& w, const double& degrees) {
    const double deg = std::floor(degrees);
    const double ddmm = deg * 100.0 + (degrees - deg) * 60.0;
    std::array<char, 32> buf{};
    auto [p, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), ddmm, std::chars_format::fixed, 4);
    if (ec != std::errc{}) return gmp::fail(gmp::Error::InvalidValue);
    return w.raw(std::string_view(buf.data(), static_cast<std::size_t>(p - buf.data())));
}

/// Frame = [n][n bytes]; a zero length byte is treated as garbage.
struct LengthByteFramer {
    static gmp::FrameScan scan(gmp::bytes_view b) {
        if (b.empty()) return gmp::FrameScan::need_more(1);
        if (b[0] == 0) return gmp::FrameScan::skip(1, gmp::Error::BadLength);
        const std::size_t n = 1u + b[0];
        if (b.size() < n) return gmp::FrameScan::need_more(n);
        return gmp::FrameScan::frame(n, n, n);
    }
};

} // namespace test
