#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <random>
#include <vector>

#include "telemetry.gmp.hpp"

using namespace acme::telemetry;

namespace {

Status make_status(Mode mode) {
    Status s;
    s.voltage = 1234;
    s.flags.armed = true;
    s.flags.mode = mode;
    s.flags.level = -3;
    s.samples = {10, -20, 30};
    if (mode == Mode::fault) s.ext = Extended{-125, 101325};
    s.label = "abc";
    return s;
}

} // namespace

TEST_CASE("Status struct round trip (conditional present and absent)") {
    for (auto mode : {Mode::run, Mode::fault}) {
        const Status s = make_status(mode);
        std::array<std::uint8_t, Status::max_wire_size> buf{};
        auto n = s.encode(buf);
        REQUIRE(n);
        CHECK(*n == (mode == Mode::fault ? 26u : 20u));
        CHECK(buf[0] == 0x5A);  // const magic written automatically

        auto d = Status::decode(gmp::bytes_view(buf.data(), *n));
        REQUIRE(d);
        Status expected = s;
        expected.count = 3;  // derived from samples.size()
        CHECK(*d == expected);
    }
}

TEST_CASE("Status view matches struct") {
    const Status s = make_status(Mode::fault);
    std::array<std::uint8_t, 64> buf{};
    auto n = s.encode(buf);
    REQUIRE(n);
    auto v = StatusView::from(gmp::bytes_view(buf.data(), *n));
    REQUIRE(v);
    CHECK(v->wire_size() == *n);
    CHECK(v->voltage() == 1234);
    CHECK_THAT(v->voltage_scaled(), Catch::Matchers::WithinAbs(12.34, 1e-9));
    CHECK(v->flags().armed);
    CHECK(v->flags().mode == Mode::fault);
    CHECK(v->flags().level == -3);
    CHECK(v->count() == 3);
    auto samples = v->samples();
    REQUIRE(samples.size() == 3);
    CHECK(samples[1] == -20);
    auto ext = v->ext();
    REQUIRE(ext);
    CHECK(ext->temperature() == -125);
    CHECK_THAT(ext->temperature_scaled(), Catch::Matchers::WithinAbs(-12.5, 1e-9));
    CHECK(ext->pressure() == 101325u);
    CHECK(v->label() == "abc");  // trailing pad characters stripped
    auto back = v->to_struct();
    REQUIRE(back);
    CHECK(back->samples == s.samples);
}

TEST_CASE("view rejects bad const and truncation") {
    const Status s = make_status(Mode::run);
    std::array<std::uint8_t, 64> buf{};
    auto n = s.encode(buf);
    REQUIRE(n);
    for (std::size_t len = 0; len < *n; ++len) {
        auto v = StatusView::from(gmp::bytes_view(buf.data(), len));
        REQUIRE_FALSE(v);
    }
    buf[0] = 0x00;
    auto v = StatusView::from(gmp::bytes_view(buf.data(), *n));
    REQUIRE_FALSE(v);
    CHECK(v.error() == gmp::Error::BadConst);
}

TEST_CASE("array capacity is enforced on decode") {
    std::array<std::uint8_t, 64> buf{};
    Status s = make_status(Mode::run);
    auto n = s.encode(buf);
    REQUIRE(n);
    buf[4] = 200;  // count > max (16)
    auto d = Status::decode(gmp::bytes_view(buf.data(), *n));
    REQUIRE_FALSE(d);
    CHECK(d.error() == gmp::Error::OutOfRange);
}

TEST_CASE("Log: terminated string and remaining bytes") {
    Log l;
    l.level = 4;
    l.text = "hello";
    l.tail = {1, 2, 3};
    std::array<std::uint8_t, 128> buf{};
    auto n = l.encode(buf);
    REQUIRE(n);
    CHECK(*n == 1 + 6 + 3);
    auto v = LogView::from(gmp::bytes_view(buf.data(), *n));
    REQUIRE(v);
    CHECK(v->text() == "hello");
    CHECK(v->tail().size() == 3);
    auto d = Log::decode(gmp::bytes_view(buf.data(), *n));
    REQUIRE(d);
    CHECK(*d == l);
}

TEST_CASE("BinaryLink framing: encode, parse and dispatch") {
    Command c;
    c.opcode = 0xBEEF;
    c.args = {9, 8, 7};
    std::array<std::uint8_t, BinaryLink::max_frame> frame{};
    auto n = BinaryLink::encode(c, frame);
    REQUIRE(n);
    // AA 55 | id 02 | len 00 06 | opcode BE EF | arg_len 03 | 09 08 07 | crc
    CHECK(*n == 2 + 3 + 6 + 2);
    CHECK(frame[0] == 0xAA);
    CHECK(frame[2] == 0x02);
    CHECK(frame[4] == 0x06);

    auto any = BinaryLink::parse(gmp::bytes_view(frame.data(), *n));
    REQUIRE(any);
    REQUIRE(std::holds_alternative<CommandView>(*any));
    const auto& cv = std::get<CommandView>(*any);
    CHECK(cv.opcode() == 0xBEEF);
    CHECK(cv.args().size() == 3);

    auto msg = BinaryLink::decode(gmp::bytes_view(frame.data(), *n));
    REQUIRE(msg);
    Command expected = c;
    expected.arg_len = 3;
    CHECK(std::get<Command>(*msg) == expected);

    // corrupt one body byte -> checksum failure
    frame[6] ^= 0x01;
    auto bad = BinaryLink::parse(gmp::bytes_view(frame.data(), *n));
    REQUIRE_FALSE(bad);
    CHECK(bad.error() == gmp::Error::BadChecksum);
}

TEST_CASE("BinaryLink stream: random chunking with garbage between frames") {
    std::vector<std::uint8_t> stream;
    std::mt19937 rng(42);
    int expected_status = 0, expected_cmd = 0, expected_log = 0;
    for (int i = 0; i < 300; ++i) {
        std::array<std::uint8_t, BinaryLink::max_frame> f{};
        gmp::Result<std::size_t> n = gmp::fail(gmp::Error::None);
        switch (rng() % 3) {
            case 0: n = BinaryLink::encode(make_status(rng() % 2 ? Mode::fault : Mode::idle), f); ++expected_status; break;
            case 1: {
                Command c;
                c.opcode = static_cast<std::uint16_t>(rng());
                c.args.resize(rng() % 32);
                n = BinaryLink::encode(c, f);
                ++expected_cmd;
                break;
            }
            default: {
                Log l;
                l.text = "msg";
                n = BinaryLink::encode(l, f);
                ++expected_log;
            }
        }
        REQUIRE(n);
        stream.insert(stream.end(), f.begin(), f.begin() + static_cast<std::ptrdiff_t>(*n));
        if (rng() % 5 == 0) stream.insert(stream.end(), {0x00, 0xAA, 0x13});  // garbage incl. partial sync
    }

    int got_status = 0, got_cmd = 0, got_log = 0, errors = 0;
    auto handler = gmp::overloaded{
        [&](const BinaryLink::HeaderView& h, const StatusView& v) {
            CHECK(h.msg_id() == MsgId::status);
            CHECK(v.voltage() == 1234);
            ++got_status;
        },
        [&](const CommandView&) { ++got_cmd; },
        [&](const LogView& v) { CHECK(v.text() == "msg"); ++got_log; },
        [&](const gmp::FrameError&) { ++errors; },
    };
    BinaryLink::Stream<> parser;
    std::size_t off = 0;
    while (off < stream.size()) {
        const std::size_t chunk = std::min<std::size_t>(1 + rng() % 40, stream.size() - off);
        parser.feed(gmp::bytes_view(stream.data() + off, chunk), handler);
        off += chunk;
    }
    CHECK(got_status == expected_status);
    CHECK(got_cmd == expected_cmd);
    CHECK(got_log == expected_log);
    CHECK(errors > 0);
}
