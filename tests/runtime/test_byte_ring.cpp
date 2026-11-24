// gmsg::ByteRing and StreamParser::feed(ring) / push(ring): wrapping, overflow, overrun reporting, threads.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "gmp/byte_ring.hpp"
#include "gmp/checksum.hpp"
#include "gmp/framing/length_prefixed.hpp"
#include "gmp/stream_parser.hpp"

using namespace gmsg;

namespace {

using Bytes = std::vector<std::uint8_t>;

// [AA][len u8][payload...][crc8 over len+payload], as in test_stream.cpp
struct RingProto {
    static constexpr LengthPrefixedConfig cfg{Pattern{{0xAA}, 1}, 1, 1, Endian::big, 3, 3, 40};
    static FrameScan scan(bytes_view b) { return scan_length_prefixed(b, cfg); }

    template <class H>
    static Error process(mut_bytes f, H& h) {
        const auto covered = bytes_view(f).subspan(1, f.size() - 2);
        if (checksum::crc8::compute(covered) != f.back()) return Error::BadChecksum;
        h(bytes_view(f).subspan(2, f.size() - 3));
        return Error::None;
    }
};

Bytes make_frame(const Bytes& payload) {
    Bytes f{0xAA, static_cast<std::uint8_t>(payload.size())};
    f.insert(f.end(), payload.begin(), payload.end());
    f.push_back(checksum::crc8::compute(bytes_view(f).subspan(1)));
    return f;
}

/// Frame number `id` with a payload of `len` bytes: [id lo, id hi, filler...] (no 0xAA inside).
Bytes numbered_frame(std::uint32_t id, std::size_t len) {
    Bytes p(std::max<std::size_t>(len, 2), 0x11);
    p[0] = static_cast<std::uint8_t>(id & 0x7F);
    p[1] = static_cast<std::uint8_t>((id >> 7) & 0x7F);
    return make_frame(p);
}
std::uint32_t id_of(const Bytes& payload) { return payload[0] | (std::uint32_t{payload[1]} << 7); }

struct Events {
    std::vector<Bytes> frames;
    std::vector<Error> errors;
    std::vector<Bytes> error_data;
    void operator()(bytes_view p) { frames.emplace_back(p.begin(), p.end()); }
    void operator()(const FrameError& e) {
        errors.push_back(e.error);
        error_data.emplace_back(e.data.begin(), e.data.end());
    }
};

void append(Bytes& to, const Bytes& b) { to.insert(to.end(), b.begin(), b.end()); }

} // namespace

TEST_CASE("byte ring: push, wrap, readable parts, full, drop and overrun") {
    ByteRing<8> r;
    CHECK(r.capacity() == 8);
    CHECK(r.empty());
    CHECK(r.readable().empty());
    for (std::uint8_t i = 0; i < 6; ++i) CHECK(r.push(i));
    CHECK(r.size() == 6);
    CHECK(r.readable().size() == 6);
    r.consume(5);
    CHECK(r.size() == 1);
    // 7 more: 3 fit before the end of storage, 4 wrap to the start
    const Bytes more{10, 11, 12, 13, 14, 15, 16};
    CHECK(r.push(bytes_view(more)) == 7);
    CHECK(r.size() == 8);
    CHECK(r.high_water() == 8);
    CHECK(r.readable().size() == 3);  // 5, 10, 11 up to the end of the storage
    CHECK(r.readable()[0] == 5);
    std::array<std::uint8_t, 16> out{};
    CHECK(r.read(out) == 8);
    CHECK(Bytes(out.begin(), out.begin() + 8) == Bytes{5, 10, 11, 12, 13, 14, 15, 16});
    CHECK(r.empty());
    CHECK_FALSE(r.overrun_pending());

    // fill, then overflow: the newest bytes are dropped and the ring keeps dropping until cleared
    for (std::uint8_t i = 0; i < 8; ++i) CHECK(r.push(i));
    CHECK_FALSE(r.push(99));
    CHECK(r.overrun_pending());
    CHECK(r.dropped() == 1);
    r.consume(4);  // room again, but the overrun is still waiting
    CHECK_FALSE(r.push(98));
    CHECK(r.push(bytes_view(more)) == 0);
    CHECK(r.dropped() == 9);
    CHECK(r.size() == 4);
    r.consume(4);
    r.clear_overrun();
    CHECK(r.push(7));
    CHECK(r.size() == 1);
    // a partial span push: what fits goes in, the rest is dropped
    const Bytes big(10, 0x33);
    CHECK(r.push(bytes_view(big)) == 7);
    CHECK(r.overrun_pending());
    CHECK(r.dropped() == 12);
}

TEST_CASE("byte ring: a narrow index wraps correctly") {
    ByteRing<128, std::uint8_t> r;  // the largest ring an 8-bit index allows
    std::uint8_t next_in = 0, next_out = 0;
    std::mt19937 rng(7);
    for (int round = 0; round < 5000; ++round) {
        const std::size_t burst = rng() % 100;
        for (std::size_t i = 0; i < burst && r.size() < 128; ++i) CHECK(r.push(next_in++));
        const std::size_t take = rng() % 120;
        std::array<std::uint8_t, 128> out{};
        const std::size_t n = r.read(std::span<std::uint8_t>(out.data(), take));
        for (std::size_t i = 0; i < n; ++i) REQUIRE(out[i] == next_out++);
    }
    CHECK_FALSE(r.overrun_pending());
    CHECK(r.high_water() <= 128);
}

TEST_CASE("feed(ring) gives the same frames and errors as feed(bytes)") {
    Bytes stream;
    std::mt19937 rng(99);
    for (std::uint32_t i = 0; i < 300; ++i) {
        Bytes f = numbered_frame(i, rng() % 12);
        if (rng() % 9 == 0) f.back() ^= 0x5A;               // bad checksum
        if (rng() % 11 == 0) stream.insert(stream.end(), {1, 2, 3});  // garbage
        append(stream, f);
    }
    Events direct;
    {
        StreamParser<RingProto, 40> sp;
        sp.feed(bytes_view(stream), direct);
    }
    for (const std::size_t max : {std::size_t{1}, std::size_t{5}, std::size_t{64}, static_cast<std::size_t>(-1)}) {
        ByteRing<64> ring;
        StreamParser<RingProto, 40> sp;
        Events ev;
        std::size_t off = 0;
        while (off < stream.size()) {
            const std::size_t burst = std::min<std::size_t>(1 + rng() % 40, stream.size() - off);
            const std::size_t pushed = ring.push(bytes_view(stream.data() + off, std::min(burst, 64 - ring.size())));
            off += pushed;
            sp.feed(ring, max, ev);
        }
        while (!ring.empty()) sp.feed(ring, max, ev);
        INFO("max bytes per feed " << max);
        CHECK(ring.dropped() == 0);
        CHECK(ev.frames == direct.frames);
        // garbage is reported per arrival (BadSync events depend on chunk boundaries, as with feed(bytes))
        const auto bad = [](const Events& e) { return std::count(e.errors.begin(), e.errors.end(), Error::BadChecksum); };
        CHECK(bad(ev) == bad(direct));
        CHECK(sp.stats().overruns == 0);
    }
}

TEST_CASE("an overrun drops the frame in progress and reports it; frames before and after the gap arrive") {
    const Bytes a = numbered_frame(1, 3), b = numbered_frame(2, 3), c = numbered_frame(3, 6), d = numbered_frame(4, 3);
    REQUIRE(a.size() + b.size() == 12);
    ByteRing<16> ring;
    StreamParser<RingProto, 40> sp;
    Events ev;
    // A + B + the first 4 bytes of C fill the ring; the rest of C is dropped, and so is D while the overrun waits
    CHECK(ring.push(bytes_view(a)) == a.size());
    CHECK(ring.push(bytes_view(b)) == b.size());
    CHECK(ring.push(bytes_view(c)) == 4);
    CHECK(ring.push(bytes_view(d)) == 0);
    CHECK(ring.dropped() == (c.size() - 4) + d.size());
    sp.feed(ring, ev);
    REQUIRE(ev.frames.size() == 2);
    CHECK(id_of(ev.frames[0]) == 1);
    CHECK(id_of(ev.frames[1]) == 2);
    REQUIRE(ev.errors == std::vector<Error>{Error::Overrun});
    CHECK(ev.error_data[0] == Bytes(c.begin(), c.begin() + 4));
    CHECK(sp.stats().overruns == 1);
    CHECK(sp.last_error() == Error::Overrun);
    CHECK(sp.buffered() == 0);
    CHECK_FALSE(ring.overrun_pending());
    // input flows again
    CHECK(ring.push(bytes_view(d)) == d.size());
    sp.feed(ring, ev);
    REQUIRE(ev.frames.size() == 3);
    CHECK(id_of(ev.frames[2]) == 4);
}

TEST_CASE("an overrun between frames is reported with no bytes") {
    const Bytes a = numbered_frame(1, 6);  // 9 bytes
    ByteRing<8> ring;
    StreamParser<RingProto, 40> sp;
    Events ev;
    CHECK(ring.push(bytes_view(a).first(8)) == 8);
    sp.feed(ring, ev);  // 8 bytes of A in the parser, ring empty
    CHECK(ring.push(bytes_view(a).subspan(8)) == 1);
    sp.feed(ring, ev);
    REQUIRE(ev.frames.size() == 1);
    // now a gap exactly at a frame boundary
    for (int i = 0; i < 8; ++i) CHECK(ring.push(0xAA));
    CHECK_FALSE(ring.push(0x01));
    ring.consume(8);  // the consumer discards the 8 sync bytes itself
    sp.feed(ring, ev);
    CHECK(ev.errors == std::vector<Error>{Error::Overrun});
    CHECK(ev.error_data[0].empty());
}

TEST_CASE("push(ring) without processing takes the gap over only after the bytes before it") {
    const Bytes a = numbered_frame(1, 2), c = numbered_frame(3, 10);
    ByteRing<16> ring;
    StreamParser<RingProto, 40> sp;
    CHECK(ring.push(bytes_view(a)) == a.size());
    CHECK(ring.push(bytes_view(c)) == 16 - a.size());
    CHECK_FALSE(ring.push(0x00));
    // a parser buffer smaller than the ring content needs two pushes before the gap is taken over
    CHECK(sp.push(ring, 10) == 10);
    CHECK(ring.overrun_pending());
    CHECK(sp.push(ring) == 6);
    CHECK_FALSE(ring.overrun_pending());

    Events ev;
    while (sp.next(ev)) {}
    REQUIRE(ev.frames.size() == 1);
    REQUIRE(ev.errors == std::vector<Error>{Error::Overrun});
    CHECK(ev.error_data[0].size() == 16 - a.size());
}

/// A gap taken over by push(ring); the next frame then arrives (push(bytes)) before the gap was reported. The scan
/// must stop at the gap (Capacity 40) and the gap must move with the buffer when it is compacted (Capacity 16).
template <std::size_t Capacity>
void check_bytes_after_gap() {
    const Bytes a = numbered_frame(1, 2), b = numbered_frame(2, 2), c = numbered_frame(3, 6), d = numbered_frame(4, 2);
    ByteRing<16> ring;
    StreamParser<RingProto, Capacity> sp;
    Events ev;
    CHECK(ring.push(bytes_view(a)) == 5);
    CHECK(ring.push(bytes_view(b)) == 5);
    CHECK(ring.push(bytes_view(c)) == 6);  // 3 bytes of C dropped
    CHECK(sp.push(ring) == 16);
    CHECK(sp.next(ev));  // A
    CHECK(sp.push(bytes_view(d)) == d.size());  // Capacity 16: compacts (head 5, tail 16)
    while (sp.next(ev)) {}
    REQUIRE(ev.frames.size() == 3);
    CHECK(id_of(ev.frames[0]) == 1);
    CHECK(id_of(ev.frames[1]) == 2);
    CHECK(id_of(ev.frames[2]) == 4);
    CHECK(ev.errors == std::vector<Error>{Error::Overrun});
    CHECK(ev.error_data[0] == Bytes(c.begin(), c.begin() + 6));
}

TEST_CASE("bytes pushed after a taken-over gap: the scan stops at the gap, compaction moves it") {
    check_bytes_after_gap<40>();
    check_bytes_after_gap<16>();
}

TEST_CASE("flush and resync with an overrun waiting") {
    const Bytes c = numbered_frame(3, 10);  // 13 bytes
    ByteRing<8> ring;
    StreamParser<RingProto, 40> sp;
    Events ev;
    CHECK(ring.push(bytes_view(c)) == 8);
    CHECK(sp.push(ring) == 8);  // the gap is now in the parser
    CHECK_FALSE(sp.resync(ev));  // nothing to give up: the next step reports the overrun
    // bytes pushed directly after the gap
    const Bytes tail{0x01, 0x02};
    CHECK(sp.push(bytes_view(tail)) == 2);
    CHECK(sp.flush(ev));
    REQUIRE(ev.errors == (std::vector<Error>{Error::Overrun, Error::Truncated}));
    CHECK(ev.error_data[0].size() == 8);
    CHECK(ev.error_data[1] == tail);
    CHECK(sp.stats().overruns == 1);
    CHECK(sp.stats().flushes == 1);
    CHECK(sp.buffered() == 0);
}

TEST_CASE("feed(ring) with a stored handler") {
    const Bytes a = numbered_frame(5, 4);
    ByteRing<32> ring;
    StreamParser<RingProto, 40, Events> sp;
    ring.push(bytes_view(a));
    ring.push(bytes_view(a));
    CHECK(sp.feed(ring) == 2);
    CHECK(sp.handler().frames.size() == 2);
    ring.push(bytes_view(a));
    CHECK(sp.feed(ring, std::size_t{3}) == 0);  // only 3 bytes taken
    CHECK(ring.size() == a.size() - 3);
    CHECK(sp.feed(ring) == 1);
}

/// Frames 0..count-1 with payload lengths i % 9 (at least 2).
Bytes thread_stream(std::uint32_t count) {
    Bytes s;
    for (std::uint32_t i = 0; i < count; ++i) append(s, numbered_frame(i, i % 9));
    return s;
}
bool frame_is(const Bytes& payload, std::uint32_t id) {
    if (payload.size() != std::max<std::size_t>(2, id % 9) || id_of(payload) != id) return false;
    return std::all_of(payload.begin() + 2, payload.end(), [](std::uint8_t b) { return b == 0x11; });
}

TEST_CASE("byte ring between two threads: every byte arrives in order when the producer waits") {
    const std::uint32_t count = 16000;
    const Bytes stream = thread_stream(count);
    ByteRing<64> ring;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        std::mt19937 rng(3);
        std::size_t off = 0;
        while (off < stream.size()) {
            const std::size_t n = std::min<std::size_t>(1 + rng() % 24, stream.size() - off);
            off += ring.push(bytes_view(stream.data() + off, std::min(n, 64 - ring.size())));
        }
        done.store(true, std::memory_order_release);
    });
    StreamParser<RingProto, 40> sp;
    Events ev;
    while (!done.load(std::memory_order_acquire) || !ring.empty()) sp.feed(ring, std::size_t{50}, ev);
    producer.join();
    CHECK(ring.dropped() == 0);
    CHECK(ev.errors.empty());
    REQUIRE(ev.frames.size() == count);
    for (std::uint32_t i = 0; i < count; ++i) REQUIRE(frame_is(ev.frames[i], i));
}

TEST_CASE("byte ring between two threads: with drops, delivered frames are intact and in order") {
    const std::uint32_t count = 16000;
    const Bytes stream = thread_stream(count);
    ByteRing<32> ring;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (const std::uint8_t b : stream) ring.push(b);  // never waits: drops when full
        done.store(true, std::memory_order_release);
    });
    StreamParser<RingProto, 40> sp;
    Events ev;
    std::mt19937 rng(5);
    while (!done.load(std::memory_order_acquire) || !ring.empty() || ring.overrun_pending()) {
        sp.feed(ring, std::size_t{1 + rng() % 16}, ev);
        if (rng() % 4 == 0) std::this_thread::yield();
    }
    producer.join();
    sp.feed(ring, ev);
    // every delivered frame is one that was sent, unchanged, in sending order
    std::int64_t last = -1;
    for (const Bytes& p : ev.frames) {
        const std::uint32_t id = id_of(p);
        REQUIRE(static_cast<std::int64_t>(id) > last);
        REQUIRE(frame_is(p, id));
        last = id;
    }
    const auto overruns = std::count(ev.errors.begin(), ev.errors.end(), Error::Overrun);
    CHECK(sp.stats().overruns == static_cast<std::uint32_t>(overruns));
    CHECK((ring.dropped() > 0) == (overruns > 0));
    CHECK(sp.stats().frames == ev.frames.size());
    INFO("dropped " << ring.dropped() << ", overruns " << overruns << ", frames " << ev.frames.size());
    CHECK(ev.frames.size() + 0 <= count);
}
