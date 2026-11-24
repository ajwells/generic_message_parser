// A custom field decoder and a custom checksum, wired in from the schema.
#include <cstdio>

#include "meter.gmp.hpp"

int main() {
    meter::Reading r;
    r.timestamp = meter::DateTime{2026, 11, 20, 14, 30, 5};
    r.set_kwh_scaled(12345.67);

    std::array<std::uint8_t, meter::MeterBus::max_frame> frame{};
    auto n = meter::MeterBus::encode(r, frame);
    if (!n) return 1;
    std::printf("frame:");
    for (std::size_t i = 0; i < *n; ++i) std::printf(" %02X", frame[i]);
    std::printf("\n");

    auto any = meter::MeterBus::parse(gmsg::bytes_view(frame.data(), *n));
    if (!any) {
        std::printf("parse failed: %s\n", gmsg::to_string(any.error()).data());
        return 1;
    }
    const auto& v = std::get<meter::ReadingView>(*any);
    const auto ts = v.timestamp();  // custom-type accessors return gmsg::Result<T>
    if (!ts) return 1;
    std::printf("%04d-%02d-%02d %02d:%02d:%02d  %.2f kWh\n", ts->year, ts->month, ts->day, ts->hour, ts->minute,
                ts->second, v.kwh_scaled());
    return 0;
}
