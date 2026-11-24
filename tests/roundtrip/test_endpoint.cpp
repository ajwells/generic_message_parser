// gmsg::Endpoint (gmp/endpoint.hpp) and the byte sources / sinks of gmp/io.hpp: service(), send() with both send
// policies, wait_for / wait_for_reply / transact with a tick clock and with std::chrono, the DMA ring source, and
// the parser's push_with / push_gap.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "nmea.gmp.hpp"
#include "telemetry.gmp.hpp"

namespace tel = acme::telemetry;
namespace nm = nav::nmea;
using Link = tel::BinaryLink;

namespace {

template <class P = Link, class M>
std::vector<std::uint8_t> frame_of(const M& m) {
    std::array<std::uint8_t, P::max_frame> buf{};
    auto n = P::encode(m, buf);
    REQUIRE(n);
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)};
}
std::vector<std::uint8_t> cat(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> w;
    for (const auto& p : parts) w.insert(w.end(), p.begin(), p.end());
    return w;
}
gmsg::bytes_view bv(const std::vector<std::uint8_t>& v) { return {v.data(), v.size()}; }

tel::Status status(std::uint16_t voltage) {
    tel::Status s;
    s.voltage = voltage;
    s.label = "s";
    return s;
}
tel::Command command(std::uint16_t op) {
    tel::Command c;
    c.opcode = op;
    return c;
}
tel::Log log_msg(std::uint8_t level) {
    tel::Log l;
    l.level = level;
    l.text = "t";
    return l;
}

/// A test clock: a counter the idle hook advances.
struct Ticks {
    std::uint32_t t = 0;
    std::uint32_t step = 1;  // added by each idle() call
    int idles = 0;
    static void idle(void* self) {
        auto* c = static_cast<Ticks*>(self);
        c->t += c->step;
        ++c->idles;
    }
};
struct TickClock {
    Ticks* c;
    std::uint32_t operator()() const { return c->t; }
};

/// Records what the listeners saw.
struct Seen {
    std::vector<std::string> events;
    void on_status(const tel::StatusView& v) { events.push_back("status " + std::to_string(v.voltage())); }
    void on_command(const tel::CommandView& v) { events.push_back("command " + std::to_string(v.opcode())); }
    void on_log(const tel::LogView& v) { events.push_back("log " + std::to_string(v.level())); }
    void on_error(const gmsg::FrameError& e) { events.push_back(std::string("error ") + std::string(gmsg::to_string(e.error))); }
    template <class E>
    void attach(E& ep) {
        ep.template on<&Seen::on_status>(*this);
        ep.template on<&Seen::on_command>(*this);
        ep.template on<&Seen::on_log>(*this);
        ep.template on<&Seen::on_error>(*this);
    }
};

/// A sink that takes at most `per_write` bytes per call and has no writable().
struct SlowSink {
    std::vector<std::uint8_t> out;
    std::size_t per_write = 3;
    std::size_t write(gmsg::bytes_view b) {
        const std::size_t n = std::min(b.size(), per_write);
        out.insert(out.end(), b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
};

} // namespace

TEST_CASE("endpoint service(): reads a source in chunks and delivers every frame to the listeners") {
    const auto w = cat({frame_of(status(100)), {0x13, 0x37}, frame_of(command(7)), frame_of(log_msg(2))});
    gmsg::SpanSource src(bv(w), 5);
    auto ep = Link::make_endpoint(src, gmsg::NoSink{});
    Seen seen;
    seen.attach(ep);
    CHECK(ep.service() == 3);
    CHECK(seen.events == std::vector<std::string>{"status 100", "error bad sync", "command 7", "log 2"});
    CHECK(src.remaining() == 0);
    CHECK(ep.service() == 0);
    CHECK(ep.stats().frames == 3);
}

TEST_CASE("endpoint service(max_bytes) reads at most max_bytes") {
    const auto w = cat({frame_of(status(1)), frame_of(status(2))});
    gmsg::SpanSource src(bv(w));
    auto ep = Link::make_endpoint(src, gmsg::NoSink{});
    Seen seen;
    seen.attach(ep);
    CHECK(ep.service(w.size() / 2 + 1) == 1);
    CHECK(src.remaining() == w.size() / 2 - 1);
    CHECK(ep.service() == 1);
}

TEST_CASE("endpoint service() runs the idle resync with the clock") {
    // a frame whose length claims more bytes than will arrive, followed by a good one
    auto bad = frame_of(status(5));
    bad.resize(10);
    bad[3] = 0x00;
    bad[4] = 200;  // length 200: the parser waits for bytes that never come
    const auto w = cat({bad, frame_of(status(6))});
    gmsg::SpanSource src(bv(w));
    Ticks clk;
    auto ep = Link::make_endpoint(src, gmsg::NoSink{}, TickClock{&clk});
    Seen seen;
    seen.attach(ep);
    ep.set_idle_timeout(10);
    ep.service();  // first tick: activity
    CHECK(seen.events.empty());
    clk.t = 5;
    ep.service();
    CHECK(seen.events.empty());
    clk.t = 20;
    ep.service();  // idle for 15 >= 10: resync, the good frame is found later in the buffer
    ep.service();
    REQUIRE(!seen.events.empty());
    CHECK(seen.events.front() == "error truncated");
    CHECK(seen.events.back() == "status 6");
}

TEST_CASE("endpoint send(): whole_frame writes the frame or nothing") {
    std::array<std::uint8_t, 64> out{};
    gmsg::SpanSink sink(out);
    auto ep = Link::make_endpoint(gmsg::NoSource{}, sink);
    const auto f = frame_of(command(9));
    auto n = ep.send(command(9));
    REQUIRE(n);
    CHECK(*n == f.size());
    CHECK(std::vector<std::uint8_t>(sink.written().begin(), sink.written().end()) == f);

    // not enough room: BufferFull, nothing written
    std::array<std::uint8_t, 8> small{};
    gmsg::SpanSink tight(small);
    auto ep2 = Link::make_endpoint(gmsg::NoSource{}, tight);
    REQUIRE(f.size() > small.size());
    auto r = ep2.send(command(9));
    REQUIRE_FALSE(r);
    CHECK(r.error() == gmsg::Error::BufferFull);
    CHECK(tight.written().empty());

    // header values given explicitly; encode errors pass through
    gmsg::SpanSink sink3(out);
    auto ep3 = Link::make_endpoint(gmsg::NoSource{}, sink3);
    Link::Header h;
    REQUIRE(ep3.send(h, command(3)));
    CHECK(sink3.written().size() == frame_of(command(3)).size());
    auto none = Link::make_endpoint(gmsg::NoSource{}, gmsg::NoSink{});
    CHECK(none.send(command(1)).error() == gmsg::Error::BufferFull);
}

TEST_CASE("endpoint send(): a sink without writable() that takes part of a frame") {
    SlowSink sink;
    auto ep = Link::make_endpoint(gmsg::NoSource{}, sink);
    const auto f = frame_of(status(3));
    // whole_frame: such a sink is assumed to take whole frames; a short write fails
    auto r = ep.send(status(3));
    REQUIRE_FALSE(r);
    CHECK(r.error() == gmsg::Error::BufferFull);
    CHECK(sink.out.size() == 3);

    // retry without a timeout: written in pieces, the idle hook runs in between
    sink.out.clear();
    int idles = 0;
    ep.set_idle(+[](void* n) { ++*static_cast<int*>(n); }, &idles);
    ep.set_send_policy(gmsg::SendPolicy::retry);
    CHECK(ep.send_policy() == gmsg::SendPolicy::retry);
    REQUIRE(ep.send(status(3)));
    CHECK(sink.out == f);
    CHECK(idles == static_cast<int>((f.size() + 2) / 3) - 1);
}

TEST_CASE("endpoint send(): retry with a timeout") {
    struct Stuck {
        std::size_t taken = 0;
        std::size_t write(gmsg::bytes_view b) {
            if (taken >= 4) return 0;  // the line stops after 4 bytes
            const std::size_t n = std::min<std::size_t>(b.size(), 2);
            taken += n;
            return n;
        }
    } sink;
    Ticks clk;
    auto ep = Link::make_endpoint(gmsg::NoSource{}, sink, TickClock{&clk});
    ep.set_idle(&Ticks::idle, &clk);
    ep.set_send_policy(gmsg::SendPolicy::retry, 50);
    auto r = ep.send(command(1));
    REQUIRE_FALSE(r);
    CHECK(r.error() == gmsg::Error::Timeout);
    CHECK(sink.taken == 4);
    CHECK(clk.t >= 50);
    CHECK(clk.t < 60);
}

TEST_CASE("endpoint send_raw() writes bytes as they are") {
    std::array<std::uint8_t, 8> out{};
    gmsg::SpanSink sink(out);
    auto ep = Link::make_endpoint(gmsg::NoSource{}, sink);
    const std::vector<std::uint8_t> cmd{0xFD};
    REQUIRE(ep.send_raw(bv(cmd)));
    CHECK(sink.written().size() == 1);
    CHECK(sink.written()[0] == 0xFD);
}

TEST_CASE("endpoint with ByteRings: interrupt-style RX ring as source, TX ring as sink") {
    gmsg::ByteRing<64> rx;
    gmsg::ByteRing<16> tx;
    auto ep = Link::make_endpoint(rx, tx);
    Seen seen;
    seen.attach(ep);

    // TX: the ring is the transmit queue; a frame that does not fit is refused whole
    const auto f = frame_of(command(5));
    REQUIRE(ep.send(command(5)));
    CHECK(tx.size() == f.size());
    CHECK(tx.room() == 16 - f.size());
    auto r = ep.send(command(5));
    REQUIRE_FALSE(r);
    CHECK(r.error() == gmsg::Error::BufferFull);
    CHECK(tx.size() == f.size());
    std::array<std::uint8_t, 16> sent{};
    CHECK(tx.read(sent) == f.size());
    CHECK(std::vector<std::uint8_t>(sent.begin(), sent.begin() + static_cast<std::ptrdiff_t>(f.size())) == f);

    // RX: frames pushed by the "ISR" are delivered; an overflow is reported as an overrun
    const auto s = frame_of(status(1));
    rx.push(bv(s));
    CHECK(ep.service() == 1);
    const auto big = cat({frame_of(status(2)), frame_of(status(3)), frame_of(status(4)), frame_of(status(5))});
    rx.push(bv(big));  // more than 64 bytes: the end is dropped
    CHECK(rx.overrun_pending());
    ep.service();
    rx.push(bv(s));
    ep.service();
    CHECK(seen.events.front() == "status 1");
    CHECK(std::find(seen.events.begin(), seen.events.end(), "error overrun") != seen.events.end());
    CHECK(seen.events.back() == "status 1");
    CHECK(ep.stats().overruns == 1);
}

TEST_CASE("endpoint wait_for(): other messages go to the listeners, the awaited one is returned") {
    const auto w = cat({frame_of(status(1)), frame_of(log_msg(4)), frame_of(command(42)), frame_of(status(2))});
    gmsg::SpanSource src(bv(w), 7);
    Ticks clk;
    auto ep = Link::make_endpoint(src, gmsg::NoSink{}, TickClock{&clk});
    ep.set_idle(&Ticks::idle, &clk);
    Seen seen;
    seen.attach(ep);

    auto c = ep.wait_for<tel::Command>(100);
    REQUIRE(c);
    CHECK(c->opcode == 42);
    CHECK(seen.events == std::vector<std::string>{"status 1", "log 4"});  // the command itself is not delivered
    CHECK(src.remaining() > 0);  // reading stopped soon after the command

    // the view type works too; a match predicate skips non-matching messages (they go to the listeners)
    auto s = ep.wait_for<tel::StatusView>(100, [](const tel::StatusView& v) { return v.voltage() == 2; });
    REQUIRE(s);
    CHECK(s->voltage == 2);

    // nothing more arrives: Timeout after the limit, with the idle hook advancing the clock
    const std::uint32_t start = clk.t;
    auto t = ep.wait_for<tel::Status>(30);
    REQUIRE_FALSE(t);
    CHECK(t.error() == gmsg::Error::Timeout);
    CHECK(clk.t - start == 30);

    // timeout 0 still takes a message that is buffered or readable now
    const auto lf = frame_of(log_msg(9));
    gmsg::SpanSource src2(bv(lf));
    auto ep2 = Link::make_endpoint(src2, gmsg::NoSink{}, TickClock{&clk});
    auto l = ep2.wait_for<tel::Log>(0);
    REQUIRE(l);
    CHECK(l->level == 9);
}

TEST_CASE("endpoint wait_for(): match with the header, wait_for_reply() returns it") {
    Link::Header h;
    h.msg_id = tel::MsgId::status;
    std::array<std::uint8_t, Link::max_frame> buf{};
    const auto n = Link::encode(h, status(7), buf);
    REQUIRE(n);
    const std::vector<std::uint8_t> w = cat({frame_of(status(6)), std::vector<std::uint8_t>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n))});
    gmsg::SpanSource src(bv(w));
    Ticks clk;
    auto ep = Link::make_endpoint(src, gmsg::NoSink{}, TickClock{&clk});
    int headers = 0;
    auto r = ep.wait_for_reply<tel::Status>(10, [&](const Link::HeaderView& hv, const tel::StatusView& v) {
        ++headers;
        CHECK(hv.msg_id() == tel::MsgId::status);
        return v.voltage() == 7;
    });
    REQUIRE(r);
    CHECK(headers == 2);
    CHECK(r->message.voltage == 7);
    CHECK(r->header.msg_id == tel::MsgId::status);
    CHECK(r->header.len == frame_of(status(7)).size() - 2 - 3 - 2);  // sync, header, crc
}

TEST_CASE("endpoint transact(): request out, reply back through a simulated device") {
    // the "device" answers every Command with a Status whose voltage is the opcode; a stale Status is pending
    gmsg::ByteRing<256> rx;
    const auto stale = frame_of(status(999));
    rx.push(bv(stale));
    auto device = [&](gmsg::bytes_view b) -> std::size_t {
        auto m = Link::decode(b);
        REQUIRE(m);
        rx.push(bv(frame_of(status(std::get<tel::Command>(*m).opcode))));
        return b.size();
    };
    Ticks clk;
    auto ep = Link::make_endpoint(rx, gmsg::sink_fn(device), TickClock{&clk});
    ep.set_idle(&Ticks::idle, &clk);
    Seen seen;
    seen.attach(ep);
    auto r = ep.transact<tel::Status>(command(17), 20);
    REQUIRE(r);
    CHECK(r->voltage == 17);  // not the stale 999, which went to the listener first
    CHECK(seen.events == std::vector<std::string>{"status 999"});

    // a reply that never comes
    auto silent = Link::make_endpoint(gmsg::NoSource{}, gmsg::sink_fn([](gmsg::bytes_view b) { return b.size(); }),
                                      TickClock{&clk});
    silent.set_idle(&Ticks::idle, &clk);
    auto t = silent.transact<tel::Status>(command(1), 5);
    REQUIRE_FALSE(t);
    CHECK(t.error() == gmsg::Error::Timeout);
    // a request that cannot be sent is not waited for
    auto blocked = Link::make_endpoint(gmsg::NoSource{}, gmsg::NoSink{}, TickClock{&clk});
    const auto before = clk.t;
    CHECK(blocked.transact<tel::Status>(command(1), 5).error() == gmsg::Error::BufferFull);
    CHECK(clk.t == before);
    // header overload
    Link::Header h;
    auto r2 = ep.transact<tel::Status>(h, command(23), 20);
    REQUIRE(r2);
    CHECK(r2->voltage == 23);
}

TEST_CASE("endpoint with std::chrono::steady_clock and a producer thread") {
    gmsg::ByteRing<512> rx;
    auto ep = Link::make_endpoint(rx, gmsg::NoSink{}, std::chrono::steady_clock{});
    ep.set_idle(+[] { std::this_thread::yield(); });
    std::atomic<bool> go{false};
    std::thread producer([&] {
        while (!go.load()) std::this_thread::yield();
        for (std::uint16_t i = 1; i <= 20; ++i) {
            const auto f = frame_of(status(i));
            for (auto b : f)
                while (!rx.push(b)) std::this_thread::yield();
        }
    });
    int got = 0;
    ep.on(+[](int* n, const tel::StatusView&) { ++*n; }, &got);
    go = true;
    auto last = ep.wait_for<tel::Status>(std::chrono::seconds(5), [](const tel::StatusView& v) { return v.voltage() == 20; });
    producer.join();
    REQUIRE(last);
    CHECK(last->voltage == 20);
    CHECK(got == 19);

    const auto t0 = std::chrono::steady_clock::now();
    auto t = ep.wait_for<tel::Status>(std::chrono::milliseconds(20));
    CHECK(t.error() == gmsg::Error::Timeout);
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(20));
    ep.set_idle_timeout(std::chrono::milliseconds(5));
}

TEST_CASE("endpoint on a text protocol (NMEA)") {
    const std::string gga = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    const std::string txt = "$GPTXT,01,01,02,ANTSTATUS=OK*3B\r\n";
    const std::string in = gga + "noise" + txt;
    gmsg::SpanSource src(gmsg::as_bytes(in.data(), in.size()), 9);
    std::array<std::uint8_t, 128> out{};
    gmsg::SpanSink sink(out);
    Ticks clk;
    auto ep = nm::Nmea::make_endpoint(src, sink, TickClock{&clk});
    int ggas = 0;
    ep.on(+[](int* n, const nm::GGAView&) { ++*n; }, &ggas);
    auto t = ep.wait_for<nm::TXT>(10);
    REQUIRE(t);
    CHECK(ggas == 1);
    REQUIRE(ep.send(*t));
    CHECK(std::string(reinterpret_cast<const char*>(sink.written().data()), sink.written().size()) == txt);
}

TEST_CASE("FileSource / FileSink") {
    std::FILE* f = std::tmpfile();
    REQUIRE(f);
    {
        auto ep = Link::make_endpoint(gmsg::NoSource{}, gmsg::FileSink(f));
        REQUIRE(ep.send(status(11)));
        REQUIRE(ep.send(command(12)));
    }
    std::rewind(f);
    auto ep = Link::make_endpoint(gmsg::FileSource(f), gmsg::NoSink{});
    Seen seen;
    seen.attach(ep);
    ep.service();
    CHECK(seen.events == std::vector<std::string>{"status 11", "command 12"});
    std::fclose(f);
}

TEST_CASE("StreamParser push_with / push_gap / space") {
    Link::Stream<64> p;
    CHECK(p.space() == 64);
    const auto a = frame_of(status(1));
    const auto b = frame_of(status(2));
    std::size_t offered = 0;
    CHECK(p.push_with([&](gmsg::mut_bytes free) {
        offered = free.size();
        std::copy(a.begin(), a.end(), free.begin());
        return a.size();
    }) == a.size());
    CHECK(offered == 64);
    CHECK(p.space() == 64 - a.size());
    // half of b, then input was lost: a is delivered, the half frame is dropped as an overrun, then b arrives whole
    p.push(bv(b).first(4));
    CHECK(p.push_gap());
    CHECK(p.push_gap());  // same position: nothing to add
    p.push(bv(b));
    CHECK_FALSE(p.push_gap());  // a second gap while the first is pending
    std::vector<std::string> ev;
    auto h = gmsg::overloaded{[&](const tel::StatusView& v) { ev.push_back("status " + std::to_string(v.voltage())); },
                              [&](const gmsg::FrameError& e) { ev.push_back(std::string(gmsg::to_string(e.error))); }};
    while (p.next(h)) {}
    CHECK(ev == std::vector<std::string>{"status 1", "overrun", "status 2"});
    CHECK(p.stats().overruns == 1);
}

namespace {
/// A simulated circular DMA: write(bytes) stores at the DMA position and raises the half / complete events.
template <std::size_t N>
struct FakeDma {
    volatile std::uint8_t buf[N] = {};
    std::size_t pos = 0;
    std::function<void()> on_half;
    void write(const std::vector<std::uint8_t>& bytes) {
        for (auto b : bytes) {
            buf[pos] = b;
            pos = (pos + 1) % N;
            if ((pos == N / 2 || pos == 0) && on_half) on_half();
        }
    }
};
} // namespace

TEST_CASE("DmaRingSource: reads a circular DMA buffer across the wrap") {
    FakeDma<32> dma;
    auto src = gmsg::dma_source(dma.buf, [&] { return dma.pos; });
    auto ep = Link::make_endpoint(src, gmsg::NoSink{});
    Seen seen;
    seen.attach(ep);
    for (std::uint16_t i = 1; i <= 6; ++i) {  // frames of 21 bytes in a 32-byte buffer: wraps every time
        dma.write(frame_of(status(i)));
        ep.service();
    }
    CHECK(seen.events == std::vector<std::string>{"status 1", "status 2", "status 3", "status 4", "status 5", "status 6"});
    CHECK(src.overruns() == 0);
}

TEST_CASE("DmaRingSource with lap tracking reports an overrun") {
    FakeDma<32> dma;
    gmsg::DmaRingSource<32, std::function<std::size_t()>> src(dma.buf, [&] { return dma.pos; }, true);
    dma.on_half = [&] { src.on_half(); };
    auto ep = Link::make_endpoint(src, gmsg::NoSink{});
    Seen seen;
    seen.attach(ep);
    const auto f1 = frame_of(status(1));
    dma.write(f1);
    ep.service();
    dma.write(cat({frame_of(status(2)), frame_of(status(3))}));  // 42 bytes: the DMA laps unread input
    ep.service();
    CHECK(src.overruns() == 1);
    CHECK(ep.stats().overruns == 1);  // reported by the same service() call
    dma.write(frame_of(status(4)));
    ep.service();
    // everything buffered at the lap is dropped; the overrun is reported (no frame was in progress)
    CHECK(seen.events == std::vector<std::string>{"status 1", "error overrun", "status 4"});
    CHECK(ep.stats().overruns == 1);
    CHECK(src.unread() == 0);
}

TEST_CASE("DmaRingSource: a lap in the middle of a frame drops it as an overrun") {
    FakeDma<32> dma;
    gmsg::DmaRingSource<32, std::function<std::size_t()>> src(dma.buf, [&] { return dma.pos; }, true);
    dma.on_half = [&] { src.on_half(); };
    auto ep = Link::make_endpoint(src, gmsg::NoSink{});
    Seen seen;
    seen.attach(ep);
    const auto f = frame_of(status(1));
    dma.write(std::vector<std::uint8_t>(f.begin(), f.begin() + 8));  // start of a frame, read by the endpoint
    ep.service();
    dma.write(std::vector<std::uint8_t>(40, 0x00));  // more than the buffer: the rest of the frame is lost
    ep.service();
    dma.write(frame_of(status(2)));
    ep.service();
    CHECK(src.overruns() == 1);
    REQUIRE(seen.events.size() >= 2);
    CHECK(seen.events.front() == "error overrun");
    CHECK(seen.events.back() == "status 2");
}

// ---- requests that do not block (gmsg::Pending) ----------------------------------------------------------------

namespace {
struct Done {
    std::vector<std::string> log;
    void on_status(const gmsg::Result<tel::Status>& r) {
        log.push_back(r ? "reply " + std::to_string(r->voltage) : std::string("error ") + std::string(gmsg::to_string(r.error())));
    }
};
} // namespace

TEST_CASE("request(): returns at once; service() completes it with the reply, callback runs once") {
    gmsg::ByteRing<256> rx;
    std::vector<std::vector<std::uint8_t>> sent;
    auto sink = gmsg::sink_fn([&](gmsg::bytes_view b) {
        sent.emplace_back(b.begin(), b.end());
        return b.size();
    });
    Ticks clk;
    auto ep = Link::make_endpoint(rx, sink, TickClock{&clk});
    Seen seen;
    seen.attach(ep);
    rx.push(bv(frame_of(status(999))));  // old: delivered by request() before it sends
    Done done;
    gmsg::Pending<tel::Status> p;
    p.on_done<&Done::on_status>(done);
    CHECK(p.state() == gmsg::RequestState::idle);
    REQUIRE(ep.request(p, command(17), 50) == gmsg::Error::None);
    CHECK(sent.size() == 1);
    CHECK(p.waiting());
    CHECK(ep.pending() == 1);
    CHECK(seen.events == std::vector<std::string>{"status 999"});
    ep.service();
    CHECK(p.waiting());  // nothing yet
    rx.push(bv(cat({frame_of(log_msg(1)), frame_of(status(17)), frame_of(status(18))})));
    ep.service();
    REQUIRE(p.done());
    CHECK(p.result()->voltage == 17);
    CHECK(done.log == std::vector<std::string>{"reply 17"});
    CHECK(seen.events == std::vector<std::string>{"status 999", "log 1", "status 18"});  // the reply is not delivered
    CHECK(ep.pending() == 0);
    ep.service();
    CHECK(done.log.size() == 1);
}

TEST_CASE("request(): timeout, cancel, restart, send failure, several at once, match") {
    gmsg::ByteRing<256> rx;
    Ticks clk;
    auto ep = Link::make_endpoint(rx, gmsg::sink_fn([](gmsg::bytes_view b) { return b.size(); }), TickClock{&clk});
    Seen seen;
    seen.attach(ep);
    Done done;
    gmsg::Pending<tel::Status> a;
    a.on_done<&Done::on_status>(done);
    REQUIRE(ep.request(a, command(1), 10) == gmsg::Error::None);
    clk.t = 9;
    ep.service();
    CHECK(a.waiting());
    clk.t = 10;
    ep.service();
    REQUIRE(a.done());
    CHECK(a.result().error() == gmsg::Error::Timeout);
    CHECK(done.log == std::vector<std::string>{"error timeout"});
    // cancel: no callback; restart works
    REQUIRE(ep.request(a, command(1), 10) == gmsg::Error::None);
    a.cancel();
    CHECK(a.state() == gmsg::RequestState::idle);
    CHECK(ep.pending() == 0);
    clk.t = 100;
    ep.service();
    CHECK(done.log.size() == 1);
    // send failure: the request stays idle
    auto blocked = Link::make_endpoint(gmsg::NoSource{}, gmsg::NoSink{}, TickClock{&clk});
    CHECK(blocked.request(a, command(1), 10) == gmsg::Error::BufferFull);
    CHECK(a.state() == gmsg::RequestState::idle);
    // several at once, oldest first; a match that waits for one voltage; a Command expected without sending
    int want = 7;
    auto by_voltage = [&want](const tel::StatusView& v) { return v.voltage() == want; };
    gmsg::Pending<tel::Status> first, second;
    gmsg::Pending<tel::Status, decltype(by_voltage)> seven(by_voltage);
    gmsg::Pending<tel::Command> cmd;
    REQUIRE(ep.request(seven, command(2), 100) == gmsg::Error::None);
    REQUIRE(ep.request(first, command(3), 100) == gmsg::Error::None);
    REQUIRE(ep.request(second, command(4), 100) == gmsg::Error::None);
    ep.expect(cmd, 100);
    CHECK(ep.pending() == 4);
    rx.push(bv(cat({frame_of(status(5)), frame_of(status(7)), frame_of(status(6)), frame_of(command(8)), frame_of(status(9))})));
    ep.service();
    CHECK(seven.result()->voltage == 7);
    CHECK(first.result()->voltage == 5);
    CHECK(second.result()->voltage == 6);
    CHECK(cmd.result()->opcode == 8);
    CHECK(seen.events.back() == "status 9");
    // a callback that starts the next request
    struct Chain {
        decltype(ep)* e;
        gmsg::Pending<tel::Status>* p;
        int replies = 0;
        void on(const gmsg::Result<tel::Status>& r) {
            if (r && ++replies < 3) (void)e->request(*p, command(static_cast<std::uint16_t>(r->voltage + 1)), 100);
        }
    } chain{&ep, &first};
    first.on_done<&Chain::on>(chain);
    REQUIRE(ep.request(first, command(1), 100) == gmsg::Error::None);
    rx.push(bv(cat({frame_of(status(1)), frame_of(status(2)), frame_of(status(3))})));
    ep.service();
    CHECK(chain.replies == 3);
    // a request destroyed while waiting leaves the endpoint
    {
        gmsg::Pending<tel::Log> gone;
        ep.expect(gone, 100);
        CHECK(ep.pending() == 1);
    }
    CHECK(ep.pending() == 0);
}

TEST_CASE("requests are served while a blocking wait_for runs, and outlive no endpoint") {
    gmsg::ByteRing<256> rx;
    Ticks clk;
    gmsg::Pending<tel::Log> log_req;
    {
        auto ep = Link::make_endpoint(rx, gmsg::NoSink{}, TickClock{&clk});
        ep.set_idle(&Ticks::idle, &clk);
        gmsg::Pending<tel::Status> st;
        ep.expect(st, 5);
        ep.expect(log_req, 1000);
        rx.push(bv(cat({frame_of(log_msg(3)), frame_of(command(4))})));
        auto c = ep.wait_for<tel::Command>(100);
        REQUIRE(c);
        CHECK(log_req.result()->level == 3);  // taken by the request during the wait
        CHECK(st.waiting());
        CHECK_FALSE(ep.wait_for<tel::Command>(10));  // the Status request times out meanwhile
        CHECK(st.result().error() == gmsg::Error::Timeout);
        ep.expect(log_req, 1000);
    }
    CHECK(log_req.state() == gmsg::RequestState::idle);  // the endpoint went away first
}
