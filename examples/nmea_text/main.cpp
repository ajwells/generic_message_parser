// Parse NMEA 0183 sentences (text protocol, XOR checksum, optional fields) and encode one.
#include <cstdio>
#include <string_view>

#include "nmea.gmp.hpp"

namespace nm = nav::nmea;

int main() {
    constexpr std::string_view log =
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"
        "$GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n"  // bad checksum
        "$GPTXT,01,01,02,ANTSTATUS=OK*3B\r\n"
        "$GPGGA,000000,,,,,0,00,,,,,,,*66\r\n";

    nm::Nmea::Stream<> parser;
    parser.feed(gmsg::as_bytes(log.data(), log.size()),
                gmsg::overloaded{
                    [](const nm::GGAView& g) {
                        // Text views parse lazily: each accessor returns gmsg::Result<T>.
                        std::printf("GGA time=%.0f quality=%s sats=%u", g.utc_time().value_or(0.0),
                                    nm::to_string(g.quality().value_or(nm::FixQuality::invalid)).data(),
                                    static_cast<unsigned>(g.satellites().value_or(0)));
                        auto lat = g.latitude();  // Result<optional<double>>: optional field
                        if (lat && *lat) std::printf(" lat=%.3f", **lat);
                        else std::printf(" (no fix)");
                        std::printf("\n");
                    },
                    [](const nm::TXTView& t) {
                        std::printf("TXT \"%.*s\"\n", static_cast<int>(t.text().size()), t.text().data());
                    },
                    [](const gmsg::FrameError& e) { std::printf("rejected: %s\n", gmsg::to_string(e.error).data()); },
                });

    nm::TXT txt;
    txt.total = 1;
    txt.number = 1;
    txt.severity = 2;
    txt.text = "HELLO";
    std::array<std::uint8_t, nm::Nmea::max_frame> out{};
    if (auto n = nm::Nmea::encode(txt, out))
        std::printf("encoded: %.*s", static_cast<int>(*n), reinterpret_cast<const char*>(out.data()));
    return 0;
}
