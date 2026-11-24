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
    std::vector<gmsg::Error> errors;
    std::vector<std::size_t> error_sizes;
    void operator()(const tel::LogView& v) { levels.push_back(v.level()); }
    void operator()(const gmsg::FrameError& e) {
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
    CHECK(r.errors[0] == gmsg::Error::Truncated);
    CHECK(r.error_sizes[0] == data.size());  // reported: the bytes of the incomplete frame (all that is buffered)
    CHECK(s.stats().resyncs == 1);
    CHECK(s.last_error() == gmsg::Error::Truncated);

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
    CHECK(r.errors[0] == gmsg::Error::Truncated);
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
    CHECK(r.errors == std::vector<gmsg::Error>{gmsg::Error::Truncated});
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
        CHECK(s.last_error() == gmsg::Error::BadSync);  // the rest of the bad frame was skipped while resyncing
    }
    {
        // stored handler: errors go to it
        Recorder r;
        Link::StreamWith<Recorder&> s(r);
        s.set_idle_timeout(5);
        (void)s.feed(data);
        CHECK_FALSE(s.tick(0));
        CHECK(s.tick(5));
        CHECK(r.errors.front() == gmsg::Error::Truncated);
        while (s.next()) {}
        CHECK(r.levels == std::vector<int>{1, 2});
    }
}

// ---- interrupt input through gmsg::ByteRing ------------------------------------------------------------------

TEST_CASE("generated streams drain a ByteRing; an overrun drops the frame in progress") {
    const Bytes a = frame_of(log_msg(1)), b = frame_of(log_msg(2)), c = frame_of(log_msg(3)), d = frame_of(log_msg(4));
    gmsg::ByteRing<32> ring;
    const std::size_t complete = a.size() + b.size() + c.size();
    REQUIRE(complete < 32);
    REQUIRE(complete + d.size() > 32);
    for (const Bytes* f : {&a, &b, &c}) CHECK(ring.push(gmsg::bytes_view(*f)) == f->size());
    ring.push(gmsg::bytes_view(d));  // only its start fits
    CHECK(ring.overrun_pending());

    SECTION("feed(ring) with a handler") {
        Link::Stream<> sp;
        Recorder rec;
        CHECK(sp.feed(ring, rec) == 3);
        CHECK(rec.levels == std::vector<int>{1, 2, 3});
        CHECK(rec.errors == std::vector<gmsg::Error>{gmsg::Error::Overrun});
        CHECK(rec.error_sizes == std::vector<std::size_t>{32 - complete});
        CHECK(ring.push(gmsg::bytes_view(d)) == d.size());
        CHECK(sp.feed(ring, rec) == 1);
        CHECK(rec.levels == std::vector<int>{1, 2, 3, 4});
    }
    SECTION("CallbackStream: the error listener gets the overrun") {
        struct App {
            std::vector<int> levels;
            std::vector<gmsg::Error> errors;
            void on_log(const tel::LogView& v) { levels.push_back(v.level()); }
            void on_error(const gmsg::FrameError& e) { errors.push_back(e.error); }
        } app;
        Link::CallbackStream<> sp;
        sp.on<&App::on_log>(app).on<&App::on_error>(app);
        CHECK(sp.feed(ring) == 3);
        CHECK(app.levels == std::vector<int>{1, 2, 3});
        CHECK(app.errors == std::vector<gmsg::Error>{gmsg::Error::Overrun});
    }
    SECTION("pull API: poll_event returns the overrun inline, in stream order") {
        Link::Stream<> sp;
        sp.push(ring);
        std::vector<int> order;  // level, or -1 for the overrun
        while (auto ev = sp.poll_event()) {
            if (const auto* v = std::get_if<Link::AnyView>(&*ev)) order.push_back(std::get<tel::LogView>(*v).level());
            else {
                CHECK(std::get<gmsg::FrameError>(*ev).error == gmsg::Error::Overrun);
                order.push_back(-1);
            }
        }
        CHECK(order == std::vector<int>{1, 2, 3, -1});
        CHECK(sp.stats().overruns == 1);
    }
}
