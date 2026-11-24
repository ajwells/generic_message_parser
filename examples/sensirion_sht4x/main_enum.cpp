// SHT4x with commands as an enum (sht4x_enum.yaml): build a command sequence as data, encode each command,
// pick the response decoder from the command, and decode bytes written by someone else back to a command.
// The I2C transfers are simulated with captured bytes; replace them with your bus driver.
#include <array>
#include <cstdio>
#include <span>

#include "sht4x_enum.gmp.hpp"

namespace sht = sensirion::sht4x_enum;
using sht::Command;

namespace {

/// What to read back after a command, derived from the enum (one switch instead of one type per command).
enum class Reply { none, measurement, serial };

constexpr Reply reply_for(Command c) {
    switch (c) {
        case Command::soft_reset: return Reply::none;
        case Command::read_serial_number: return Reply::serial;
        default: return Reply::measurement;  // measure_* and heater_* commands
    }
}

/// Simulated sensor: the 6 bytes it would return for a command.
std::array<std::uint8_t, 6> simulated_read(Command c) {
    if (c == Command::read_serial_number) return {0x0F, 0x4F, 0x0B, 0x5A, 0x3C, 0x30};
    return {0x66, 0x66, 0x93, 0x7A, 0xE1, 0xA4};  // 25.00 degC, 54.00 %RH
}

}  // namespace

int main() {
    // A command sequence is plain data.
    constexpr std::array sequence{Command::soft_reset, Command::read_serial_number, Command::measure_high_precision,
                                  Command::heater_20mw_100ms};

    for (Command c : sequence) {
        std::array<std::uint8_t, 1> tx{};
        if (!sht::CommandFrame{c}.encode(tx)) return 1;
        std::printf("write %02X  %-24s", tx[0], sht::to_string(c).data());

        switch (reply_for(c)) {
            case Reply::none: std::printf("(no response)\n"); break;
            case Reply::serial: {
                const auto rx = simulated_read(c);
                auto s = sht::SerialNumberView::from(rx);
                if (!s) { std::printf("bad response: %s\n", gmsg::to_string(s.error()).data()); return 1; }
                std::printf("serial = 0x%08X\n", static_cast<unsigned>(s->serial()));  // computed value
                break;
            }
            case Reply::measurement: {
                const auto rx = simulated_read(c);
                auto m = sht::MeasurementResponse::decode(rx);
                if (!m) { std::printf("bad response: %s\n", gmsg::to_string(m.error()).data()); return 1; }
                const auto& meas = std::get<sht::Measurement>(*m);
                std::printf("T = %.2f degC (%.2f degF), RH = %.2f %%RH\n", meas.temperature_scaled(),
                            meas.temperature_degf(), meas.humidity_scaled());
                break;
            }
        }
    }

    // Decoding a command byte (e.g. in a bus sniffer). Command is `strict: true` in the schema, so a byte
    // that is not a known command fails with UnknownEnum instead of producing an unnamed enum value.
    for (std::uint8_t b : {std::uint8_t{0xE0}, std::uint8_t{0x42}}) {
        const std::array<std::uint8_t, 1> rx{b};
        auto f = sht::CommandFrame::decode(rx);
        if (f) std::printf("sniffed %02X -> %s\n", b, sht::to_string(f->command).data());
        else std::printf("sniffed %02X -> error: %s\n", b, gmsg::to_string(f.error()).data());
    }
    return 0;
}
