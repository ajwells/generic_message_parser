// Encode a few frames, add line noise, and recover the messages with the stream parser.
#include <cstdio>
#include <vector>

#include "telemetry.gmp.hpp"

int main() {
    std::vector<std::uint8_t> wire;
    auto append = [&](const auto& msg) {
        std::array<std::uint8_t, demo::Link::max_frame> buf{};
        if (auto n = demo::Link::encode(msg, buf)) wire.insert(wire.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    };

    demo::Status ok;
    ok.uptime_s = 3600;
    ok.set_voltage_scaled(12.6);
    ok.state.armed = true;
    ok.state.mode = demo::Mode::run;
    append(ok);

    wire.insert(wire.end(), {0x13, 0x37, 0xAA});  // line noise

    demo::Samples s;
    s.channel = 3;
    s.values = {100, -100, 250};  // `count` is filled in automatically
    append(s);

    demo::Status bad = ok;
    bad.state.mode = demo::Mode::fault;
    bad.fault = demo::Fault{0x0042, 875};
    append(bad);

    demo::Link::Stream<> parser;
    auto handler = gmp::overloaded{
        [](const demo::StatusView& v) {
            std::printf("Status  uptime=%us voltage=%.3fV mode=%s", static_cast<unsigned>(v.uptime_s()), v.voltage_scaled(),
                        demo::to_string(v.state().mode).data());
            if (auto f = v.fault()) std::printf(" fault=0x%04X temp=%.1fC", f->code(), f->temperature_scaled());
            std::printf("\n");
        },
        [](const demo::SamplesView& v) {
            // gmp::println dumps every field (enum names, units, nested messages); see README "Printing"
            gmp::println(v, stdout, {.style = gmp::PrintStyle::single_line});
        },
        [](const gmp::FrameError& e) {
            std::printf("dropped %zu byte(s): %s\n", e.data.size(), gmp::to_string(e.error).data());
        },
    };
    // Deliver the stream in awkward 5-byte chunks, as a UART driver might.
    const gmp::bytes_view all(wire);
    for (std::size_t off = 0; off < all.size(); off += 5)
        parser.feed(all.subspan(off, std::min<std::size_t>(5, all.size() - off)), handler);

    const auto& st = parser.stats();
    std::printf("frames=%u errors=%u skipped=%u\n", static_cast<unsigned>(st.frames),
                static_cast<unsigned>(st.frame_errors), static_cast<unsigned>(st.bytes_skipped));
    return 0;
}
