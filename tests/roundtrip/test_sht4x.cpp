// Sensirion SHT4x example schema: commands, per-word CRC-8 responses and unit conversions.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <utility>

#include "sht4x.gmp.hpp"

using namespace sensirion::sht4x;
using Catch::Matchers::WithinAbs;

namespace {
// T = 0x6666 (25.00 degC), RH = 0x7AE1 (54.00 %RH); CRCs per datasheet Table 7 (CRC-8/NRSC-5).
constexpr std::array<std::uint8_t, 6> kMeasurement{0x66, 0x66, 0x93, 0x7A, 0xE1, 0xA4};
constexpr std::array<std::uint8_t, 6> kSerial{0x0F, 0x4F, 0x0B, 0x5A, 0x3C, 0x30};

template <class M>
std::uint8_t command_byte() {
    std::array<std::uint8_t, 1> out{};
    REQUIRE(M{}.encode(out) == std::size_t{1});
    REQUIRE(M::decode(out));  // and the byte decodes back as the same command
    return out[0];
}
} // namespace

TEST_CASE("SHT4x commands encode to the datasheet bytes (Table 8)") {
    CHECK(command_byte<MeasureHighPrecision>() == 0xFD);
    CHECK(command_byte<MeasureMediumPrecision>() == 0xF6);
    CHECK(command_byte<MeasureLowPrecision>() == 0xE0);
    CHECK(command_byte<ReadSerialNumber>() == 0x89);
    CHECK(command_byte<SoftReset>() == 0x94);
    CHECK(command_byte<Heater200mW1s>() == 0x39);
    CHECK(command_byte<Heater200mW100ms>() == 0x32);
    CHECK(command_byte<Heater110mW1s>() == 0x2F);
    CHECK(command_byte<Heater110mW100ms>() == 0x24);
    CHECK(command_byte<Heater20mW1s>() == 0x1E);
    CHECK(command_byte<Heater20mW100ms>() == 0x15);
    const std::array<std::uint8_t, 1> wrong{0xFD};
    CHECK(SoftReset::decode(wrong).error() == gmp::Error::BadConst);
}

TEST_CASE("SHT4x measurement: struct, view and protocol agree") {
    auto s = Measurement::decode(kMeasurement);
    REQUIRE(s);
    CHECK(s->temperature == 0x6666);
    CHECK(s->humidity == 0x7AE1);
    CHECK_THAT(s->temperature_scaled(), WithinAbs(25.0, 1e-9));
    CHECK_THAT(s->temperature_degf(), WithinAbs(77.0, 1e-9));  // -49 + 315 * 0x6666 / 65535
    CHECK_THAT(s->humidity_scaled(), WithinAbs(54.0, 1e-3));

    auto v = MeasurementView::from(kMeasurement);
    REQUIRE(v);
    CHECK(v->temperature() == 0x6666);
    CHECK_THAT(v->temperature_degf(), WithinAbs(77.0, 1e-9));
    CHECK_THAT(v->humidity_scaled(), WithinAbs(54.0, 1e-3));

    auto p = MeasurementResponse::decode(kMeasurement);
    REQUIRE(p);
    CHECK(std::get<Measurement>(*p) == *s);

    std::array<std::uint8_t, 6> out{};
    REQUIRE(s->encode(out) == std::size_t{6});
    CHECK(out == kMeasurement);  // encode writes both CRCs
}

TEST_CASE("SHT4x word CRCs are checked in every decode path") {
    for (std::size_t i = 0; i < kMeasurement.size(); ++i) {
        INFO("flipped byte " << i);
        auto bad = kMeasurement;
        bad[i] ^= 0x10;
        CHECK(Measurement::decode(bad).error() == gmp::Error::BadChecksum);
        CHECK(MeasurementView::from(bad).error() == gmp::Error::BadChecksum);
        CHECK(MeasurementResponse::parse(bad).error() == gmp::Error::BadChecksum);
    }
    CHECK(Measurement::decode(std::span(kMeasurement).first(5)).error() == gmp::Error::Truncated);
}

TEST_CASE("SHT4x serial number and setters") {
    auto v = SerialNumberView::from(kSerial);
    REQUIRE(v);
    CHECK(((std::uint32_t{v->serial_high()} << 16) | v->serial_low()) == 0x0F4F5A3Cu);
    auto r = SerialNumberResponse::decode(kSerial);
    REQUIRE(r);
    CHECK(std::get<SerialNumber>(*r).serial_low == 0x5A3C);

    Measurement m{};
    m.set_temperature_degf(77.0);
    CHECK(m.temperature == 0x6666);
    m.set_temperature_scaled(25.0);
    m.set_humidity_scaled(54.0);
    std::array<std::uint8_t, 6> out{};
    REQUIRE(m.encode(out));
    CHECK(out == kMeasurement);
}

TEST_CASE("SHT4x responses in a byte stream") {
    // Measurements read back to back, delivered in odd-sized chunks.
    std::array<std::uint8_t, 18> bytes{};
    for (std::size_t k = 0; k < 3; ++k)
        for (std::size_t i = 0; i < 6; ++i) bytes[k * 6 + i] = kMeasurement[i];
    MeasurementResponse::Stream<> stream;
    int got = 0;
    auto handler = [&](const MeasurementView& v) {
        ++got;
        CHECK(v.humidity() == 0x7AE1);
    };
    stream.feed(std::span(bytes).first(4), handler);
    stream.feed(std::span(bytes).subspan(4), handler);
    CHECK(got == 3);
}

// ---- command-enum variant (examples/sensirion_sht4x/sht4x_enum.yaml) ----
#include "sht4x_enum.gmp.hpp"

TEST_CASE("SHT4x enum variant: every command byte matches the per-message schema") {
    namespace en = sensirion::sht4x_enum;
    using C = en::Command;
    const std::pair<C, std::uint8_t> table[] = {
        {C::measure_high_precision, command_byte<MeasureHighPrecision>()},
        {C::measure_medium_precision, command_byte<MeasureMediumPrecision>()},
        {C::measure_low_precision, command_byte<MeasureLowPrecision>()},
        {C::read_serial_number, command_byte<ReadSerialNumber>()},
        {C::soft_reset, command_byte<SoftReset>()},
        {C::heater_200mw_1s, command_byte<Heater200mW1s>()},
        {C::heater_200mw_100ms, command_byte<Heater200mW100ms>()},
        {C::heater_110mw_1s, command_byte<Heater110mW1s>()},
        {C::heater_110mw_100ms, command_byte<Heater110mW100ms>()},
        {C::heater_20mw_1s, command_byte<Heater20mW1s>()},
        {C::heater_20mw_100ms, command_byte<Heater20mW100ms>()},
    };
    for (const auto& [cmd, byte] : table) {
        INFO(en::to_string(cmd));
        std::array<std::uint8_t, 1> out{};
        REQUIRE(en::CommandFrame{cmd}.encode(out) == std::size_t{1});
        CHECK(out[0] == byte);
        auto back = en::CommandFrame::decode(out);
        REQUIRE(back);
        CHECK(back->command == cmd);
        CHECK_FALSE(en::to_string(back->command).empty());
    }
    const std::array<std::uint8_t, 1> unknown{0x42};
    CHECK(en::CommandFrame::decode(unknown).error() == gmp::Error::UnknownEnum);  // Command is strict
    CHECK(en::CommandFrameView::from(unknown).error() == gmp::Error::UnknownEnum);
    CHECK_FALSE(en::is_known(static_cast<en::Command>(0x42)));
}

TEST_CASE("SHT4x enum variant: responses decode like the per-message schema") {
    namespace en = sensirion::sht4x_enum;
    auto a = MeasurementResponse::decode(kMeasurement);
    auto b = en::MeasurementResponse::decode(kMeasurement);
    REQUIRE(a);
    REQUIRE(b);
    const auto& ma = std::get<Measurement>(*a);
    const auto& mb = std::get<en::Measurement>(*b);
    CHECK(mb.temperature == ma.temperature);
    CHECK(mb.humidity == ma.humidity);
    CHECK_THAT(mb.temperature_degf(), WithinAbs(77.0, 1e-9));
    auto bad = kMeasurement;
    bad[4] ^= 0x01;
    CHECK(en::MeasurementResponse::parse(bad).error() == gmp::Error::BadChecksum);
    auto s = en::SerialNumberView::from(kSerial);
    REQUIRE(s);
    CHECK(s->serial_high() == 0x0F4F);
}

TEST_CASE("SHT4x enum variant: encoding an unknown command fails") {
    namespace en = sensirion::sht4x_enum;
    std::array<std::uint8_t, 1> out{};
    CHECK(en::CommandFrame{static_cast<en::Command>(0x42)}.encode(out).error() == gmp::Error::UnknownEnum);
}

TEST_CASE("SHT4x serial number as a computed value, in both schemas") {
    auto v = SerialNumberView::from(kSerial);
    REQUIRE(v);
    CHECK(v->serial() == 0x0F4F5A3Cu);
    SerialNumber s{};
    s.set_serial(0x0F4F5A3Cu);
    CHECK(s.serial_high == 0x0F4F);
    CHECK(s.serial_low == 0x5A3C);
    std::array<std::uint8_t, 6> out{};
    REQUIRE(s.encode(out));
    CHECK(out == kSerial);  // set_serial + encode reproduces the sensor's bytes, CRCs included

    namespace en = sensirion::sht4x_enum;
    auto r = en::SerialNumberResponse::decode(kSerial);
    REQUIRE(r);
    CHECK(std::get<en::SerialNumber>(*r).serial() == 0x0F4F5A3Cu);
}

TEST_CASE("SHT4x: Schema Studio's example frames decode identically in C++") {
    struct Case { std::array<std::uint8_t, 6> bytes; double t, rh; };
    const Case cases[] = {
        {{0x66, 0x66, 0x93, 0x7A, 0xE1, 0xA4}, 25.0, 54.0},
        {{0x33, 0x33, 0x88, 0xBA, 0x5D, 0xCC}, -10.0, 85.0},
        {{0xAB, 0xE2, 0xC7, 0x24, 0xDD, 0x96}, 72.5, 12.0},
        {{0x47, 0xAE, 0x45, 0xDB, 0x8B, 0xCF}, 4.0, 101.2},  // uncropped RH above 100 %
    };
    for (const auto& c : cases) {
        auto m = MeasurementResponse::decode(c.bytes);
        REQUIRE(m);
        const auto& v = std::get<Measurement>(*m);
        CHECK_THAT(v.temperature_scaled(), WithinAbs(c.t, 0.01));
        CHECK_THAT(v.humidity_scaled(), WithinAbs(c.rh, 0.01));
    }
}
