// Recovery on an idle link: resync(), flush(), set_idle_timeout() + tick().

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

#include "telemetry.gmp.hpp"

namespace tel = acme::telemetry;
using Link = tel::BinaryLink;  // AA 55 | msg_id (u8) | len (u16 BE) | body | crc16

namespace {

using Bytes = std::vector<std::uint8_t>;

template <class M>
Bytes frame_of(const M& m) {
    std::array<std::uint8_t, Link::max_frame> buf{};
    auto n = Link::encode(m, buf);
    REQUIRE(n);
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)};
}

tel::Log log_msg(std::uint8_t level) {
    tel::Log l;
    l.level = level;
    l.text = "t";
    return l;
}

/// A Log frame whose length field claims 30 more bytes than the frame has (still a plausible Log size), followed by
/// two good frames: without help, the parser waits for bytes that never come.
Bytes stuck_stream() {
    Bytes bad = frame_of(log_msg(9));
    bad[4] = static_cast<std::uint8_t>(bad[4] + 30);
    Bytes s = bad;
    for (const std::uint8_t lvl : {std::uint8_t{1}, std::uint8_t{2}}) {
        const auto f = frame_of(log_msg(lvl));
        s.insert(s.end(), f.begin(), f.end());
    }
    return s;
}

struct Recorder {
    std::vector<int> levels;
    std::vector<gmp::Error> errors;
    std::vector<std::size_t> error_sizes;
    void operator()(const tel::LogView& v) { levels.push_back(v.level()); }
    void operator()(const gmp::FrameError& e) {
        errors.push_back(e.error);
        error_sizes.push_back(e.data.size());
    }
};

} // namespace

TEST_CASE("resync gives up an incomplete frame; the frames behind it are delivered") {
    const auto data = stuck_stream();
    Link::Stream<> s;
    Recorder r;
    (void)s.feed(data, r);
    CHECK(r.levels.empty());  // everything waits behind the bad length
    CHECK(s.buffered() == data.size());

    REQUIRE(s.resync(r));
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0] == gmp::Error::Truncated);
    CHECK(r.error_sizes[0] == data.size());  // reported: the bytes of the incomplete frame (all that is buffered)
    CHECK(s.stats().resyncs == 1);
    CHECK(s.last_error() == gmp::Error::Truncated);

    while (s.next(r)) {}  // resync only drops; the usual calls deliver what became complete
    CHECK(r.levels == std::vector<int>{1, 2});
    CHECK(s.buffered() == 0);
    CHECK_FALSE(s.resync(r));  // nothing left
}

TEST_CASE("resync does nothing when the head frame is complete or the buffer is empty") {
    Link::Stream<> s;
    Recorder r;
    CHECK_FALSE(s.resync(r));
    const auto f = frame_of(log_msg(3));
    CHECK(s.push(f) == f.size());
    CHECK_FALSE(s.resync(r));  // complete: not stuck
    CHECK(r.errors.empty());
    CHECK(s.poll());
}

TEST_CASE("flush drops everything buffered and reports it once") {
    Link::Stream<> s;
    Recorder r;
    const auto data = stuck_stream();
    (void)s.feed(data, r);
    REQUIRE(s.flush(r));
    CHECK(s.buffered() == 0);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0] == gmp::Error::Truncated);
    CHECK(r.error_sizes[0] == data.size());
    CHECK(s.stats().flushes == 1);
    CHECK(s.stats().bytes_skipped == data.size());
    CHECK_FALSE(s.flush(r));
    CHECK(r.levels.empty());
}

TEST_CASE("tick resyncs after the idle timeout, counted from the last bytes; the clock may wrap") {
    const auto data = stuck_stream();
    Link::Stream<> s;
    Recorder r;
    s.set_idle_timeout(100);
    std::uint32_t now = 0xFFFFFFC0u;  // wraps during the test
    CHECK_FALSE(s.tick(now, r));      // first tick: starts the clock
    (void)s.feed(Bytes(data.begin(), data.begin() + 5), r);
    now += 90;
    CHECK_FALSE(s.tick(now, r));  // bytes arrived since the last tick: activity
    (void)s.feed(Bytes(data.begin() + 5, data.end()), r);
    now += 60;
    CHECK_FALSE(s.tick(now, r));  // activity again (the clock restarts here)
    now += 99;
    CHECK_FALSE(s.tick(now, r));  // idle for 99 < 100
    CHECK(r.errors.empty());
    now += 1;
    REQUIRE(s.tick(now, r));  // idle for 100: resync
    CHECK(r.errors == std::vector<gmp::Error>{gmp::Error::Truncated});
    while (s.next(r)) {}
    CHECK(r.levels == std::vector<int>{1, 2});
    now += 500;
    CHECK_FALSE(s.tick(now, r));  // nothing buffered
    CHECK(s.stats().resyncs == 1);
}

TEST_CASE("tick: timeout 0 is off; a stored handler and pull mode get the same recovery") {
    const auto data = stuck_stream();
    {
        Link::Stream<> s;
        (void)s.push(data);
        CHECK_FALSE(s.tick(0));
        CHECK_FALSE(s.tick(1000000));  // timeout 0: never resyncs
    }
    {
        // pull mode (no handler): resync only drops; poll() returns the frames behind
        Link::Stream<> s;
        (void)s.push(data);
        CHECK_FALSE(s.poll());
        s.set_idle_timeout(5);
        CHECK_FALSE(s.tick(10));
        CHECK(s.tick(15));
        int n = 0;
        while (auto v = s.poll()) {
            CHECK(std::holds_alternative<tel::LogView>(*v));
            ++n;
        }
        CHECK(n == 2);
        CHECK(s.last_error() == gmp::Error::BadSync);  // the rest of the bad frame was skipped while resyncing
    }
    {
        // stored handler: errors go to it
        Recorder r;
        Link::StreamWith<Recorder&> s(r);
        s.set_idle_timeout(5);
        (void)s.feed(data);
        CHECK_FALSE(s.tick(0));
        CHECK(s.tick(5));
        CHECK(r.errors.front() == gmp::Error::Truncated);
        while (s.next()) {}
        CHECK(r.levels == std::vector<int>{1, 2});
    }
}
