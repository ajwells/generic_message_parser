// 0x80 is reserved as the sync byte: every 0x80 / 0x81 inside a frame is sent as 81 00 / 81 01.
// Encode a few frames, show the escaping, then cut one frame short and let the stream parser
// resynchronise on the next 0x80.
#include <cstdio>
#include <vector>

#include "motor_link.gmp.hpp"

namespace {

using Link = motor::MotorLink;
using Frame = std::vector<std::uint8_t>;

template <class M>
Frame encode(const M& msg) {
    std::array<std::uint8_t, Link::max_frame> buf{};
    auto n = Link::encode(msg, buf);
    if (!n) {
        std::printf("encode failed: %s\n", gmp::to_string(n.error()).data());
        return {};
    }
    return Frame(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
}

/// Print a wire frame with escape pairs in brackets.
void print_wire(const char* label, const Frame& f) {
    std::printf("%-10s", label);
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (i > 0 && f[i] == 0x81 && i + 1 < f.size()) {
            std::printf(" [%02X %02X]", f[i], f[i + 1]);
            ++i;
        } else {
            std::printf(" %02X", f[i]);
        }
    }
    std::printf("   (%zu bytes)\n", f.size());
}

} // namespace

int main() {
    // --- encoding: reserved bytes in the payload are escaped ------------------------------------
    motor::SetSpeed cmd;
    cmd.rpm = 1920;  // 0x0780
    cmd.direction = motor::Direction::forward;
    cmd.ramp_ms = 385;  // 0x0181
    const Frame set_speed = encode(cmd);

    motor::Telemetry t;
    t.seq = 128;  // 0x0080
    t.rpm = 1920;
    t.set_current_scaled(3.21);
    t.set_temperature_scaled(38.5);  // 385 = 0x0181
    const Frame telemetry = encode(t);

    motor::Log log;
    log.text = "rpm limit reached";  // `len` is filled in automatically
    const Frame log_frame = encode(log);

    t.seq = 129;
    const Frame telemetry2 = encode(t);

    std::printf("Escaped frames (escape pairs in brackets: [81 00] = 0x80, [81 01] = 0x81):\n");
    print_wire("SetSpeed", set_speed);
    print_wire("Telemetry", telemetry);
    print_wire("Log", log_frame);

    // --- whole-buffer decoding: the frame is unescaped in place, so parse() takes a mutable buffer
    Frame copy = set_speed;
    if (auto v = Link::parse(copy); v && std::holds_alternative<motor::SetSpeedView>(*v)) {
        const auto& s = std::get<motor::SetSpeedView>(*v);
        std::printf("\nparse(): SetSpeed rpm=%u direction=%s ramp=%ums\n", s.rpm(), motor::to_string(s.direction()).data(),
                    s.ramp_ms());
    }

    // --- streaming: a frame cut short, then line noise ------------------------------------------
    std::vector<std::uint8_t> wire;
    wire.insert(wire.end(), telemetry.begin(), telemetry.end());
    wire.insert(wire.end(), log_frame.begin(), log_frame.begin() + 9);  // cut short: the rest never arrives
    wire.insert(wire.end(), telemetry2.begin(), telemetry2.end());
    wire.insert(wire.end(), {0x13, 0x37});  // line noise
    wire.insert(wire.end(), log_frame.begin(), log_frame.end());

    std::printf("\nStream of %zu bytes, fed in 3-byte chunks:\n", wire.size());
    Link::Stream<> parser;
    auto handler = gmp::overloaded{
        [](const motor::TelemetryView& v) {
            std::printf("  Telemetry seq=%u rpm=%u current=%.2fA temperature=%.1fC\n", v.seq(), v.rpm(), v.current_scaled(),
                        v.temperature_scaled());
        },
        [](const motor::LogView& v) {
            std::printf("  Log \"%.*s\"\n", static_cast<int>(v.text().size()), v.text().data());
        },
        [](const gmp::FrameError& e) {
            std::printf("  dropped %zu byte(s): %s\n", e.data.size(), gmp::to_string(e.error).data());
        },
    };
    const gmp::bytes_view all(wire);
    for (std::size_t off = 0; off < all.size(); off += 3)
        parser.feed(all.subspan(off, std::min<std::size_t>(3, all.size() - off)), handler);

    const auto& st = parser.stats();
    std::printf("frames=%u errors=%u skipped=%u\n", static_cast<unsigned>(st.frames), static_cast<unsigned>(st.frame_errors),
                static_cast<unsigned>(st.bytes_skipped));
    return 0;
}
