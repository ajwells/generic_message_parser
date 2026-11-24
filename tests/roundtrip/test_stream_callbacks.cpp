// Stored stream handlers: make_stream / StreamWith (compile-time handler) and CallbackStream (runtime callbacks).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

#include "nmea.gmp.hpp"
#include "telemetry.gmp.hpp"

namespace tel = acme::telemetry;
namespace nm = nav::nmea;

namespace {

/// Status, garbage, Command, Log frames (BinaryLink, which has a header: msg_id + len).
std::vector<std::uint8_t> telemetry_wire() {
    std::vector<std::uint8_t> wire;
    auto append = [&](const auto& m) {
        std::array<std::uint8_t, tel::BinaryLink::max_frame> buf{};
        auto n = tel::BinaryLink::encode(m, buf);
        REQUIRE(n);
        wire.insert(wire.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    };
    tel::Status s;
    s.voltage = 1234;
    s.flags.mode = tel::Mode::run;
    s.label = "x";
    append(s);
    wire.insert(wire.end(), {0x13, 0x37});  // garbage -> FrameError (bad sync)
    tel::Command c;
    c.opcode = 7;
    append(c);
    tel::Log l;
    l.level = 3;
    l.text = "hi";
    append(l);
    return wire;
}

std::string nmea_wire() {
    std::string out;
    auto append = [&](const auto& m) {
        std::array<std::uint8_t, nm::Nmea::max_frame> buf{};
        auto n = nm::Nmea::encode(m, buf);
        REQUIRE(n);
        out.append(reinterpret_cast<const char*>(buf.data()), *n);
    };
    nm::GGA g;
    g.utc_time = 1.0;
    g.quality = nm::FixQuality::gps;
    g.satellites = 5;
    append(g);
    nm::TXT t;
    t.total = 1;
    t.number = 1;
    t.severity = 2;
    t.text = "OK";
    append(t);
    return out;
}

gmsg::bytes_view bytes(const std::vector<std::uint8_t>& v) { return {v.data(), v.size()}; }
gmsg::bytes_view bytes(const std::string& s) { return gmsg::as_bytes(s.data(), s.size()); }

struct App {
    std::vector<std::string> log;
    void on_status(const tel::StatusView& v) { log.push_back("status " + std::to_string(v.voltage())); }
    void on_command(const tel::BinaryLink::HeaderView& h, const tel::CommandView& v) {
        log.push_back("command " + std::to_string(v.opcode()) + " id " + std::string(tel::to_string(h.msg_id())));
    }
    void on_any(const tel::BinaryLink::AnyView& v) { log.push_back("frame " + std::to_string(v.index())); }
    void on_error(const gmsg::FrameError& e) { log.push_back("error " + std::string(gmsg::to_string(e.error))); }
    void on_gga(const nm::GGAView& v) const { const_cast<App*>(this)->log.push_back("gga " + std::to_string(*v.satellites())); }
};

std::vector<std::string>* g_log = nullptr;
void free_log(const tel::LogView& v) { g_log->push_back("log " + std::to_string(v.level())); }
void typed_log(App& app, const tel::LogView& v) { app.log.push_back("typed log " + std::to_string(v.level())); }
void c_style_log(void* ctx, const tel::LogView& v) {
    static_cast<App*>(ctx)->log.push_back("c log " + std::to_string(v.level()));
}

/// A named handler type: usable as a class member through StreamWith.
struct NamedHandler {
    std::vector<std::string>* out;
    void operator()(const tel::StatusView&) const { out->push_back("status"); }
    void operator()(const tel::BinaryLink::HeaderView& h, const tel::LogView&) const {
        out->push_back("log len " + std::to_string(h.len()));
    }
};

struct Device {
    std::vector<std::string> seen;
    tel::BinaryLink::StreamWith<NamedHandler> parser{NamedHandler{&seen}};
};

} // namespace

TEST_CASE("make_stream stores an overload set: feed(bytes) without a handler") {
    const auto wire = telemetry_wire();
    std::vector<std::string> seen;
    auto parser = tel::BinaryLink::make_stream(gmsg::overloaded{
        [&](const tel::StatusView& v) { seen.push_back("status " + std::to_string(v.voltage())); },
        [&](const tel::BinaryLink::HeaderView& h, const tel::CommandView&) { seen.push_back("command len " + std::to_string(h.len())); },
        [&](const gmsg::FrameError& e) { seen.push_back(std::string(gmsg::to_string(e.error))); },
    });
    for (std::size_t i = 0; i < wire.size(); i += 3)  // chunked
        parser.feed(bytes(wire).subspan(i, std::min<std::size_t>(3, wire.size() - i)));
    // Log has no overload: ignored, as with a handler passed per call
    CHECK(seen == std::vector<std::string>{"status 1234", "bad sync", "command len 3"});
    CHECK(parser.stats().frames == 3);
}

TEST_CASE("StreamWith<NamedHandler> works as a class member; next() uses the stored handler") {
    const auto wire = telemetry_wire();
    Device d;
    d.parser.push(bytes(wire));
    while (d.parser.next()) {}
    CHECK(d.seen == std::vector<std::string>{"status", "log len 4"});  // level + "hi\0"
}

TEST_CASE("CallbackStream: member functions, header only when asked, every-frame and error listeners") {
    const auto wire = telemetry_wire();
    App app;
    tel::BinaryLink::CallbackStream<> parser;
    parser.on<&App::on_status>(app)      // (View): header dropped
        .on<&App::on_command>(app)       // (HeaderView, View): header passed
        .on<&App::on_any>(app)           // AnyView: every frame, after the message listener
        .on<&App::on_error>(app);        // FrameError
    CHECK(parser.has<tel::StatusView>());
    CHECK_FALSE(parser.has<tel::LogView>());
    CHECK(parser.feed(bytes(wire)) == 3);
    CHECK(app.log == std::vector<std::string>{"status 1234", "frame 0", "error bad sync", "command 7 id command", "frame 1",
                                              "frame 2"});
}

TEST_CASE("CallbackStream: free functions, typed and void* contexts, lambdas, off() and clear()") {
    const auto wire = telemetry_wire();
    App app;
    std::vector<std::string> global;
    g_log = &global;
    tel::BinaryLink::CallbackStream<> parser;

    parser.on<&free_log>();  // no context
    parser.feed(bytes(wire));
    CHECK(global == std::vector<std::string>{"log 3"});

    parser.on<&typed_log>(app);  // re-binding replaces the listener (one per slot)
    parser.feed(bytes(wire));
    parser.on(&c_style_log, static_cast<void*>(&app));  // run-time function pointer, C-style context
    parser.feed(bytes(wire));
    parser.on<[](App& a, const tel::BinaryLink::HeaderView& h, const tel::StatusView&) {
        a.log.push_back("lambda status id " + std::string(tel::to_string(h.msg_id())));
    }>(app);
    parser.feed(bytes(wire));
    CHECK(app.log == std::vector<std::string>{"typed log 3", "c log 3", "lambda status id status", "c log 3"});

    parser.off<tel::LogView>();
    CHECK_FALSE(parser.has<tel::LogView>());
    app.log.clear();
    parser.feed(bytes(wire));
    CHECK(app.log == std::vector<std::string>{"lambda status id status"});

    parser.clear();
    app.log.clear();
    CHECK(parser.feed(bytes(wire)) == 3);  // frames are still parsed and counted, nobody listens
    CHECK(app.log.empty());
    CHECK(parser.stats().frames == 18);  // 6 feeds x 3 frames
}

TEST_CASE("CallbackStream on a protocol without a header (text)") {
    const auto wire = nmea_wire();
    App app;
    std::vector<std::string> any;
    nm::Nmea::CallbackStream<> parser;
    parser.on<&App::on_gga>(app);
    parser.on<[](std::vector<std::string>& out, const nm::Nmea::AnyView& v) {
        out.push_back(std::holds_alternative<nm::TXTView>(v) ? "txt" : "gga");
    }>(any);
    CHECK(parser.feed(bytes(wire)) == 2);
    CHECK(app.log == std::vector<std::string>{"gga 5"});
    CHECK(any == std::vector<std::string>{"gga", "txt"});
    // the Callbacks object can be used directly as a handler too
    nm::Nmea::Callbacks cb;
    int n = 0;
    cb.on<[](int& k, const nm::TXTView&) { ++k; }>(n);
    nm::Nmea::Stream<> plain;
    plain.feed(bytes(wire), cb);
    CHECK(n == 1);
}
