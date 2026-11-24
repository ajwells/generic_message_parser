#pragma once
// User-supplied codecs referenced from meter.yaml.

#include <cstdio>

#include <gmp/byte_reader.hpp>
#include <gmp/byte_writer.hpp>
#include <gmp/print.hpp>

namespace meter {

struct DateTime {
    int year = 2000, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    bool operator==(const DateTime&) const = default;
};

inline int bcd(std::uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
inline std::uint8_t to_bcd(int v) { return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10)); }

/// YY MM DD hh mm ss, BCD encoded.
inline gmsg::Result<DateTime> decode_bcd(gmsg::ByteReader& r) {
    auto b = r.read_bytes(6);
    if (!b) return gmsg::fail(b.error());
    const auto& d = *b;
    DateTime t{2000 + bcd(d[0]), bcd(d[1]), bcd(d[2]), bcd(d[3]), bcd(d[4]), bcd(d[5])};
    if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31) return gmsg::fail(gmsg::Error::InvalidValue);
    return t;
}

inline gmsg::VoidResult encode_bcd(gmsg::ByteWriter& w, const DateTime& t) {
    for (int v : {t.year - 2000, t.month, t.day, t.hour, t.minute, t.second})
        if (auto e = w.write<std::uint8_t>(to_bcd(v)); !e) return e;
    return {};
}

/// Print hook, found by ADL: 2026-11-20 14:30:05. Without it the field prints as <?>.
inline void gmsg_print(gmsg::Printer& p, const DateTime& t) {
    char b[24];
    const int n = std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute,
                                t.second);
    if (n > 0) p.text(std::string_view(b, static_cast<std::size_t>(n)));
}

/// Custom checksum: modulo-256 sum.
inline std::uint8_t mod256(gmsg::bytes_view d) {
    unsigned s = 0;
    for (auto b : d) s += b;
    return static_cast<std::uint8_t>(s);
}

} // namespace meter
