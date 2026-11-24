#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <vector>

#include "can.gmp.hpp"
#include "hdlc.gmp.hpp"

using Catch::Matchers::WithinAbs;

namespace {

link::hdlc::Batch make_batch(std::uint8_t n) {
    link::hdlc::Batch b;
    b.when = test::Time{12, 34, 56};
    for (std::uint8_t i = 0; i < n; ++i) {
        link::hdlc::Reading r;
        r.channel = i;
        r.name = i % 2 ? "temperature" : "p";
        r.value = 1.5f * static_cast<float>(i);
        // 0x7E / 0x7D bytes in the payload force escaping
        if (i == 1) r.channel = 0x7E;
        if (i == 2) r.channel = 0x7D;
        (void)b.readings.push_back(r);
    }
    if (n > 2) b.crc_in_payload = 0x7E7D7E7Du;
    return b;
}

link::hdlc::Batch with_derived(link::hdlc::Batch b) {
    b.n = static_cast<std::uint8_t>(b.readings.size());
    for (auto& r : b.readings) r.name_len = static_cast<std::uint8_t>(r.name.size());
    return b;
}

} // namespace

TEST_CASE("HDLC: escaping, custom codec, nested variable-size array") {
    using namespace link::hdlc;
    for (int ni : {0, 1, 3, 8}) {
        const auto n = static_cast<std::uint8_t>(ni);
        const Batch b = make_batch(n);
        std::array<std::uint8_t, Hdlc::max_frame> frame{};
        auto len = Hdlc::encode(b, frame);
        REQUIRE(len);
        // No unescaped flag bytes inside the frame.
        for (std::size_t i = 1; i + 1 < *len; ++i) REQUIRE(frame[i] != 0x7E);
        CHECK(frame[0] == 0x7E);
        CHECK(frame[*len - 1] == 0x7E);

        auto copy = frame;  // parse unescapes in place
        auto any = Hdlc::decode(gmsg::mut_bytes(copy.data(), *len));
        REQUIRE(any);
        CHECK(std::get<Batch>(*any) == with_derived(b));

        copy = frame;
        auto view = Hdlc::parse(gmsg::mut_bytes(copy.data(), *len));
        REQUIRE(view);
        const auto& bv = std::get<BatchView>(*view);
        CHECK(*bv.when() == test::Time{12, 34, 56});
        std::size_t count = 0;
        for (const auto& r : bv.readings()) {
            CHECK(r.name() == (count % 2 ? "temperature" : "p"));
            CHECK_THAT(r.value(), WithinAbs(1.5 * static_cast<double>(count), 1e-6));
            ++count;
        }
        CHECK(count == n);
        CHECK(bv.crc_in_payload().has_value() == (n > 2));
    }
}

TEST_CASE("HDLC: header const and checksum are validated") {
    using namespace link::hdlc;
    Ping p;
    p.seq = 7;
    std::array<std::uint8_t, Hdlc::max_frame> frame{};
    auto len = Hdlc::encode(p, frame);
    REQUIRE(len);
    CHECK(frame[1] == 0xFF);  // const address
    auto copy = frame;
    copy[1] = 0xFE;
    auto bad = Hdlc::parse(gmsg::mut_bytes(copy.data(), *len));
    REQUIRE_FALSE(bad);
    CHECK(bad.error() == gmsg::Error::BadChecksum);
}

TEST_CASE("HDLC stream: shared flags between frames") {
    using namespace link::hdlc;
    std::vector<std::uint8_t> stream;
    for (int i = 0; i < 20; ++i) {
        std::array<std::uint8_t, Hdlc::max_frame> f{};
        auto len = i % 2 ? Hdlc::encode(make_batch(static_cast<std::uint8_t>(i % 5)), f) : Hdlc::encode(Ping{static_cast<std::uint32_t>(i), {}}, f);
        REQUIRE(len);
        // share the closing flag with the next opening flag
        const std::size_t start = stream.empty() ? 0 : 1;
        stream.insert(stream.end(), f.begin() + static_cast<std::ptrdiff_t>(start), f.begin() + static_cast<std::ptrdiff_t>(*len));
    }
    int pings = 0, batches = 0;
    Hdlc::Stream<> sp;
    sp.feed(stream, gmsg::overloaded{[&](const PingView&) { ++pings; }, [&](const BatchView&) { ++batches; }});
    CHECK(pings == 10);
    CHECK(batches == 10);
}

TEST_CASE("SLIP style map escaping with custom checksum") {
    using namespace link::hdlc;
    Ping p;
    p.seq = 0xC0DBC0DB;
    p.payload = {0xC0, 0xDB, 1, 2};
    std::array<std::uint8_t, Summed::max_frame> frame{};
    auto len = Summed::encode(p, frame);
    REQUIRE(len);
    for (std::size_t i = 1; i + 1 < *len; ++i) REQUIRE(frame[i] != 0xC0);
    auto any = Summed::decode(gmsg::mut_bytes(frame.data(), *len));
    REQUIRE(any);
    CHECK(std::get<Ping>(*any) == p);
}

TEST_CASE("CAN: lsb_first signals with scale/offset and const") {
    using namespace vehicle::can;
    EngineData e;
    e.signals.rpm = 1000;
    e.signals.gear = Gear::drive;
    e.signals.coolant = 130;  // raw; offset -40 -> 90 degC
    e.signals.throttle = 512;
    e.signals.counter = 9;
    e.signals.flags = 0xBEEF;
    std::array<std::uint8_t, 8> frame{};
    auto n = Can::encode(e, frame);
    REQUIRE(n);
    CHECK(*n == 8);
    CHECK(frame[0] == 0xE8);                    // rpm low byte
    CHECK(frame[1] == ((1000 >> 8) | (3 << 6)));  // rpm high bits + gear
    CHECK(frame[2] == 130);
    CHECK((frame[5] >> 4) == 1);  // const version nibble
    CHECK(frame[6] == 0xEF);
    CHECK(frame[7] == 0xBE);

    auto v = Can::parse(frame);
    REQUIRE(v);
    const auto s = std::get<EngineDataView>(*v).signals();
    CHECK_THAT(s.rpm_scaled(), WithinAbs(500.0, 1e-9));
    CHECK(s.gear == Gear::drive);
    CHECK_THAT(s.coolant_scaled(), WithinAbs(90.0, 1e-9));
    CHECK_THAT(s.throttle_scaled(), WithinAbs(51.2, 1e-9));
    CHECK(s.counter == 9);

    frame[5] ^= 0x10;  // break the const version
    CHECK(Can::parse(frame).error() == gmsg::Error::BadConst);
}

TEST_CASE("fixed framing with sync, padding and inline CRC parameters") {
    using namespace vehicle::can;
    Sensor s;
    s.temp = -1234;
    s.humidity = 55;
    std::array<std::uint8_t, FixedLink::max_frame> frame{};
    auto n = FixedLink::encode(s, frame);
    REQUIRE(n);
    CHECK(*n == 10);  // 2 sync + 1 id + 3 body + 3 pad + 1 crc
    CHECK(frame[0] == 0xA5);
    CHECK(frame[1] == 0xA5);
    // CRC-8/SAE-J1850 of "123456789" is 0x4B
    constexpr std::uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    using J1850 = gmsg::checksum::Crc<std::uint8_t, 8, 0x1D, 0xFF, false, false, 0xFF>;
    static_assert(J1850::compute(gmsg::bytes_view(check)) == 0x4B);

    std::vector<std::uint8_t> stream{0x01, 0xA5};
    stream.insert(stream.end(), frame.begin(), frame.begin() + 10);
    stream.insert(stream.end(), frame.begin(), frame.begin() + 10);
    int got = 0;
    FixedLink::Stream<> sp;
    sp.feed(stream, [&](const SensorView& v) {
        CHECK(v.temp() == -1234);
        CHECK_THAT(v.temp_scaled(), WithinAbs(-12.34, 1e-9));
        ++got;
    });
    CHECK(got == 2);
}
