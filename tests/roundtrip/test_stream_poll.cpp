// Pull API of the stream parser: poll(), poll_frame(), poll_message(), poll_event(), poll_for<V>().

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

#include "nmea.gmp.hpp"
#include "telemetry.gmp.hpp"

namespace tel = acme::telemetry;
namespace nm = nav::nmea;
using Link = tel::BinaryLink;

namespace {

template <class M>
std::vector<std::uint8_t> frame_of(const M& m) {
    std::array<std::uint8_t, Link::max_frame> buf{};
    auto n = Link::encode(m, buf);
    REQUIRE(n);
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)};
}

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

/// Status(100), garbage, Command(7), Log(2)
std::vector<std::uint8_t> wire() {
    std::vector<std::uint8_t> w;
    for (const auto& part : {frame_of(status(100)), std::vector<std::uint8_t>{0x13, 0x37}, frame_of(command(7)),
                             frame_of(log_msg(2))})
        w.insert(w.end(), part.begin(), part.end());
    return w;
}

gmp::bytes_view bv(const std::vector<std::uint8_t>& v) { return {v.data(), v.size()}; }

template <class R>
constexpr bool has_header = requires(R r) { r.header; };

std::string kind(const Link::AnyView& v) {
    return std::visit(gmp::overloaded{[](const tel::StatusView& s) { return "status " + std::to_string(s.voltage()); },
                                      [](const tel::CommandView& c) { return "command " + std::to_string(c.opcode()); },
                                      [](const tel::LogView& l) { return "log " + std::to_string(l.level()); }},
                      v);
}

} // namespace

TEST_CASE("poll: messages in order, partial input, errors skipped and counted") {
    const auto w = wire();
    Link::Stream<> parser;
    parser.push(bv(w).first(5));  // part of the first frame
    CHECK_FALSE(parser.poll());
    parser.push(bv(w).subspan(5));
    std::vector<std::string> got;
    while (auto m = parser.poll()) got.push_back(kind(*m));
    CHECK(got == std::vector<std::string>{"status 100", "command 7", "log 2"});
    CHECK(parser.last_error() == gmp::Error::BadSync);
    CHECK(parser.stats().frames == 3);
    CHECK(parser.stats().bytes_skipped == 2);
    CHECK(parser.buffered() == 0);
}

TEST_CASE("poll: a view stays valid until the parser is used again (Debug check)") {
    const auto w = wire();
    Link::Stream<> parser;
    parser.push(bv(w));
    auto first = parser.poll();
    REQUIRE(first);
    CHECK(first.valid());
    CHECK(kind(*first) == "status 100");
    auto second = parser.poll();  // releases the first frame
    REQUIRE(second);
    CHECK_FALSE(first.valid());
    CHECK(second.valid());
    parser.push(bv(w));  // push also releases
    CHECK_FALSE(second.valid());
    gmp::Polled<Link::AnyView> empty;
    CHECK(empty.valid());
    CHECK_FALSE(empty);
}

TEST_CASE("poll_frame: header and frame bytes") {
    const auto f = frame_of(command(9));
    Link::Stream<> parser;
    parser.push(bv(f));
    auto r = parser.poll_frame();
    REQUIRE(r);
    CHECK(r->header.msg_id() == tel::MsgId::command);
    CHECK(r->header.len() == 3);
    CHECK(r->bytes.size() == f.size());
    CHECK(kind(r->message) == "command 9");
    static_assert(has_header<Link::Received> && !has_header<nm::Nmea::Received>, "text protocols have no header");
}

TEST_CASE("poll_message: struct copies outlive the buffer; failed struct decodes are skipped") {
    // a Log frame whose text is longer than the struct's max (64): the view accepts it, the struct does not
    std::vector<std::uint8_t> body{5};
    body.insert(body.end(), 70, 'x');
    body.push_back(0);
    std::vector<std::uint8_t> bad{0x03, 0x00, static_cast<std::uint8_t>(body.size())};
    bad.insert(bad.end(), body.begin(), body.end());
    const auto crc = gmp::checksum::crc16_ccitt::compute(bv(bad));
    bad.insert(bad.begin(), {0xAA, 0x55});
    bad.push_back(static_cast<std::uint8_t>(crc >> 8));
    bad.push_back(static_cast<std::uint8_t>(crc));

    auto w = frame_of(status(5));
    w.insert(w.end(), bad.begin(), bad.end());
    const auto tail = frame_of(log_msg(1));
    w.insert(w.end(), tail.begin(), tail.end());

    Link::Stream<> parser;
    parser.push(bv(w));
    auto a = parser.poll_message();
    REQUIRE(a);
    auto b = parser.poll_message();
    REQUIRE(b);
    parser.reset();  // the copies do not depend on the buffer
    CHECK(std::get<tel::Status>(*a).voltage == 5);
    CHECK(std::get<tel::Log>(*b).level == 1);
    CHECK(parser.last_error() == gmp::Error::None);  // reset() clears it
}

TEST_CASE("poll_message: a struct decode failure is reported like a bad frame") {
    std::vector<std::uint8_t> body{5};
    body.insert(body.end(), 70, 'x');
    body.push_back(0);
    std::vector<std::uint8_t> bad{0x03, 0x00, static_cast<std::uint8_t>(body.size())};
    bad.insert(bad.end(), body.begin(), body.end());
    const auto crc = gmp::checksum::crc16_ccitt::compute(bv(bad));
    bad.insert(bad.begin(), {0xAA, 0x55});
    bad.push_back(static_cast<std::uint8_t>(crc >> 8));
    bad.push_back(static_cast<std::uint8_t>(crc));

    Link::Stream<> parser;
    parser.push(bv(bad));
    CHECK(parser.poll());  // the view is fine
    // with a stored handler, every dropped frame / byte run reaches its FrameError overload
    std::vector<gmp::Error> errors;
    auto bound = Link::make_stream([&](const gmp::FrameError& e) { errors.push_back(e.error); });
    bound.push(bv(bad));
    CHECK_FALSE(bound.poll_message());
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.front() == gmp::Error::OutOfRange);  // then the rest of the frame is skipped while resyncing
    CHECK(bound.stats().frame_errors == 1);
}

TEST_CASE("poll_event: messages and errors inline, in stream order") {
    const auto w = wire();
    Link::Stream<> parser;
    parser.push(bv(w));
    std::vector<std::string> got;
    while (auto e = parser.poll_event()) {
        std::visit(gmp::overloaded{[&](const Link::AnyView& v) { got.push_back(kind(v)); },
                                   [&](const gmp::FrameError& err) {
                                       got.push_back(std::string(gmp::to_string(err.error)) + " " +
                                                     std::to_string(err.data.size()));
                                   }},
                   *e);
    }
    CHECK(got == std::vector<std::string>{"status 100", "bad sync 2", "command 7", "log 2"});
}

TEST_CASE("CallbackStream: poll reports errors to the error listener, never calls message listeners") {
    const auto w = wire();
    struct Counts {
        int status = 0, errors = 0;
        void on_status(const tel::StatusView&) { ++status; }
        void on_error(const gmp::FrameError&) { ++errors; }
    } c;
    Link::CallbackStream<> link;
    link.on<&Counts::on_status>(c).on<&Counts::on_error>(c);
    link.push(bv(w));
    int polled = 0;
    while (link.poll()) ++polled;
    CHECK(polled == 3);
    CHECK(c.status == 0);
    CHECK(c.errors == 1);
    // poll_event returns errors instead of calling the listener
    link.push(bv(w));
    while (link.poll_event()) {}
    CHECK(c.errors == 1);
}

TEST_CASE("poll_for: other messages go to the listeners while waiting for the reply") {
    std::vector<std::uint8_t> w;
    for (const auto& part : {frame_of(status(1)), frame_of(log_msg(4)), frame_of(command(42)), frame_of(status(2))})
        w.insert(w.end(), part.begin(), part.end());

    struct App {
        std::vector<std::string> seen;
        void on_status(const tel::StatusView& v) { seen.push_back("status " + std::to_string(v.voltage())); }
        void on_log(const Link::HeaderView& h, const tel::LogView&) { seen.push_back("log len " + std::to_string(h.len())); }
    } app;
    Link::CallbackStream<> link;
    link.on<&App::on_status>(app).on<&App::on_log>(app);

    link.push(bv(w).first(10));  // the reply is not complete yet
    CHECK_FALSE(link.poll_for<tel::CommandView>());
    link.push(bv(w).subspan(10));
    auto reply = link.poll_for<tel::CommandView>();
    REQUIRE(reply);
    CHECK(reply->opcode() == 42);
    CHECK(app.seen == std::vector<std::string>{"status 1", "log len 3"});
    // frames after the reply stay buffered until asked for
    CHECK(link.feed(gmp::bytes_view{}) == 1);
    CHECK(app.seen.back() == "status 2");

    // make_stream handlers work too
    int other = 0;
    auto bound = Link::make_stream([&](const tel::StatusView&) { ++other; });
    bound.push(bv(w));
    auto cmd = bound.poll_for<tel::CommandView>();
    REQUIRE(cmd);
    CHECK(other == 1);
}

TEST_CASE("poll on a text protocol") {
    std::string text;
    auto add = [&](const auto& m) {
        std::array<std::uint8_t, nm::Nmea::max_frame> buf{};
        auto n = nm::Nmea::encode(m, buf);
        REQUIRE(n);
        text.append(reinterpret_cast<const char*>(buf.data()), *n);
    };
    nm::TXT t;
    t.total = 1;
    t.number = 1;
    t.severity = 0;
    t.text = "HELLO";
    add(t);
    nm::Nmea::Stream<> parser;
    parser.push(gmp::as_bytes(text.data(), text.size()));
    auto r = parser.poll_frame();
    REQUIRE(r);
    CHECK(std::get<nm::TXTView>(r->message).text() == "HELLO");
    CHECK(std::string(reinterpret_cast<const char*>(r->bytes.data()), r->bytes.size()) == text);
}
