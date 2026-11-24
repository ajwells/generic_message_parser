// js_crossref: reference results of the generated C++ for the generated-JavaScript cross-check
// (tests/roundtrip_js/test_generated.js). For every message (protocol headers included) and every protocol of the
// test schemas it records what C++ does with valid encodings, mutated and truncated copies and random bytes:
//   messages   struct decode (error, or gmsg::print text and the re-encoded bytes) and view from() (error or text)
//   protocols  parse / decode / re-encode of single frames, and the events and statistics of noisy streams
// Valid encodings come from default-constructed structs (always encodable) and hand-picked sample values.

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "can.gmp.hpp"
#include "computed.gmp.hpp"
#include "custom.gmp.hpp"
#include "enums.gmp.hpp"
#include "limits.gmp.hpp"
#include "fieldcrc.gmp.hpp"
#include "framing.gmp.hpp"
#include "hdlc.gmp.hpp"
#include "motor_link.gmp.hpp"
#include "nmea.gmp.hpp"
#include "sht4x.gmp.hpp"
#include "sht4x_enum.gmp.hpp"
#include "stuffed.gmp.hpp"
#include "telemetry.gmp.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
std::mt19937 rng(20261122);
std::string out;
bool first_message = true, first_protocol = true;
std::string messages_json, protocols_json;

std::string js(std::string_view s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20 || c >= 0x7f) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else o += static_cast<char>(c);
    }
    return o + "\"";
}
std::string hex(const Bytes& b) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (auto x : b) { s += h[x >> 4]; s += h[x & 15]; }
    return "\"" + s + "\"";
}
template <class T>
std::string text(const T& v, bool single = false) {
    std::string s;
    gmsg::PrintOptions o;
    if (single) o.style = gmsg::PrintStyle::single_line;
    (void)gmsg::print(v, s, o);
    return s;
}
std::string err(gmsg::Error e) { return "{\"err\":" + std::to_string(static_cast<int>(e)) + "}"; }

template <class M>
Bytes encode(const M& m) {
    std::array<std::uint8_t, 4096> buf{};
    auto n = m.encode(buf);
    if (!n) return {};
    return Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
}

/// Seeds plus truncated, mutated and extended copies, and random byte strings.
std::vector<Bytes> variants(const std::vector<Bytes>& seeds) {
    std::vector<Bytes> v;
    for (const auto& s : seeds) {
        v.push_back(s);
        const std::size_t n = s.size();
        for (std::size_t k = 0; k < n && k < 24; ++k) v.emplace_back(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(rng() % (n ? n : 1)));
        for (int k = 0; k < 16 && n; ++k) {
            auto x = s;
            x[rng() % n] = static_cast<std::uint8_t>(rng());
            v.push_back(x);
        }
        for (int k = 0; k < 10 && n; ++k) {
            auto x = s;
            x[rng() % n] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
            v.push_back(x);
        }
        for (int k = 0; k < 3; ++k) {
            auto x = s;
            x.insert(x.begin() + static_cast<std::ptrdiff_t>(n ? rng() % n : 0), static_cast<std::uint8_t>(rng()));
            v.push_back(x);
        }
        auto x = s;
        for (int k = 0; k < 3; ++k) x.push_back(static_cast<std::uint8_t>(rng()));
        v.push_back(x);
        for (int k = 0; k < 3; ++k) {
            Bytes r(rng() % (n + 4));
            for (auto& b : r) b = static_cast<std::uint8_t>(rng() % 4 == 0 ? rng() % 4 : rng());
            v.push_back(r);
        }
    }
    return v;
}

/// One message: M is the struct (void for view-only messages), V the view.
template <class M, class V>
void message(const char* schema, const char* name, const std::vector<Bytes>& seeds) {
    std::string cases;
    for (const auto& b : variants(seeds)) {
        const gmsg::bytes_view bv(b.data(), b.size());
        std::string c = "{\"hex\":" + hex(b);
        if constexpr (!std::is_void_v<M>) {
            auto d = M::decode(bv);
            if (!d) c += ",\"decode\":" + err(d.error());
            else {
                std::array<std::uint8_t, 4096> buf{};
                auto n = d->encode(buf);
                c += ",\"decode\":{\"multi\":" + js(text(*d)) + ",\"single\":" + js(text(*d, true));
                if (n) c += ",\"encode\":" + hex(Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)));
                else c += ",\"encodeErr\":" + std::to_string(static_cast<int>(n.error()));
                c += "}";
            }
        }
        auto v = V::from(bv);
        if (!v) c += ",\"view\":" + err(v.error());
        else {
            c += ",\"view\":{\"multi\":" + js(text(*v));
            if constexpr (requires { v->wire_size(); }) c += ",\"size\":" + std::to_string(v->wire_size());
            if constexpr (requires { v->extra_bytes(); }) c += ",\"extra\":" + std::to_string(v->extra_bytes());
            if constexpr (requires { v->extra_tokens(); }) c += ",\"extra\":" + std::to_string(v->extra_tokens());
            c += "}";
        }
        cases += (cases.empty() ? "" : ",\n") + c + "}";
    }
    messages_json += (first_message ? "" : ",\n") + std::string("{\"schema\":") + js(schema) + ",\"name\":" + js(name) +
                     ",\"cases\":[\n" + cases + "]}";
    first_message = false;
}

// ---- protocols ----------------------------------------------------------------------------------------------
template <class P>
constexpr bool has_header = requires { typename P::HeaderView; };
template <class P>
constexpr bool has_stream = requires { typename P::template Stream<>; };

template <class P>
struct ProtocolCase {
    const char* schema;
    const char* name;
    std::vector<Bytes> frames;  // valid frames

    template <class M>
    ProtocolCase& add(const M& m) {
        std::array<std::uint8_t, 4096> buf{};
        auto n = P::encode(m, gmsg::mut_bytes(buf.data(), buf.size()));
        if (n) frames.emplace_back(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
        else std::fprintf(stderr, "js_crossref: encode failed for %s\n", name);
        return *this;
    }

    void done() {
        std::string singles;
        for (const auto& f : variants(frames)) {
            auto b = f;  // parse may unescape in place
            std::string c = "{\"hex\":" + hex(f);
            auto v = P::parse(gmsg::mut_bytes(b.data(), b.size()));
            if (!v) c += ",\"parse\":" + err(v.error());
            else c += ",\"parse\":{\"single\":" + js(text(*v, true)) + "}";
            if constexpr (requires { typename P::AnyMessage; }) {
                auto b2 = f;
                auto m = P::decode(gmsg::mut_bytes(b2.data(), b2.size()));
                if (!m) c += ",\"decode\":" + err(m.error());
                else {
                    std::array<std::uint8_t, 4096> buf{};
                    auto n = P::encode(*m, gmsg::mut_bytes(buf.data(), buf.size()));
                    c += ",\"decode\":{\"single\":" + js(text(*m, true));
                    if (n) c += ",\"encode\":" + hex(Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)));
                    else c += ",\"encodeErr\":" + std::to_string(static_cast<int>(n.error()));
                    c += "}";
                }
            }
            singles += (singles.empty() ? "" : ",\n") + c + "}";
        }
        std::string streams;
        if constexpr (has_stream<P>) {
            for (int round = 0; round < 12; ++round) {
                Bytes wire;
                const int parts = 3 + static_cast<int>(rng() % 6);
                for (int k = 0; k < parts; ++k) {
                    const auto r = rng() % 6;
                    if (r < 3 && !frames.empty()) {
                        const auto& f = frames[rng() % frames.size()];
                        wire.insert(wire.end(), f.begin(), f.end());
                    } else if (r == 3 && !frames.empty()) {
                        auto f = frames[rng() % frames.size()];
                        if (!f.empty()) f[rng() % f.size()] ^= static_cast<std::uint8_t>(1 + rng() % 255);
                        wire.insert(wire.end(), f.begin(), f.end());
                    } else if (r == 4 && !frames.empty()) {
                        const auto& f = frames[rng() % frames.size()];
                        wire.insert(wire.end(), f.begin(), f.begin() + static_cast<std::ptrdiff_t>(rng() % (f.size() + 1)));
                    } else {
                        for (int j = 0, n = 1 + static_cast<int>(rng() % 5); j < n; ++j) wire.push_back(static_cast<std::uint8_t>(rng()));
                    }
                }
                // chunk 0 is the recovery run: chunks of 3 with resync (r) / flush (f) / tick (t) / nothing (.) after
                // each chunk, then 8 idle ticks; the clock advances by 1 per op and the idle timeout is 2
                for (const std::size_t chunk : {std::size_t{1}, std::size_t{5}, wire.size() ? wire.size() : 1, std::size_t{0}}) {
                    std::string ev, ops;
                    typename P::template Stream<> parser;  // the real C++ stream: resyncs over the bytes as received
                    auto on_view = [&](const auto& v) { ev += "M:" + text(v, true) + "\n"; };
                    auto handler = gmsg::overloaded{
                        [&](const gmsg::FrameError& e) {
                            ev += "E" + std::to_string(static_cast<int>(e.error)) + ":" + std::to_string(e.data.size()) + "\n";
                        },
                        [&](const auto&... a) {
                            if constexpr (sizeof...(a) == 2) {
                                const auto& [h, v] = std::tie(a...);
                                ev += "H:" + text(h, true) + "\n";
                                on_view(v);
                            } else {
                                (on_view(a), ...);
                            }
                        },
                    };
                    if (chunk) {
                        for (std::size_t pos = 0; pos < wire.size(); pos += chunk)
                            (void)parser.feed(gmsg::bytes_view(wire.data() + pos, std::min(chunk, wire.size() - pos)), handler);
                    } else {
                        parser.set_idle_timeout(2);
                        std::uint32_t now = 0;
                        auto op = [&](char c) {
                            ops += c;
                            ++now;
                            const bool did = c == 'r' ? parser.resync(handler) : c == 'f' ? parser.flush(handler)
                                           : c == 't' ? parser.tick(now, handler) : false;
                            ev += std::string("R") + (did ? "1" : "0") + "\n";
                            while (parser.next(handler)) {}
                        };
                        for (std::size_t pos = 0; pos < wire.size(); pos += 3) {
                            (void)parser.feed(gmsg::bytes_view(wire.data() + pos, std::min<std::size_t>(3, wire.size() - pos)), handler);
                            const auto k = rng() % 12;
                            op(k == 0 ? 'r' : k == 1 ? 'f' : k < 8 ? 't' : '.');
                        }
                        for (int k = 0; k < 8; ++k) op('t');
                    }
                    const auto& st = parser.stats();
                    streams += (streams.empty() ? "" : ",\n") + std::string("{\"hex\":") + hex(wire) +
                               ",\"chunk\":" + std::to_string(chunk) + ",\"ops\":" + js(ops) + ",\"events\":" + js(ev) +
                               ",\"stats\":[" + std::to_string(st.frames) + "," + std::to_string(st.frame_errors) + "," +
                               std::to_string(st.bytes_skipped) + "," + std::to_string(st.overflows) + "," +
                               std::to_string(st.resyncs) + "," + std::to_string(st.flushes) + "," +
                               std::to_string(st.frames_with_extra) + "]}";
                }
            }
        }
        protocols_json += (first_protocol ? "" : ",\n") + std::string("{\"schema\":") + js(schema) + ",\"name\":" + js(name) +
                          ",\"frames\":[\n" + singles + "],\"streams\":[\n" + streams + "]}";
        first_protocol = false;
    }
};

} // namespace

// view type of a struct M: the generated `MView`
#define GMP_MSG(schema, ns, M, ...)                                                                    \
    message<ns::M, ns::M##View>(schema, #M, [&] {                                                     \
        std::vector<Bytes> seeds{encode(ns::M{})};                                                     \
        for (const auto& m : std::initializer_list<ns::M>{__VA_ARGS__}) seeds.push_back(encode(m));   \
        return seeds;                                                                                  \
    }())

int main() {
    // ---- telemetry ---------------------------------------------------------------------------------
    namespace t = acme::telemetry;
    auto status = [](t::Mode mode) {
        t::Status s;
        s.voltage = 1234;
        s.flags.armed = true;
        s.flags.mode = mode;
        s.flags.level = -3;
        s.samples = {10, -20, 30};
        s.count = 3;
        if (mode == t::Mode::fault) s.ext = t::Extended{-125, 101325};
        s.label = "ab\"c";
        return s;
    };
    t::Command cmd;
    cmd.opcode = 0xBEEF;
    cmd.args = {0x00, 0x7F, 0xAB, 0x10};
    cmd.arg_len = 4;
    t::Log lg;
    lg.level = 200;
    lg.text = "tab\there\x01";
    lg.tail = {9, 8, 7};
    GMP_MSG("telemetry", t, Extended, t::Extended{-125, 101325});
    GMP_MSG("telemetry", t, Status, status(t::Mode::run), status(t::Mode::fault), status(static_cast<t::Mode>(6)));
    GMP_MSG("telemetry", t, Command, cmd);
    GMP_MSG("telemetry", t, Log, lg);
    GMP_MSG("telemetry", t, BinaryLinkHeader);
    ProtocolCase<t::BinaryLink>{"telemetry", "BinaryLink", {}}.add(status(t::Mode::run)).add(status(t::Mode::fault)).add(cmd).add(lg).add(t::Log{}).done();

    // ---- can ---------------------------------------------------------------------------------------
    namespace v = vehicle::can;
    v::EngineData e;
    e.signals.rpm = 1601;
    e.signals.gear = v::Gear::drive;
    e.signals.coolant = 130;
    e.signals.throttle = 457;
    e.signals.counter = 9;
    e.signals.flags = 0x1234;
    v::Sensor sn;
    sn.temp = -1234;
    sn.humidity = 55;
    GMP_MSG("can", v, EngineData, e);
    GMP_MSG("can", v, Sensor, sn);
    GMP_MSG("can", v, FixedLinkHeader);
    ProtocolCase<v::FixedLink>{"can", "FixedLink", {}}.add(sn).add(v::Sensor{}).done();

    // ---- computed ----------------------------------------------------------------------------------
    test::computed::Packed pk;
    pk.b0 = 0x12;
    pk.b1 = 0x34;
    pk.b2 = 0x56;
    pk.wide = 0xDEADBEEF;
    pk.lo = 1001;
    pk.flags.ready = true;
    pk.flags.level = 77;
    pk.unit = test::computed::Unit::cm;
    pk.items = {1, 2, 3};
    pk.n = 3;
    GMP_MSG("computed", test::computed, Packed, pk);

    // ---- field CRCs ----------------------------------------------------------------------------------
    namespace f = test::fieldcrc;
    f::Mixed mx;
    mx.state = f::State::on;
    mx.items = {1, 0xFFFF};
    mx.count = 2;
    mx.level = -7;
    f::Mixed mx2 = mx;
    mx2.state = f::State::error;
    mx2.extra = 0x0102;
    f::Reading rd;
    rd.raw_ = -1234;
    GMP_MSG("fieldcrc", f, Mixed, mx, mx2);
    GMP_MSG("fieldcrc", f, Reading, rd);

    // ---- enums ---------------------------------------------------------------------------------------
    namespace en = test::enums;
    en::Sample sa;
    sa.color = en::Color::green;
    sa.open = static_cast<en::Open>(9);
    sa.flags.mode = en::Mode::run;
    sa.levels = {en::Level::low, en::Level::high};
    sa.inner.color = en::Color::blue;
    sa.more = {en::Color::red, en::Color::blue};
    sa.n = 2;
    en::Tail tl;
    tl.rest = {en::Color::red, en::Color::green, en::Color::blue};
    GMP_MSG("enums", en, Inner);
    GMP_MSG("enums", en, Sample, sa);
    GMP_MSG("enums", en, Tail, tl);
    GMP_MSG("enums", en, Line);

    // ---- value limits ----------------------------------------------------------------------------------
    namespace li = test::limits;
    auto limits_sample = [](bool edge) {
        li::Sample s;
        s.humidity = edge ? 3146 : 30000;
        s.temp = edge ? -304 : 1250;
        s.big = edge ? 0x8000000000000000ull : 0xFFFFFFFFFFFFFFFEull;
        s.neg = edge ? -1 : INT64_MIN;
        s.ratio = edge ? 1.5f : -1.5f;
        s.wide = edge ? 1e300 : -1e300;
        s.flags.level = edge ? 5 : 0;
        s.flags.trim = edge ? -10 : 15;
        s.flags.gain = edge ? 200 : 0;
        s.levels = {static_cast<std::uint16_t>(edge ? 10 : 1000), static_cast<std::uint16_t>(edge ? 1000 : 10)};
        s.inner.pct = edge ? 100 : 0;
        for (int k = 0; k < (edge ? 3 : 1); ++k) (void)s.more.push_back(static_cast<std::int8_t>(edge ? -50 : 127));
        s.n = static_cast<std::uint8_t>(s.more.size());
        if (edge) s.opt = 2;
        return s;
    };
    li::Tail lt;
    lt.rest = {500, 0, 499};
    li::Line ll;
    ll.speed = 99.5;
    ll.count = 999;
    ll.opt = -5;
    ll.level = 500;
    GMP_MSG("limits", li, Inner);
    {
        // every byte of both samples changed (one at a time): reaches each limit, including bit fields
        std::vector<Bytes> seeds{encode(limits_sample(true)), encode(limits_sample(false))};
        for (const auto& base : std::vector<Bytes>(seeds))
            for (std::size_t i = 0; i < base.size(); ++i)
                for (const std::uint8_t x : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
                    Bytes b = base;
                    b[i] ^= x;
                    seeds.push_back(b);
                }
        message<li::Sample, li::SampleView>("limits", "Sample", seeds);
    }
    GMP_MSG("limits", li, Tail, lt);
    {
        std::vector<Bytes> seeds{encode(ll)};
        for (const char* t : {"99.6,1,,1", "-0.5,1,,1", "nan,1,,1", "inf,1,,1", "1,1000,,1", "1,1,-6,1", "1,1,,501", "0,0,,0"})
            seeds.emplace_back(t, t + std::char_traits<char>::length(t));
        message<li::Line, li::LineView>("limits", "Line", seeds);
    }

    // ---- framing ---------------------------------------------------------------------------------------
    namespace fr = test::framing;
    fr::Reading r1;
    r1.channel = 3;
    r1.value = -123456;
    fr::Reading r2;
    r2.channel = 0x7E;
    r2.value = 0x0D7D7EC0;
    fr::Note nt;
    nt.text = "hi \x7E\x0D";
    nt.n = 5;
    GMP_MSG("framing", fr, Reading, r1, r2);
    GMP_MSG("framing", fr, Note, nt);
    GMP_MSG("framing", fr, AdjustedHeader);
    GMP_MSG("framing", fr, EscapedWithEndHeader);
    ProtocolCase<fr::Adjusted>{"framing", "Adjusted", {}}.add(r1).add(nt).add(r2).done();
    ProtocolCase<fr::WholeFrame>{"framing", "WholeFrame", {}}.add(r1).add(nt).add(r2).done();
    ProtocolCase<fr::EscapedWithEnd>{"framing", "EscapedWithEnd", {}}.add(r1).add(nt).add(r2).done();
    ProtocolCase<fr::FixedWithEnd>{"framing", "FixedWithEnd", {}}.add(r1).add(r2).done();
    ProtocolCase<fr::Loose>{"framing", "Loose", {}}.add(r1).add(nt).add(r2).done();
    ProtocolCase<fr::Exact>{"framing", "Exact", {}}.add(r1).add(nt).add(r2).done();

    // ---- hdlc (custom type, custom checksum) ----------------------------------------------------------------
    namespace hd = link::hdlc;
    hd::Reading hr;
    hr.channel = 2;
    hr.name = "temp";
    hr.value = 21.5f;
    hd::Batch bt;
    bt.when = test::Time{12, 34, 56};
    bt.readings = {hr, hr, hr};
    bt.n = 3;
    bt.crc_in_payload = 0xA1B2C3D4;
    hd::Ping pg;
    pg.seq = 0x7E7D7E;
    pg.payload = {0x7E, 0x7D, 0xC0, 0xDB};
    GMP_MSG("hdlc", hd, Reading, hr);
    GMP_MSG("hdlc", hd, Batch, bt);
    GMP_MSG("hdlc", hd, Ping, pg);
    ProtocolCase<hd::Hdlc>{"hdlc", "Hdlc", {}}.add(bt).add(pg).add(hd::Ping{}).done();
    ProtocolCase<hd::Summed>{"hdlc", "Summed", {}}.add(pg).add(hd::Ping{}).done();

    // ---- custom (text custom type, custom framer, view-only message) ---------------------------------------------
    ext::Pos ps;
    ps.lat = 48.1173;
    ps.lon = 11.5166;
    GMP_MSG("custom", ext, Pos, ps);
    message<void, ext::BlobView>("custom", "Blob", {Bytes{1, 2, 3}, Bytes{0}, Bytes{}});
    ProtocolCase<ext::TextPos>{"custom", "TextPos", {}}.add(ps).add(ext::Pos{}).done();
    {
        // Custom carries only a view-only message: frames are built by hand ([n][header n][blob...])
        ProtocolCase<ext::Custom> pc{"custom", "Custom", {}};
        pc.frames = {Bytes{3, 7, 1, 2}, Bytes{1, 9}, Bytes{5, 1, 0xAA, 0xBB, 0xCC, 0xDD}};
        pc.done();
    }

    // ---- motor_link ------------------------------------------------------------------------------------
    GMP_MSG("motor_link", motor, SetSpeed);
    GMP_MSG("motor_link", motor, Telemetry);
    GMP_MSG("motor_link", motor, Log);
    GMP_MSG("motor_link", motor, MotorLinkHeader);
    ProtocolCase<motor::MotorLink>{"motor_link", "MotorLink", {}}.add(motor::SetSpeed{}).add(motor::Telemetry{}).add(motor::Log{}).done();

    // ---- NMEA ------------------------------------------------------------------------------------------
    namespace nm = nav::nmea;
    nm::GGA g;
    g.utc_time = 123519.0;
    g.latitude = 4807.038;
    g.lat_hemi = 'N';
    g.longitude = 1131.0;
    g.lon_hemi = 'E';
    g.quality = nm::FixQuality::dgps;
    g.satellites = 8;
    g.hdop = 0.9f;
    g.altitude = 545.4;
    g.altitude_unit = 'M';
    nm::GGA g2;
    g2.utc_time = 1.5;
    g2.quality = nm::FixQuality::invalid;
    nm::TXT tx;
    tx.total = 1;
    tx.number = 1;
    tx.severity = 2;
    tx.text = "ANTENNA OK";
    GMP_MSG("nmea", nm, GGA, g, g2);
    GMP_MSG("nmea", nm, TXT, tx);
    ProtocolCase<nm::Nmea>{"nmea", "Nmea", {}}.add(g).add(g2).add(tx).done();

    // ---- SHT4x -----------------------------------------------------------------------------------------
    namespace sh = sensirion::sht4x;
    sh::Measurement me;
    me.temperature = 0x6666;
    me.humidity = 0x8A3D;
    sh::SerialNumber sr;
    sr.serial_high = 0x0F1E;
    sr.serial_low = 0x2D3C;
    GMP_MSG("sht4x", sh, MeasureHighPrecision);
    GMP_MSG("sht4x", sh, SoftReset);
    GMP_MSG("sht4x", sh, Measurement, me);
    GMP_MSG("sht4x", sh, SerialNumber, sr);
    ProtocolCase<sh::MeasurementResponse>{"sht4x", "MeasurementResponse", {}}.add(me).done();
    ProtocolCase<sh::SerialNumberResponse>{"sht4x", "SerialNumberResponse", {}}.add(sr).done();
    namespace se = sensirion::sht4x_enum;
    GMP_MSG("sht4x_enum", se, CommandFrame);
    GMP_MSG("sht4x_enum", se, Measurement);

    // ---- stuffed (byte stuffing on every framing) ------------------------------------------------------------------
    namespace st = link::stuffed;
    st::Data d;
    d.data_ = {0x80, 0x81, 0x0D, 0x00, 0x7E, 0x81};
    d.n = 6;
    st::Ping p1;
    p1.seq = 0x80818000;
    st::Ping p2;
    p2.seq = 7;
    GMP_MSG("stuffed", st, Data, d);
    GMP_MSG("stuffed", st, Ping, p1, p2);
    ProtocolCase<st::SyncLink>{"stuffed", "SyncLink", {}}.add(d).add(p1).add(p2).done();
    ProtocolCase<st::WireChecksum>{"stuffed", "WireChecksum", {}}.add(d).add(p1).done();
    ProtocolCase<st::WireLength>{"stuffed", "WireLength", {}}.add(d).add(p1).add(p2).done();
    ProtocolCase<st::CrcLength>{"stuffed", "CrcLength", {}}.add(d).add(p1).done();
    ProtocolCase<st::WireCrcLength>{"stuffed", "WireCrcLength", {}}.add(d).add(p1).add(p2).done();
    ProtocolCase<st::WireCrcOverWire>{"stuffed", "WireCrcOverWire", {}}.add(d).add(p2).done();
    ProtocolCase<st::FixedLink>{"stuffed", "FixedLink", {}}.add(p1).add(p2).done();
    ProtocolCase<st::Delimited>{"stuffed", "Delimited", {}}.add(d).add(p1).done();

    std::printf("{\"messages\":[\n%s],\n\"protocols\":[\n%s]}\n", messages_json.c_str(), protocols_json.c_str());
    return 0;
}
