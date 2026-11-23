// Cross-check of generated C protocols against generated C++ protocols, for every protocol the C backend supports
// (all framing types, escaping styles, wire checksums and wire-counted lengths).
//
// For each protocol: valid frames are built by the C++ encoder from messages found by decoding random bodies.
// Whole-buffer: parse / decode / process / encode must agree on valid, truncated, corrupted and random input
// (error, message kind, position and size of the message bytes, encoded bytes).
// Stream: noisy byte streams (garbage, corrupted and truncated frames) fed in chunks of several sizes must give
// the same sequence of delivered frames (kind, header and message bytes) and errors, and the same statistics.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

// generated C++
#include "print_compare.hpp"
#include "can.gmp.hpp"
#include "custom.gmp.hpp"
#include "hdlc.gmp.hpp"
#include "framing.gmp.hpp"
#include "motor_link.gmp.hpp"
#include "nmea.gmp.hpp"
#include "sht4x.gmp.hpp"
#include "sht4x_enum.gmp.hpp"
#include "stuffed.gmp.hpp"
#include "telemetry.gmp.hpp"
// generated C
#include "can.gmp.h"
#include "custom.gmp.h"
#include "hdlc.gmp.h"
#include "framing.gmp.h"
#include "motor_link.gmp.h"
#include "nmea.gmp.h"
#include "sht4x.gmp.h"
#include "sht4x_enum.gmp.h"
#include "stuffed.gmp.h"
#include "telemetry.gmp.h"

namespace {

using Bytes = std::vector<std::uint8_t>;

int code(gmp::Error e) { return static_cast<int>(e); }

/// First members of every generated C view: { data, size }.
struct Head {
    const std::uint8_t* data;
    std::size_t size;
};
Head head_of(const void* view) {
    Head h;
    std::memcpy(&h, view, sizeof h);
    return h;
}

Bytes bytes_of(const std::uint8_t* p, std::size_t n) { return Bytes(p, p + n); }

/// The bytes a C++ message view covers (binary views: data(); text views: raw()).
template <class V>
Bytes view_bytes(const V& v) {
    if constexpr (requires { v.data(); }) {
        return bytes_of(v.data().data(), v.data().size());
    } else {
        const auto r = v.raw();
        return bytes_of(reinterpret_cast<const std::uint8_t*>(r.data()), r.size());
    }
}

template <class V>
const std::uint8_t* view_ptr(const V& v) {
    if constexpr (requires { v.data(); }) return v.data().data();
    else return reinterpret_cast<const std::uint8_t*>(v.raw().data());
}

/// One delivered frame or reported error, comparable across the two languages.
struct Event {
    int type = 0;     // 0 frame, 1 error
    int kind = 0;     // frame: message index (0-based); error: error code
    Bytes header;     // frame: header view bytes
    Bytes message;    // frame: message view bytes; error: dropped bytes
    bool operator==(const Event&) const = default;
};

// ---- C side, generic over a protocol's generated functions ---------------------------------------------------
#define C_PROTOCOL_VIEWS(CPP, CP) \
    using Cpp = CPP; \
    using AnyView = CP##_any_view; \
    using Callbacks = CP##_callbacks; \
    static gmsg_error parse(std::uint8_t* d, std::size_t n, AnyView* v) { return CP##_parse(d, n, v); } \
    static gmsg_error process(std::uint8_t* d, std::size_t n, const Callbacks* cb) { return CP##_process(d, n, cb); } \
    static void print_view(const AnyView* v, gmsg_printer* p) { CP##_any_view_print(v, p); }

#define C_PROTOCOL_STRUCTS(CP) \
    static constexpr bool structs = true; \
    using AnyMessage = CP##_any_message; \
    static gmsg_error decode(std::uint8_t* d, std::size_t n, AnyMessage* m) { return CP##_decode(d, n, m); } \
    static void print_message(const AnyMessage* m, gmsg_printer* p) { CP##_any_message_print(m, p); } \
    static gmsg_error encode(const AnyMessage* m, std::uint8_t* o, std::size_t c, std::size_t* w) { \
        return CP##_encode(m, o, c, w); \
    }

#define C_STREAM(CP) \
    static constexpr bool stream = true; \
    using Stream = CP##_stream; \
    static gmsg_error stream_init(Stream* s, std::uint8_t* b, std::size_t n) { return CP##_stream_init(s, b, n); } \
    static std::size_t feed(Stream* s, const std::uint8_t* d, std::size_t n) { return CP##_stream_feed(s, d, n); } \
    static bool next(Stream* s) { return CP##_stream_next(s); } \
    static bool resync(Stream* s) { return CP##_stream_resync(s); } \
    static bool flush(Stream* s) { return CP##_stream_flush(s); } \
    static void set_idle_timeout(Stream* s, std::uint32_t t) { CP##_stream_set_idle_timeout(s, t); } \
    static bool tick(Stream* s, std::uint32_t now) { return CP##_stream_tick(s, now); } \
    static const gmsg_stream_stats* stats(const Stream* s) { return CP##_stream_stats(s); } \
    using Received = CP##_received; \
    using PollEvent = CP##_event; \
    static std::size_t push(Stream* s, const std::uint8_t* d, std::size_t n) { return CP##_stream_push(s, d, n); } \
    static bool poll(Stream* s, AnyView* v) { return CP##_stream_poll(s, v); } \
    static bool poll_frame(Stream* s, Received* r) { return CP##_stream_poll_frame(s, r); } \
    static gmsg_poll poll_event(Stream* s, PollEvent* e) { return CP##_stream_poll_event(s, e); } \
    static bool poll_for(Stream* s, int kind, AnyView* v) { return CP##_stream_poll_for(s, static_cast<CP##_kind>(kind), v); }

#define C_POLL_MESSAGE(CP) \
    static bool poll_message(Stream* s, AnyMessage* m) { return CP##_stream_poll_message(s, m); }

#define C_PROTOCOL(NAME, CPP, CP) \
    struct NAME { \
        C_PROTOCOL_VIEWS(CPP, CP) \
        C_PROTOCOL_STRUCTS(CP) \
        C_STREAM(CP) \
        C_POLL_MESSAGE(CP) \
    }

/// Protocols without framing (one message per buffer) have no stream.
#define C_PROTOCOL_NO_STREAM(NAME, CPP, CP) \
    struct NAME { \
        C_PROTOCOL_VIEWS(CPP, CP) \
        C_PROTOCOL_STRUCTS(CP) \
        static constexpr bool stream = false; \
    }

/// Protocols whose messages are views only (no structs: no decode / encode).
#define C_PROTOCOL_VIEW_ONLY(NAME, CPP, CP) \
    struct NAME { \
        C_PROTOCOL_VIEWS(CPP, CP) \
        static constexpr bool structs = false; \
        C_STREAM(CP) \
    }

C_PROTOCOL(BinaryLinkC, acme::telemetry::BinaryLink, acme_telemetry_binary_link);
C_PROTOCOL_NO_STREAM(CanC, vehicle::can::Can, vehicle_can_can);
C_PROTOCOL(CanFixedC, vehicle::can::FixedLink, vehicle_can_fixed_link);
C_PROTOCOL(MotorLinkC, motor::MotorLink, motor_motor_link);
C_PROTOCOL(Sht4xMeasurementC, sensirion::sht4x::MeasurementResponse, sensirion_sht4x_measurement_response);
C_PROTOCOL(Sht4xSerialC, sensirion::sht4x::SerialNumberResponse, sensirion_sht4x_serial_number_response);
C_PROTOCOL(Sht4xEnumMeasurementC, sensirion::sht4x_enum::MeasurementResponse, sensirion_sht4x_enum_measurement_response);
C_PROTOCOL(SyncLinkC, link::stuffed::SyncLink, link_stuffed_sync_link);
C_PROTOCOL(WireChecksumC, link::stuffed::WireChecksum, link_stuffed_wire_checksum);
C_PROTOCOL(WireLengthC, link::stuffed::WireLength, link_stuffed_wire_length);
C_PROTOCOL(CrcLengthC, link::stuffed::CrcLength, link_stuffed_crc_length);
C_PROTOCOL(WireCrcLengthC, link::stuffed::WireCrcLength, link_stuffed_wire_crc_length);
C_PROTOCOL(WireCrcOverWireC, link::stuffed::WireCrcOverWire, link_stuffed_wire_crc_over_wire);
C_PROTOCOL(StuffedFixedC, link::stuffed::FixedLink, link_stuffed_fixed_link);
C_PROTOCOL(DelimitedC, link::stuffed::Delimited, link_stuffed_delimited);
C_PROTOCOL(AdjustedC, test::framing::Adjusted, test_framing_adjusted);
C_PROTOCOL(WholeFrameC, test::framing::WholeFrame, test_framing_whole_frame);
C_PROTOCOL(EscapedWithEndC, test::framing::EscapedWithEnd, test_framing_escaped_with_end);
C_PROTOCOL(FixedWithEndC, test::framing::FixedWithEnd, test_framing_fixed_with_end);
C_PROTOCOL(LooseC, test::framing::Loose, test_framing_loose);
C_PROTOCOL(ExactC, test::framing::Exact, test_framing_exact);
C_PROTOCOL(HdlcC, link::hdlc::Hdlc, link_hdlc_hdlc);
C_PROTOCOL(SummedC, link::hdlc::Summed, link_hdlc_summed);
C_PROTOCOL_VIEW_ONLY(CustomFramerC, ext::Custom, ext_custom);
C_PROTOCOL(NmeaC, nav::nmea::Nmea, nav_nmea_nmea);
C_PROTOCOL(TextPosC, ext::TextPos, ext_text_pos);

template <class P>
constexpr bool has_header = requires { typename P::HeaderView; };

/// on_frame / on_error callbacks recording into a std::vector<Event> (ctx).
template <class T, int Type = 0, class... A>
void c_on_frame(void* ctx, A... args) {
    auto* out = static_cast<std::vector<Event>*>(ctx);
    const auto* v = std::get<sizeof...(A) - 1>(std::tie(args...));
    Event ev;
    ev.type = Type;
    ev.kind = static_cast<int>(v->kind) - 1;
    const Head m = head_of(&v->as);
    ev.message = bytes_of(m.data, m.size);
    if constexpr (sizeof...(A) == 2) {
        const Head h = head_of(std::get<0>(std::tie(args...)));
        ev.header = bytes_of(h.data, h.size);
    }
    out->push_back(std::move(ev));
}

inline void c_on_error(void* ctx, gmsg_error e, const std::uint8_t* data, std::size_t n) {
    static_cast<std::vector<Event>*>(ctx)->push_back(Event{1, static_cast<int>(e), {}, bytes_of(data, n)});
}

template <class T, int Type = 0>
typename T::Callbacks recording_callbacks(std::vector<Event>& out) {
    typename T::Callbacks cb;
    std::memset(&cb, 0, sizeof cb);
    cb.ctx = &out;
    cb.on_frame = c_on_frame<T, Type>;
    cb.on_error = c_on_error;
    return cb;
}

// ---- C++ side -------------------------------------------------------------------------------------------------
template <class P, int Type = 0>
auto cpp_recorder(std::vector<Event>& out) {
    auto record = [&out](const auto& v) {
        Event ev;
        ev.type = Type;
        ev.kind = static_cast<int>(typename P::AnyView(v).index());
        ev.message = view_bytes(v);
        out.push_back(std::move(ev));
    };
    auto error = [&out](const gmp::FrameError& e) {
        out.push_back(Event{1, code(e.error), {}, bytes_of(e.data.data(), e.data.size())});
    };
    if constexpr (has_header<P>)
        return gmp::overloaded{error, [record, &out](const typename P::HeaderView& h, const auto& v) {
                                   record(v);
                                   out.back().header = bytes_of(h.data().data(), h.data().size());
                               }};
    else
        return gmp::overloaded{error, record};
}

/// Messages of every type of the protocol, found by decoding random bodies (plus value-initialised ones).
template <class P, std::size_t... I>
std::vector<typename P::AnyMessage> sample_messages(std::mt19937& rng, std::index_sequence<I...>) {
    std::vector<typename P::AnyMessage> out;
    // escaped protocols: bytes that need escaping are common, so frames contain escape pairs
    Bytes special;
    if constexpr (requires { P::framing_config.escape; }) {
        const auto& c = P::framing_config.escape;
        if (c.enabled) {
            special.assign(c.raw.begin(), c.raw.begin() + c.count);
            special.push_back(c.esc);
        }
    }
    auto one = [&]<class M>(std::type_identity<M>) {
        out.emplace_back(M{});
        int found = 0;
        for (int k = 0; k < 200000 && found < 3; ++k) {
            Bytes b(rng() % 40);
            for (auto& x : b)
                x = static_cast<std::uint8_t>(!special.empty() && rng() % 4 == 0 ? special[rng() % special.size()]
                                              : rng() % 3 == 0                    ? rng() % 4
                                                                                  : rng());
            if (auto m = M::decode(gmp::bytes_view(b.data(), b.size()))) {
                out.emplace_back(*m);
                ++found;
            }
        }
        static const std::string text_chars = "0123456789,,,.-NE";
        for (int k = 0; k < 20000 && found < 6; ++k) {
            Bytes b(rng() % 40);
            for (auto& x : b) x = static_cast<std::uint8_t>(text_chars[rng() % text_chars.size()]);
            if (auto m = M::decode(gmp::bytes_view(b.data(), b.size()))) {
                out.emplace_back(*m);
                ++found;
            }
        }
    };
    (one(std::type_identity<std::variant_alternative_t<I, typename P::AnyMessage>>{}), ...);
    return out;
}

template <class P>
std::vector<Bytes> valid_frames(unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<Bytes> frames;
    for (const auto& m : sample_messages<P>(rng, std::make_index_sequence<std::variant_size_v<typename P::AnyMessage>>{})) {
        std::array<std::uint8_t, 1024> buf{};
        if (auto n = P::encode(m, buf)) frames.emplace_back(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    }
    return frames;
}

std::vector<Bytes> mutations(const std::vector<Bytes>& valid, std::mt19937& rng) {
    std::vector<Bytes> out;
    for (const auto& v : valid) {
        out.push_back(v);
        for (std::size_t n = 0; n < v.size(); ++n) out.emplace_back(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n));
        for (std::size_t i = 0; i < v.size(); ++i)
            for (const std::uint8_t x : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
                Bytes m = v;
                m[i] ^= x;
                out.push_back(m);
            }
        Bytes t = v;
        t.push_back(static_cast<std::uint8_t>(rng()));
        out.push_back(t);
    }
    for (int k = 0; k < 200; ++k) {
        Bytes r(rng() % 40);
        for (auto& b : r) b = static_cast<std::uint8_t>(rng());
        out.push_back(r);
    }
    return out;
}

/// False when `in` holds an escape pair for a byte that needs no escaping: restoring a failed frame writes that byte
/// plainly, so the buffer then differs from the input.
template <class P>
bool canonical_escapes(const Bytes& in) {
    if constexpr (requires { P::framing_config.escape; }) {
        const auto& c = P::framing_config.escape;
        if (!c.enabled) return true;
        for (std::size_t i = 0; i + 1 < in.size(); ++i) {
            if (in[i] != c.esc) continue;
            const auto d = c.decode(in[i + 1]);
            if (d && !c.needs_escape(*d)) return false;
            ++i;
        }
    }
    return true;
}

/// Whole-buffer parse / decode / process / encode.
template <class T>
void check_whole_buffer(const std::vector<Bytes>& valid, unsigned seed) {
    using P = typename T::Cpp;
    std::mt19937 rng(seed);
    int ok = 0;
    for (const auto& in : mutations(valid, rng)) {
        INFO("input size " << in.size());
        // parse (escaped protocols unescape in place: every call gets its own copy)
        Bytes a = in, b = in;
        const auto cpp = P::parse(gmp::mut_bytes(a.data(), a.size()));
        typename T::AnyView v;
        const auto ce = T::parse(b.data(), b.size(), &v);
        REQUIRE(static_cast<int>(ce) == (cpp ? 0 : code(cpp.error())));
        CHECK(a == b);  // C and C++ leave the same bytes
        if (!cpp && canonical_escapes<P>(in)) CHECK(a == in);  // a frame that fails is left as received
        if (cpp) {
            ++ok;
            REQUIRE(static_cast<std::size_t>(v.kind) == cpp->index() + 1);
            const Head h = head_of(&v.as);
            std::visit([&](const auto& x) {
                CHECK(h.data - b.data() == view_ptr(x) - a.data());
                CHECK(h.size == view_bytes(x).size());
            }, *cpp);
            print_compare::same(*cpp, &T::print_view, v);
        }
        // process: same outcome as parse, one on_frame call on success
        {
            Bytes c = in;
            std::vector<Event> events;
            const auto cb = recording_callbacks<T>(events);
            CHECK(T::process(c.data(), c.size(), &cb) == ce);
            CHECK(events.size() == (ce == GMSG_OK ? 1u : 0u));
            CHECK(c == b);
        }
        if constexpr (T::structs) {
        // decode and re-encode
        Bytes a2 = in, b2 = in;
        const auto cpp_m = P::decode(gmp::mut_bytes(a2.data(), a2.size()));
        typename T::AnyMessage m;
        const auto de = T::decode(b2.data(), b2.size(), &m);
        REQUIRE(static_cast<int>(de) == (cpp_m ? 0 : code(cpp_m.error())));
        if (cpp_m) {
            REQUIRE(static_cast<std::size_t>(m.kind) == cpp_m->index() + 1);
            print_compare::same(*cpp_m, &T::print_message, m);
            std::array<std::uint8_t, 1024> x{}, y{};
            const auto n = P::encode(*cpp_m, x);
            std::size_t cn = 0;
            const auto ee = T::encode(&m, y.data(), y.size(), &cn);
            REQUIRE(static_cast<int>(ee) == (n ? 0 : code(n.error())));
            if (n) {
                REQUIRE(cn == *n);
                CHECK(std::memcmp(x.data(), y.data(), cn) == 0);
                const auto small = P::encode(*cpp_m, gmp::mut_bytes(x.data(), *n - 1));
                CHECK(static_cast<int>(T::encode(&m, y.data(), cn - 1, nullptr)) == (small ? 0 : code(small.error())));
            }
        }
        }
    }
    CHECK(ok > 0);
}

/// A noisy stream: valid frames mixed with garbage, corrupted frames and truncated frames.
Bytes noisy_stream(const std::vector<Bytes>& valid, std::mt19937& rng) {
    Bytes s;
    for (int k = 0; k < 40; ++k) {
        const auto& f = valid[rng() % valid.size()];
        const auto r = rng() % 10;
        if (r < 6) {
            s.insert(s.end(), f.begin(), f.end());
        } else if (r < 7) {
            for (auto n = rng() % 6; n > 0; --n) s.push_back(static_cast<std::uint8_t>(rng()));
        } else if (r < 9) {
            Bytes m = f;
            m[rng() % m.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
            s.insert(s.end(), m.begin(), m.end());
        } else {
            s.insert(s.end(), f.begin(), f.begin() + static_cast<std::ptrdiff_t>(rng() % f.size()));
        }
    }
    return s;
}

/// Frames delivered with data after their message, summed over check_stream runs.
std::size_t g_extra_frames = 0;

template <class T>
void check_stream(const std::vector<Bytes>& valid, unsigned seed) {
    using P = typename T::Cpp;
    std::mt19937 rng(seed);
    std::size_t frames = 0, errors = 0;
    for (int round = 0; round < 20; ++round) {
        const Bytes data = noisy_stream(valid, rng);
        for (const std::size_t chunk : {std::size_t{1}, std::size_t{5}, std::size_t{64}, data.size()}) {
            INFO("round " << round << ", chunk " << chunk);
            std::vector<Event> expected, actual;
            typename P::template Stream<> cpp;
            auto handler = cpp_recorder<P>(expected);
            for (std::size_t pos = 0; pos < data.size(); pos += chunk)
                (void)cpp.feed(gmp::bytes_view(data.data() + pos, std::min(chunk, data.size() - pos)), handler);

            static std::array<std::uint8_t, 4096> buf;
            REQUIRE(P::max_frame <= buf.size());
            typename T::Stream s;
            REQUIRE(T::stream_init(&s, buf.data(), P::max_frame) == GMSG_OK);
            s.cb = recording_callbacks<T>(actual);
            for (std::size_t pos = 0; pos < data.size(); pos += chunk)
                (void)T::feed(&s, data.data() + pos, std::min(chunk, data.size() - pos));

            REQUIRE(actual.size() == expected.size());
            for (std::size_t i = 0; i < actual.size(); ++i) {
                INFO("event " << i);
                CHECK(actual[i] == expected[i]);
                ++(actual[i].type == 0 ? frames : errors);
            }
            const auto* st = T::stats(&s);
            CHECK(st->frames == cpp.stats().frames);
            CHECK(st->frame_errors == cpp.stats().frame_errors);
            CHECK(st->bytes_skipped == cpp.stats().bytes_skipped);
            CHECK(st->overflows == cpp.stats().overflows);
            CHECK(st->frames_with_extra == cpp.stats().frames_with_extra);
            g_extra_frames += cpp.stats().frames_with_extra;
        }
    }
    // the streams must exercise both delivered frames and resync after errors
    INFO("frames " << frames << ", errors " << errors);
    CHECK(frames > 100);
    CHECK(errors > 10);
}

/// Event of a polled frame: message kind and bytes (and header / frame bytes for poll_frame).
template <class T>
Event c_polled(const typename T::AnyView& v) {
    Event ev;
    ev.kind = static_cast<int>(v.kind) - 1;
    const Head m = head_of(&v.as);
    ev.message = bytes_of(m.data, m.size);
    return ev;
}

template <class P, class V>
Event cpp_polled(const V& any) {
    Event ev;
    ev.kind = static_cast<int>(any.index());
    std::visit([&](const auto& x) { ev.message = view_bytes(x); }, any);
    return ev;
}

enum class PollMode { poll, frame, message, event, poll_for };

template <class T, class = void>
struct any_message_of { using type = int; };
template <class T>
struct any_message_of<T, std::void_t<typename T::AnyMessage>> { using type = typename T::AnyMessage; };

/// Push `data` in chunks and drain with the given poll function after each push, in both languages. Errors
/// reported to the callbacks are events of type 1; messages handed to the callbacks during poll_for type 2.
template <class T>
void check_poll(const Bytes& data, std::size_t chunk, PollMode mode) {
    using P = typename T::Cpp;
    using AnyMessageC = typename any_message_of<T>::type;
    std::vector<Event> expected, actual;

    auto cpp = P::template make_stream<P::max_frame>(cpp_recorder<P, 2>(expected));
    auto cpp_drain = [&] {
        for (;;) {
            if (mode == PollMode::poll) {
                auto v = cpp.poll();
                if (!v) return;
                expected.push_back(cpp_polled<P>(*v));
            } else if (mode == PollMode::frame) {
                auto r = cpp.poll_frame();
                if (!r) return;
                Event ev = cpp_polled<P>(r->message);
                if constexpr (has_header<P>) ev.header = bytes_of(r->header.data().data(), r->header.data().size());
                ev.header.push_back(static_cast<std::uint8_t>(r->bytes.size()));  // frame size
                ev.header.push_back(static_cast<std::uint8_t>(r->extra));         // data after the message
                expected.push_back(std::move(ev));
            } else if (mode == PollMode::message) {
                if constexpr (T::structs) {
                    auto m = cpp.poll_message();
                    if (!m) return;
                    std::array<std::uint8_t, 1024> buf{};
                    const auto n = P::encode(*m, buf);
                    REQUIRE(n);
                    expected.push_back(Event{0, static_cast<int>(m->index()), {}, Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n))});
                } else {
                    return;
                }
            } else if (mode == PollMode::event) {
                auto e = cpp.poll_event();
                if (!e) return;
                if (const auto* fe = std::get_if<gmp::FrameError>(&*e))
                    expected.push_back(Event{1, code(fe->error), {}, bytes_of(fe->data.data(), fe->data.size())});
                else
                    expected.push_back(cpp_polled<P>(std::get<0>(*e)));
            } else {
                using V0 = std::variant_alternative_t<0, typename P::AnyView>;
                auto v = cpp.template poll_for<V0>();
                if (!v) return;
                expected.push_back(cpp_polled<P>(typename P::AnyView(*v)));
            }
        }
    };
    for (std::size_t pos = 0; pos < data.size();) {
        pos += cpp.push(gmp::bytes_view(data.data() + pos, std::min(chunk, data.size() - pos)));
        cpp_drain();
    }

    static std::array<std::uint8_t, 4096> buf;
    typename T::Stream s;
    REQUIRE(T::stream_init(&s, buf.data(), P::max_frame) == GMSG_OK);
    s.cb = recording_callbacks<T, 2>(actual);
    auto c_drain = [&] {
        for (;;) {
            typename T::AnyView v;
            if (mode == PollMode::poll) {
                if (!T::poll(&s, &v)) return;
                actual.push_back(c_polled<T>(v));
            } else if (mode == PollMode::frame) {
                typename T::Received r;
                if (!T::poll_frame(&s, &r)) return;
                Event ev = c_polled<T>(r.message);
                if constexpr (has_header<P>) {
                    const Head h = head_of(&r.header);
                    ev.header = bytes_of(h.data, h.size);
                }
                ev.header.push_back(static_cast<std::uint8_t>(r.size));
                ev.header.push_back(static_cast<std::uint8_t>(r.extra));
                actual.push_back(std::move(ev));
            } else if (mode == PollMode::message) {
                if constexpr (T::structs) {
                    AnyMessageC m;
                    if (!T::poll_message(&s, &m)) return;
                    std::array<std::uint8_t, 1024> out{};
                    std::size_t n = 0;
                    REQUIRE(T::encode(&m, out.data(), out.size(), &n) == GMSG_OK);
                    actual.push_back(Event{0, static_cast<int>(m.kind) - 1, {}, Bytes(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n))});
                } else {
                    return;
                }
            } else if (mode == PollMode::event) {
                typename T::PollEvent e;
                const gmsg_poll r = T::poll_event(&s, &e);
                if (r == GMSG_POLL_NEED_MORE) return;
                if (r == GMSG_POLL_ERROR) actual.push_back(Event{1, static_cast<int>(e.error), {}, bytes_of(e.data, e.size)});
                else actual.push_back(c_polled<T>(e.message));
            } else {
                if (!T::poll_for(&s, 1, &v)) return;
                actual.push_back(c_polled<T>(v));
            }
        }
    };
    for (std::size_t pos = 0; pos < data.size();) {
        pos += T::push(&s, data.data() + pos, std::min(chunk, data.size() - pos));
        c_drain();
    }

    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        INFO("event " << i);
        CHECK(actual[i] == expected[i]);
    }
    CHECK(T::stats(&s)->frames == cpp.stats().frames);
    CHECK(T::stats(&s)->frame_errors == cpp.stats().frame_errors);
    CHECK(T::stats(&s)->bytes_skipped == cpp.stats().bytes_skipped);
    CHECK(T::stats(&s)->overflows == cpp.stats().overflows);
}

template <class T>
void check_polls(const std::vector<Bytes>& valid, unsigned seed) {
    std::mt19937 rng(seed);
    for (int round = 0; round < 6; ++round) {
        const Bytes data = noisy_stream(valid, rng);
        for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, data.size()})
            for (const auto mode : {PollMode::poll, PollMode::frame, PollMode::message, PollMode::event, PollMode::poll_for}) {
                INFO("round " << round << ", chunk " << chunk << ", mode " << static_cast<int>(mode));
                check_poll<T>(data, chunk, mode);
            }
    }
}

/// Protocols without encoders: frames are given.
/// Noisy streams fed in chunks, with resync / flush / idle ticks between the chunks (the same sequence for both
/// languages; the clock wraps around): events, return values and statistics must match.
template <class T>
void check_recovery(const std::vector<Bytes>& valid, unsigned seed) {
    using P = typename T::Cpp;
    std::mt19937 rng(seed);
    std::size_t resyncs = 0, flushes = 0;
    for (int round = 0; round < 20; ++round) {
        const Bytes data = noisy_stream(valid, rng);
        const std::size_t chunk = 1 + rng() % 9;
        const unsigned op_seed = rng();
        INFO("round " << round << ", chunk " << chunk);
        std::vector<Event> expected, actual;
        typename P::template Stream<> cpp;
        auto handler = cpp_recorder<P>(expected);
        static std::array<std::uint8_t, 4096> buf;
        typename T::Stream s;
        REQUIRE(T::stream_init(&s, buf.data(), P::max_frame) == GMSG_OK);
        s.cb = recording_callbacks<T>(actual);
        cpp.set_idle_timeout(3);
        T::set_idle_timeout(&s, 3);
        std::mt19937 ops(op_seed);
        std::uint32_t now = 0xFFFFFFF0u;  // wraps during the round
        auto step = [&] {
            const auto op = ops() % 10;
            now += 1 + ops() % 2;
            bool a = false, b = false;
            if (op == 0) { a = cpp.resync(handler); b = T::resync(&s); }
            else if (op == 1 && ops() % 4 == 0) { a = cpp.flush(handler); b = T::flush(&s); }
            else { a = cpp.tick(now, handler); b = T::tick(&s, now); }
            CHECK(a == b);
            while (cpp.next(handler)) {}
            while (T::next(&s)) {}
        };
        for (std::size_t pos = 0; pos < data.size(); pos += chunk) {
            const std::size_t n = std::min(chunk, data.size() - pos);
            (void)cpp.feed(gmp::bytes_view(data.data() + pos, n), handler);
            (void)T::feed(&s, data.data() + pos, n);
            step();
        }
        for (int k = 0; k < 12; ++k) step();  // the link goes quiet: idle ticks give up the frames left over
        REQUIRE(actual.size() == expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            INFO("event " << i);
            CHECK(actual[i] == expected[i]);
        }
        const auto* st = T::stats(&s);
        CHECK(st->frames == cpp.stats().frames);
        CHECK(st->frame_errors == cpp.stats().frame_errors);
        CHECK(st->bytes_skipped == cpp.stats().bytes_skipped);
        CHECK(st->resyncs == cpp.stats().resyncs);
        CHECK(st->flushes == cpp.stats().flushes);
        resyncs += cpp.stats().resyncs;
        flushes += cpp.stats().flushes;
    }
    CHECK(resyncs > 0);
    CHECK(flushes > 0);
}

template <class T>
void check_frames(const std::vector<Bytes>& valid, unsigned seed) {
    for (const auto& f : valid) {
        Bytes a = f, b = f;
        typename T::AnyView v;
        CHECK(T::Cpp::parse(gmp::mut_bytes(a.data(), a.size())));
        CHECK(T::parse(b.data(), b.size(), &v) == GMSG_OK);
    }
    check_whole_buffer<T>(valid, seed + 1);
    if constexpr (T::stream) {
        check_stream<T>(valid, seed + 2);
        check_polls<T>(valid, seed + 3);
        check_recovery<T>(valid, seed + 4);
    }
}

template <class T>
void check_protocol(unsigned seed) {
    const auto valid = valid_frames<typename T::Cpp>(seed);
    // at least one encodable frame per message type (value-initialised or found by decoding random bodies)
    REQUIRE(valid.size() >= std::variant_size_v<typename T::Cpp::AnyMessage>);
    // independent of the comparison: every encoded frame parses back in both languages
    for (const auto& f : valid) {
        Bytes a = f, b = f;
        typename T::AnyView v;
        CHECK(T::Cpp::parse(gmp::mut_bytes(a.data(), a.size())));
        CHECK(T::parse(b.data(), b.size(), &v) == GMSG_OK);
    }
    check_whole_buffer<T>(valid, seed + 1);
    if constexpr (T::stream) {
        check_stream<T>(valid, seed + 2);
        check_polls<T>(valid, seed + 3);
        check_recovery<T>(valid, seed + 4);
    }
}

} // namespace

TEST_CASE("generated C protocols match C++: length prefixed (telemetry, motor_link)") {
    check_protocol<BinaryLinkC>(10);
    check_protocol<MotorLinkC>(11);
}

TEST_CASE("generated C protocols match C++: CAN (no framing) and fixed frames") {
    check_protocol<CanC>(20);
    check_protocol<CanFixedC>(21);
}

TEST_CASE("generated C protocols match C++: SHT4x (fixed, no header)") {
    check_protocol<Sht4xMeasurementC>(30);
    check_protocol<Sht4xSerialC>(31);
    check_protocol<Sht4xEnumMeasurementC>(32);
}

TEST_CASE("generated C protocols match C++: byte stuffing (all styles, wire checksums and lengths)") {
    check_protocol<SyncLinkC>(40);
    check_protocol<WireChecksumC>(41);
    check_protocol<WireLengthC>(42);
    check_protocol<CrcLengthC>(43);
    check_protocol<WireCrcLengthC>(44);
    check_protocol<WireCrcOverWireC>(45);
    check_protocol<StuffedFixedC>(46);
    check_protocol<DelimitedC>(47);
}

TEST_CASE("generated C protocols match C++: length adjust, coverage modes, little-endian fields, end after escaping") {
    check_protocol<AdjustedC>(50);
    check_protocol<WholeFrameC>(51);
    check_protocol<EscapedWithEndC>(52);
    check_protocol<FixedWithEndC>(53);
}

TEST_CASE("generated C protocols match C++: data after the message (extra, exact_size)") {
    g_extra_frames = 0;
    check_protocol<LooseC>(54);
    CHECK(g_extra_frames > 0);  // corrupted lengths produced frames with extra data
    g_extra_frames = 0;
    check_protocol<ExactC>(55);
    CHECK(g_extra_frames == 0);  // exact_size rejects them
}

TEST_CASE("generated C protocols match C++: custom checksum, custom type in a message (hdlc)") {
    check_protocol<HdlcC>(60);
    check_protocol<SummedC>(61);
}

TEST_CASE("generated C protocols match C++: text protocols (NMEA checksum, token type id, custom text type)") {
    check_protocol<NmeaC>(80);
    check_protocol<TextPosC>(81);
}

TEST_CASE("generated C protocols match C++: custom framer, view-only messages") {
    // frames: [n][n bytes] = header n (the length byte), then Blob { kind, data... }
    const std::vector<Bytes> valid = {{0x01, 0x07}, {0x03, 0x02, 0xAA, 0xBB}, {0x05, 0x09, 0x00, 0x01, 0x02, 0x03}};
    check_frames<CustomFramerC>(valid, 70);
}

// ---- API details ----------------------------------------------------------------------------------------------
namespace {

struct Seen {
    int status = 0, command = 0, frames = 0;
    std::uint8_t msg_id = 0;
    std::uint16_t len = 0, voltage = 0;
};

void on_status(void* ctx, const acme_telemetry_binary_link_header_view* h, const acme_telemetry_status_view* v) {
    auto* s = static_cast<Seen*>(ctx);
    ++s->status;
    s->msg_id = acme_telemetry_binary_link_header_view_msg_id(h);
    s->len = acme_telemetry_binary_link_header_view_len(h);
    s->voltage = acme_telemetry_status_view_voltage(v);
}
void on_command(void* ctx, const acme_telemetry_binary_link_header_view*, const acme_telemetry_command_view*) {
    ++static_cast<Seen*>(ctx)->command;
}
void on_any(void* ctx, const acme_telemetry_binary_link_header_view*, const acme_telemetry_binary_link_any_view*) {
    ++static_cast<Seen*>(ctx)->frames;
}

} // namespace

TEST_CASE("generated C protocol: per-message callbacks, header, encode_with_header, stream buffer size") {
    acme_telemetry_status st;
    std::memset(&st, 0, sizeof st);
    st.voltage = 3300;
    std::memcpy(st.label, "ab", 2);
    st.label_len = 2;
    std::array<std::uint8_t, 256> frame{};
    std::size_t n = 0;
    REQUIRE(acme_telemetry_binary_link_encode_status(&st, frame.data(), frame.size(), &n) == GMSG_OK);

    // the same bytes as C++
    acme::telemetry::Status cs;
    cs.voltage = 3300;
    cs.label = "ab";
    std::array<std::uint8_t, 256> cpp{};
    const auto cn = acme::telemetry::BinaryLink::encode(cs, cpp);
    REQUIRE(cn);
    REQUIRE(*cn == n);
    CHECK(std::memcmp(frame.data(), cpp.data(), n) == 0);

    Seen seen;
    acme_telemetry_binary_link_callbacks cb;
    std::memset(&cb, 0, sizeof cb);
    cb.ctx = &seen;
    cb.on_status = on_status;
    cb.on_command = on_command;
    cb.on_frame = on_any;
    REQUIRE(acme_telemetry_binary_link_process(frame.data(), n, &cb) == GMSG_OK);
    CHECK(seen.status == 1);
    CHECK(seen.command == 0);
    CHECK(seen.frames == 1);
    CHECK(seen.msg_id == ACME_TELEMETRY_MSG_ID_STATUS);  // filled in by encode
    CHECK(seen.len == n - 7);                           // body length: frame minus sync, header and CRC
    CHECK(seen.voltage == 3300);

    // encode_with_header: the id and length are still filled in
    acme_telemetry_binary_link_header h;
    h.msg_id = 99;
    h.len = 1;
    std::array<std::uint8_t, 256> frame2{};
    std::size_t n2 = 0;
    REQUIRE(acme_telemetry_binary_link_encode_status_with_header(&h, &st, frame2.data(), frame2.size(), &n2) == GMSG_OK);
    CHECK(n2 == n);
    CHECK(std::memcmp(frame.data(), frame2.data(), n) == 0);

    // a stream buffer smaller than the protocol's max frame is rejected
    static std::uint8_t small[ACME_TELEMETRY_BINARY_LINK_MAX_FRAME - 1];
    acme_telemetry_binary_link_stream s;
    CHECK(acme_telemetry_binary_link_stream_init(&s, small, sizeof small) == GMSG_ERR_BUFFER_FULL);
    static std::uint8_t big[ACME_TELEMETRY_BINARY_LINK_MAX_FRAME];
    REQUIRE(acme_telemetry_binary_link_stream_init(&s, big, sizeof big) == GMSG_OK);
    s.cb = cb;
    // garbage, a frame byte by byte, more garbage: one frame delivered
    const std::uint8_t junk[] = {0x00, 0xAA, 0x13};
    (void)acme_telemetry_binary_link_stream_feed(&s, junk, sizeof junk);
    std::size_t frames = 0;
    for (std::size_t i = 0; i < n; ++i) frames += acme_telemetry_binary_link_stream_feed(&s, frame.data() + i, 1);
    CHECK(frames == 1);
    CHECK(seen.status == 2);
    CHECK(acme_telemetry_binary_link_stream_stats(&s)->frames == 1);
    CHECK(acme_telemetry_binary_link_stream_last_error(&s) != GMSG_OK);  // the garbage was reported
    acme_telemetry_binary_link_stream_reset(&s);
    CHECK(acme_telemetry_binary_link_stream_stats(&s)->frames == 0);
    CHECK(ACME_TELEMETRY_BINARY_LINK_MAX_FRAME == acme::telemetry::BinaryLink::max_frame);
}
