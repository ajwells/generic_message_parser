// Reference data for the Studio cross-check (tools/schema-studio/tests/crosscheck.html).
//
// Encodes messages and noisy frame streams with the generated C++ code, decodes them again and writes JSON:
//   messages: { schema, target, hex, multi, single }   one message decoded on its own (gmp::print output)
//   streams:  { schema, target, hex, frames: [...] }   a byte stream with noise and corrupted frames; `frames` is
//                                                      the gmp::print output of every frame the C++ Stream parser
//                                                      delivered, in order
//   outcomes: { schema, target, hex, error }           whether a (mutated) message decodes: error 0 or the
//                                                      gmp::Error code (value limits: which inputs are rejected)
// The Studio decoder must produce the same text, which compares every decoded value.

#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "can.gmp.hpp"
#include "computed.gmp.hpp"
#include "enums.gmp.hpp"
#include "limits.gmp.hpp"
#include "fieldcrc.gmp.hpp"
#include "framing.gmp.hpp"
#include "nmea.gmp.hpp"
#include "sht4x.gmp.hpp"
#include "stuffed.gmp.hpp"
#include "telemetry.gmp.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

std::string js(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c == '\n') {
            o += "\\n";
        } else if (c == '\r') {
            o += "\\r";
        } else if (c < 0x20 || c >= 0x7F) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += static_cast<char>(c);
        }
    }
    return o + "\"";
}

std::string hex(const Bytes& b) {
    std::string out;
    for (auto x : b) {
        char t[4];
        std::snprintf(t, sizeof t, "%02X ", x);
        out += t;
    }
    return out;
}

template <class T>
std::string text(const T& v, gmp::PrintStyle style = gmp::PrintStyle::multi_line) {
    std::string s;
    (void)gmp::print(v, s, {.style = style});
    return s;
}

std::vector<std::string> messages, streams, outcomes;
int failures = 0;

void fail(const char* what, const char* target) {
    std::fprintf(stderr, "crossref: %s failed for %s\n", what, target);
    ++failures;
}

/// Encode a message on its own, decode it again and record the printed struct.
template <class M>
void message(const char* schema, const char* target, const M& m) {
    std::array<std::uint8_t, 512> buf{};
    auto n = m.encode(buf);
    if (!n) return fail("encode", target);
    const Bytes bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    auto d = M::decode(gmp::bytes_view(bytes.data(), bytes.size()));
    if (!d) return fail("decode", target);
    messages.push_back("{\"schema\":" + js(schema) + ",\"target\":" + js(target) + ",\"hex\":" + js(hex(bytes)) +
                       ",\"multi\":" + js(text(*d)) + ",\"single\":" + js(text(*d, gmp::PrintStyle::single_line)) + "}");
}

/// Record whether `bytes` decodes as M (0) or the error code.
template <class M>
void outcome(const char* schema, const char* target, const Bytes& bytes) {
    const auto d = M::decode(gmp::bytes_view(bytes.data(), bytes.size()));
    outcomes.push_back("{\"schema\":" + js(schema) + ",\"target\":" + js(target) + ",\"hex\":" + js(hex(bytes)) +
                       ",\"error\":" + std::to_string(d ? 0 : static_cast<int>(d.error())) + "}");
}

/// Outcomes of `m`'s encoding with each byte changed in a few ways (binary) or each character replaced (text).
template <class M>
void outcomes_of(const char* schema, const char* target, const M& m, bool text = false) {
    std::array<std::uint8_t, 512> buf{};
    auto n = m.encode(buf);
    if (!n) return fail("encode", target);
    const Bytes bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    outcome<M>(schema, target, bytes);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        for (const std::uint8_t x : text ? std::vector<std::uint8_t>{'9', '-', '.', '0'} : std::vector<std::uint8_t>{0x01, 0x20, 0x80, 0xFF}) {
            Bytes b = bytes;
            if (text) b[i] = x;
            else b[i] ^= x;
            outcome<M>(schema, target, b);
        }
    }
}

/// Builds a noisy stream for protocol P: frames, garbage and a corrupted copy of a frame.
template <class P>
struct Noisy {
    const char* schema;
    const char* target;
    Bytes wire;

    template <class M>
    Bytes frame(const M& m) {
        std::array<std::uint8_t, 1024> buf{};
        auto n = P::encode(m, buf);
        if (!n) {
            fail("frame encode", target);
            return {};
        }
        return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)};
    }
    template <class M>
    Noisy& add(const M& m) {
        const auto f = frame(m);
        wire.insert(wire.end(), f.begin(), f.end());
        return *this;
    }
    /// The frame with one byte near the middle changed (bad checksum / bad length / bad escape).
    template <class M>
    Noisy& corrupt(const M& m) {
        auto f = frame(m);
        if (!f.empty()) f[f.size() / 2] ^= 0x5A;
        wire.insert(wire.end(), f.begin(), f.end());
        return *this;
    }
    Noisy& noise(std::initializer_list<std::uint8_t> b) {
        wire.insert(wire.end(), b.begin(), b.end());
        return *this;
    }
    void done() {
        std::vector<std::string> printed;
        typename P::template Stream<> parser;
        parser.feed(gmp::bytes_view(wire.data(), wire.size()), [&](const auto& v) {
            using V = std::remove_cvref_t<decltype(v)>;
            if constexpr (!std::is_same_v<V, gmp::FrameError>) printed.push_back(text(v));
        });
        std::string frames;
        for (const auto& p : printed) frames += (frames.empty() ? "" : ",") + js(p);
        streams.push_back("{\"schema\":" + js(schema) + ",\"target\":" + js(target) + ",\"hex\":" + js(hex(wire)) +
                          ",\"frames\":[" + frames + "]}");
    }
};

} // namespace

int main() {
    // ---- telemetry -----------------------------------------------------------------------------
    {
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
        t::Command c;
        c.opcode = 0xBEEF;
        c.args = {0x00, 0x7F, 0xAB, 0x10};
        c.arg_len = 4;
        t::Log l;
        l.level = 200;
        l.text = "tab\there\x01";
        l.tail = {9, 8, 7};
        for (auto m : {t::Mode::run, t::Mode::fault, static_cast<t::Mode>(6)}) message("telemetry", "message:Status", status(m));
        message("telemetry", "message:Command", c);
        message("telemetry", "message:Log", l);
        Noisy<t::BinaryLink>{"telemetry", "protocol:BinaryLink", {}}
            .add(status(t::Mode::run))
            .noise({0x13, 0x37, 0xAA})
            .add(c)
            .corrupt(status(t::Mode::fault))
            .add(l)
            .add(status(t::Mode::fault))
            .done();
    }
    // ---- can -----------------------------------------------------------------------------------
    {
        namespace v = vehicle::can;
        v::EngineData e;
        e.signals.rpm = 1601;
        e.signals.gear = v::Gear::drive;
        e.signals.coolant = 130;
        e.signals.throttle = 457;
        e.signals.counter = 9;
        e.signals.flags = 0x1234;
        message("can", "message:EngineData", e);
        v::Sensor s;
        s.temp = -1234;
        s.humidity = 55;
        message("can", "message:Sensor", s);
        v::Sensor s2;
        s2.temp = 2500;
        s2.humidity = 40;
        Noisy<v::FixedLink>{"can", "protocol:FixedLink", {}}.add(s).noise({0xA5, 0x00}).corrupt(s2).add(s2).done();
    }
    // ---- computed ------------------------------------------------------------------------------
    {
        test::computed::Packed p;
        p.b0 = 0x12;
        p.b1 = 0x34;
        p.b2 = 0x56;
        p.wide = 0xDEADBEEF;
        p.lo = 1001;
        p.flags.ready = true;
        p.flags.level = 77;
        p.unit = test::computed::Unit::cm;
        p.items = {1, 2, 3};
        p.n = 3;
        message("computed", "message:Packed", p);
    }
    // ---- field CRCs and conversions --------------------------------------------------------------
    {
        namespace f = test::fieldcrc;
        f::Mixed m;
        m.state = f::State::on;
        m.items = {1, 0xFFFF};
        m.count = 2;
        m.level = -7;
        message("fieldcrc", "message:Mixed", m);
        m.state = f::State::error;
        m.extra = 0x0102;
        message("fieldcrc", "message:Mixed", m);
        f::Reading r;
        r.raw_ = -1234;
        message("fieldcrc", "message:Reading", r);
    }
    // ---- strict enums ----------------------------------------------------------------------------
    {
        namespace e = test::enums;
        e::Sample s;
        s.color = e::Color::green;
        s.open = static_cast<e::Open>(9);  // not strict: any value decodes
        s.flags.mode = e::Mode::run;
        s.levels = {e::Level::low, e::Level::high};
        s.inner.color = e::Color::blue;
        s.more = {e::Color::red, e::Color::blue};
        s.n = 2;
        message("enums", "message:Sample", s);
        e::Tail t;
        t.rest = {e::Color::red, e::Color::green, e::Color::blue};
        message("enums", "message:Tail", t);
    }
    // ---- value limits: decoded values and which mutated inputs are rejected --------------------------
    {
        namespace l = test::limits;
        l::Sample s;
        s.humidity = 3146;
        s.temp = -304;
        s.big = 0xFFFFFFFFFFFFFFFEull;
        s.neg = -1;
        s.ratio = 1.5f;
        s.wide = 1e300;
        s.flags.level = 5;
        s.flags.trim = -10;
        s.flags.gain = 200;
        s.levels = {10, 1000};
        s.inner.pct = 100;
        s.more = {-50, 0, 127};
        s.n = 3;
        s.opt = 2;
        message("limits", "message:Sample", s);
        outcomes_of("limits", "message:Sample", s);
        l::Tail t;
        t.rest = {500, 0, 499};
        message("limits", "message:Tail", t);
        outcomes_of("limits", "message:Tail", t);
        l::Line ln;
        ln.speed = 99.5;
        ln.count = 999;
        ln.opt = -5;
        ln.level = 500;
        message("limits", "message:Line", ln);
        outcomes_of("limits", "message:Line", ln, true);
    }
    // ---- sht4x -----------------------------------------------------------------------------------
    {
        sensirion::sht4x::Measurement m;
        m.temperature = 0x6666;
        m.humidity = 0x8A3D;
        message("sht4x", "message:Measurement", m);
        sensirion::sht4x::SerialNumber sr;
        sr.serial_high = 0x0F1E;
        sr.serial_low = 0x2D3C;
        message("sht4x", "message:SerialNumber", sr);
    }
    // ---- byte stuffing on every framing ------------------------------------------------------------
    {
        namespace s = link::stuffed;
        s::Data d;
        d.data_ = {0x80, 0x81, 0x0D, 0x00, 0x7E, 0x81};
        d.n = 6;
        s::Ping p;
        p.seq = 0x80818000;
        s::Ping p2;
        p2.seq = 7;
        Noisy<s::SyncLink>{"stuffed", "protocol:SyncLink", {}}.add(d).noise({0x81, 0x01}).corrupt(p).add(p).add(p2).done();
        Noisy<s::WireChecksum>{"stuffed", "protocol:WireChecksum", {}}.add(d).corrupt(d).add(p).done();
        Noisy<s::WireLength>{"stuffed", "protocol:WireLength", {}}.add(d).noise({0x00}).add(p).corrupt(p2).add(p2).done();
        Noisy<s::CrcLength>{"stuffed", "protocol:CrcLength", {}}.add(p).corrupt(d).add(d).done();
        Noisy<s::WireCrcLength>{"stuffed", "protocol:WireCrcLength", {}}.add(d).add(p).corrupt(p).add(p2).done();
        Noisy<s::WireCrcOverWire>{"stuffed", "protocol:WireCrcOverWire", {}}.add(d).corrupt(d).add(p2).done();
        Noisy<s::FixedLink>{"stuffed", "protocol:FixedLink", {}}.add(p).noise({0x42}).corrupt(p2).add(p2).done();
        Noisy<s::Delimited>{"stuffed", "protocol:Delimited", {}}.add(d).noise({0x0D, 0x55}).corrupt(p).add(p).add(d).done();
    }
    // ---- framing options: length adjust and coverage, little-endian length / checksum, end after escaping -------
    {
        namespace f = test::framing;
        f::Reading r;
        r.channel = 3;
        r.value = -123456;
        f::Reading r2;
        r2.channel = 0x7E;  // reserved on the escaped links
        r2.value = 0x0D7D7EC0;
        f::Note n;
        n.text = "hi \x7E\x0D";  // reserved bytes inside the string
        n.n = 5;
        message("framing", "message:Reading", r);
        message("framing", "message:Note", n);
        Noisy<f::Adjusted>{"framing", "protocol:Adjusted", {}}.add(r).noise({0x00, 0xA5}).corrupt(n).add(n).add(r2).done();
        Noisy<f::WholeFrame>{"framing", "protocol:WholeFrame", {}}.add(n).noise({0x55}).add(r).corrupt(r2).add(r2).done();
        Noisy<f::EscapedWithEnd>{"framing", "protocol:EscapedWithEnd", {}}.add(r2).noise({0x0D, 0x7D}).add(n).corrupt(r).add(r).done();
        Noisy<f::FixedWithEnd>{"framing", "protocol:FixedWithEnd", {}}.add(r).noise({0xC1}).corrupt(r2).add(r2).add(r).done();
    }
    // ---- NMEA text ---------------------------------------------------------------------------------
    {
        namespace n = nav::nmea;
        n::GGA g;
        g.utc_time = 123519.0;
        g.latitude = 4807.038;
        g.lat_hemi = 'N';
        g.longitude = 1131.0;
        g.lon_hemi = 'E';
        g.quality = n::FixQuality::dgps;
        g.satellites = 8;
        g.hdop = 0.9f;
        g.altitude = 545.4;
        g.altitude_unit = 'M';
        n::GGA g2;
        g2.utc_time = 1.5;
        g2.quality = n::FixQuality::invalid;
        n::TXT t;
        t.total = 1;
        t.number = 1;
        t.severity = 2;
        t.text = "ANTENNA OK";
        Noisy<n::Nmea>{"nmea", "protocol:Nmea", {}}
            .add(g)
            .noise({'x', 'y', '\r', '\n'})
            .add(g2)
            .corrupt(t)
            .add(t)
            .done();
    }

    // ---- built-in checksums over two inputs ---------------------------------------------------------
    std::vector<std::string> sums;
    {
        const std::string check = "123456789";
        Bytes ramp(256);
        for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<std::uint8_t>(i * 7 + 3);
        const gmp::bytes_view inputs[] = {gmp::as_bytes(check.data(), check.size()), gmp::bytes_view(ramp.data(), ramp.size())};
        auto add = [&]<class C>(const char* name, C) {
            for (std::size_t k = 0; k < 2; ++k) {
                char v[24];
                std::snprintf(v, sizeof v, "%llX", static_cast<unsigned long long>(C::compute(inputs[k])));
                sums.push_back("{\"algo\":" + js(name) + ",\"input\":" + std::to_string(k) + ",\"value\":" + js(v) + "}");
            }
        };
        namespace c = gmp::checksum;
        add("crc8", c::crc8{});
        add("crc8_maxim", c::crc8_maxim{});
        add("crc8_autosar", c::crc8_autosar{});
        add("crc8_nrsc5", c::crc8_nrsc5{});
        add("crc15_can", c::crc15_can{});
        add("crc16_ccitt", c::crc16_ccitt{});
        add("crc16_xmodem", c::crc16_xmodem{});
        add("crc16_kermit", c::crc16_kermit{});
        add("crc16_x25", c::crc16_x25{});
        add("crc16_modbus", c::crc16_modbus{});
        add("crc16_arc", c::crc16_arc{});
        add("crc24_openpgp", c::crc24_openpgp{});
        add("crc32", c::crc32{});
        add("crc32c", c::crc32c{});
        add("crc32_mpeg2", c::crc32_mpeg2{});
        add("crc64_ecma", c::crc64_ecma{});
        add("xor8", c::xor8{});
        add("sum8", c::sum8{});
        add("lrc8", c::lrc8{});
        add("fletcher16", c::fletcher16{});
    }

    std::printf("{\"checksums\":[\n");
    for (std::size_t i = 0; i < sums.size(); ++i) std::printf("%s%s\n", sums[i].c_str(), i + 1 < sums.size() ? "," : "");
    std::printf("],\"messages\":[\n");
    for (std::size_t i = 0; i < messages.size(); ++i) std::printf("%s%s\n", messages[i].c_str(), i + 1 < messages.size() ? "," : "");
    std::printf("],\"streams\":[\n");
    for (std::size_t i = 0; i < streams.size(); ++i) std::printf("%s%s\n", streams[i].c_str(), i + 1 < streams.size() ? "," : "");
    std::printf("],\"outcomes\":[\n");
    for (std::size_t i = 0; i < outcomes.size(); ++i) std::printf("%s%s\n", outcomes[i].c_str(), i + 1 < outcomes.size() ? "," : "");
    std::printf("]}\n");
    return failures ? 1 : 0;
}
