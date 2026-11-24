// Decode a bit-packed (Intel / lsb_first) CAN payload in place, and build one from values.
#include <cstdio>

#include "can.gmp.hpp"

namespace vc = vehicle::can;

int main() {
    // As received from a CAN controller: 8 data bytes.
    const std::array<std::uint8_t, 8> payload{0xE8, 0xC3, 0x82, 0x00, 0x02, 0x19, 0xEF, 0xBE};

    auto view = vc::EngineDataView::from(payload);
    if (!view) {
        std::printf("invalid frame: %s\n", gmsg::to_string(view.error()).data());
        return 1;
    }
    const auto s = view->signals();
    std::printf("rpm=%.1f gear=%s coolant=%.0fC throttle=%.1f%% counter=%u flags=0x%04X\n", s.rpm_scaled(),
                vc::to_string(s.gear).data(), s.coolant_scaled(), s.throttle_scaled(), s.counter, s.flags);

    vc::EngineData out;
    out.signals.rpm = static_cast<std::uint16_t>(3000 / 0.5);
    out.signals.gear = vc::Gear::drive;
    out.signals.coolant = static_cast<std::uint8_t>(95 + 40);
    std::array<std::uint8_t, 8> frame{};
    if (auto n = out.encode(frame)) {
        std::printf("encoded:");
        for (std::size_t i = 0; i < *n; ++i) std::printf(" %02X", frame[i]);
        std::printf("\n");
    }
    return 0;
}
