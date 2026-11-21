// Talk to a Sensirion SHT4x over I2C: encode commands, decode the 6-byte responses (with per-word CRC-8).
// The I2C transfers are simulated with captured bytes; replace i2c_write / i2c_read with your bus driver.
#include <array>
#include <cstdio>

#include "sht4x.gmp.hpp"

namespace sht = sensirion::sht4x;

namespace {

void print_bytes(const char* what, gmp::bytes_view b) {
    std::printf("%-8s", what);
    for (auto x : b) std::printf(" %02X", x);
    std::printf("\n");
}

}  // namespace

int main() {
    std::array<std::uint8_t, 6> tx{};

    // 1. Command: measure T & RH with high precision (write 1 byte to 0x44, then wait 8.3 ms).
    const auto n = sht::MeasureHighPrecision{}.encode(tx);
    print_bytes("write", gmp::bytes_view(tx.data(), *n));

    // 2. Response: 6 bytes read back = T word, CRC, RH word, CRC.
    const std::array<std::uint8_t, 6> rx{0x66, 0x66, 0x93, 0x7A, 0xE1, 0xA4};
    print_bytes("read", rx);
    auto m = sht::MeasurementResponse::decode(rx);
    if (!m) {
        std::printf("bad response: %s\n", gmp::to_string(m.error()).data());
        return 1;
    }
    const auto& meas = std::get<sht::Measurement>(*m);
    std::printf("T = %.2f degC (%.2f degF), RH = %.2f %%RH\n", meas.temperature_scaled(), meas.temperature_degf(),
                meas.humidity_scaled());

    // A corrupted byte is caught by the word CRC.
    auto bad = rx;
    bad[1] ^= 0x01;
    std::printf("corrupted: %s\n", gmp::to_string(sht::MeasurementResponse::parse(bad).error()).data());

    // 3. Serial number (command 0x89, then read 6 bytes), using the zero-copy view.
    const std::array<std::uint8_t, 6> serial_rx{0x0F, 0x4F, 0x0B, 0x5A, 0x3C, 0x30};
    if (auto s = sht::SerialNumberView::from(serial_rx)) {
        // serial() is a computed value: (serial_high << 16) | serial_low, declared in the schema.
        std::printf("serial = 0x%08X\n", static_cast<unsigned>(s->serial()));
    }

    // The struct can also set it: set_serial() splits the value back into serial_high / serial_low.
    sht::SerialNumber sn{};
    sn.set_serial(0x0F4F5A3C);
    if (auto k = sn.encode(tx)) print_bytes("serial", gmp::bytes_view(tx.data(), *k));

    // Encoding a response (e.g. for a sensor simulator) writes the CRCs automatically.
    sht::Measurement sim{};
    sim.set_temperature_scaled(25.0);
    sim.set_humidity_scaled(54.0);
    if (auto k = sim.encode(tx)) print_bytes("encoded", gmp::bytes_view(tx.data(), *k));
    return 0;
}
