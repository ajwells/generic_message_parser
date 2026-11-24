// Data after a message: reported (extra_bytes(), extra_tokens(), decode(data, extra), Received::extra,
// StreamStats::frames_with_extra) and rejected by protocols with exact_size (tests/schemas/framing.yaml Loose / Exact).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "framing.gmp.hpp"
#include "nmea.gmp.hpp"

namespace fr = test::framing;
namespace nm = nav::nmea;

namespace {

using Bytes = std::vector<std::uint8_t>;

template <class P, class M>
Bytes frame_of(const M& m) {
    std::array<std::uint8_t, 64> buf{};
    auto n = P::encode(m, buf);
    REQUIRE(n);
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)};
}

/// Loose / Exact frame: B5 | id | len | body. `extra` bytes appended to the body (len grows with them).
template <class P>
Bytes reading_frame(std::size_t extra) {
    fr::Reading r;
    r.channel = 3;
    r.value = -7;
    Bytes f = frame_of<P>(r);
    for (std::size_t i = 0; i < extra; ++i) f.push_back(static_cast<std::uint8_t>(0xE0 + i));
    f[2] = static_cast<std::uint8_t>(f[2] + extra);
    return f;
}

std::string nmea(std::string_view body) {  // "$" body "*" xor8 "\r\n"
    std::uint8_t x = 0;
    for (char c : body) x ^= static_cast<std::uint8_t>(c);
    static const char* hex = "0123456789ABCDEF";
    return "$" + std::string(body) + "*" + hex[x >> 4] + hex[x & 15] + "\r\n";
}

gmsg::bytes_view bv(const std::string& s) { return gmsg::as_bytes(s.data(), s.size()); }

} // namespace

TEST_CASE("binary views and struct decode report the bytes after the message") {
    fr::Reading r;
    r.channel = 1;
    r.value = 2;
    std::array<std::uint8_t, 16> buf{};
    const auto n = r.encode(buf);
    REQUIRE(n);
    const gmsg::bytes_view exact(buf.data(), *n), longer(buf.data(), *n + 3);
    CHECK(fr::ReadingView::from(exact)->extra_bytes() == 0);
    CHECK(fr::ReadingView::from(longer)->extra_bytes() == 3);
    CHECK(fr::ReadingView::unchecked(longer).extra_bytes() == 3);
    std::size_t extra = 99;
    REQUIRE(fr::Reading::decode(longer, extra));
    CHECK(extra == 3);
    REQUIRE(fr::Reading::decode(exact, extra));
    CHECK(extra == 0);
    CHECK(fr::Reading::decode(longer) == fr::Reading::decode(exact));  // the plain decode ignores the extra bytes

    // variable size: the message ends where its fields say
    fr::Note note;
    note.text = "abc";
    note.n = 3;
    const auto m = note.encode(buf);
    REQUIRE(m);
    CHECK(fr::NoteView::from(gmsg::bytes_view(buf.data(), *m + 2))->extra_bytes() == 2);
}

TEST_CASE("text views and struct decode report the tokens after the message") {
    const std::string rec = "01,02,03,hello,x,y";
    const auto v = nm::TXTView::from(rec);
    REQUIRE(v);
    CHECK(v->extra_tokens() == 2);
    CHECK(nm::TXTView::from(std::string_view("01,02,03,hello"))->extra_tokens() == 0);
    CHECK(nm::TXTView::from(std::string_view("01,02"))->extra_tokens() == 0);  // missing tokens are not negative extra
    std::size_t extra = 0;
    const auto d = nm::TXT::decode_text(rec, extra);
    REQUIRE(d);
    CHECK(extra == 2);
    CHECK(d->text.view() == "hello");
}

TEST_CASE("without exact_size, frames with extra data are delivered and counted") {
    const auto good = reading_frame<fr::Loose>(0), longer = reading_frame<fr::Loose>(3);
    fr::Loose::Stream<> s;
    (void)s.push(good);
    (void)s.push(longer);
    auto a = s.poll_frame();
    REQUIRE(a);
    CHECK(a->extra == 0);
    auto b = s.poll_frame();
    REQUIRE(b);
    CHECK(b->extra == 3);
    CHECK(std::get<fr::ReadingView>(b->message).extra_bytes() == 3);
    CHECK(s.stats().frames == 2);
    CHECK(s.stats().frames_with_extra == 1);
    CHECK(s.stats().frame_errors == 0);

    // whole-buffer parse: the view reports it
    Bytes buf = longer;
    const auto v = fr::Loose::parse(gmsg::mut_bytes(buf.data(), buf.size()));
    REQUIRE(v);
    CHECK(gmsg::extra_of(*v) == 3);
}

TEST_CASE("exact_size: a frame with data after its message fails with BadLength") {
    const auto good = reading_frame<fr::Exact>(0), longer = reading_frame<fr::Exact>(2);
    Bytes buf = longer;
    CHECK(fr::Exact::parse(gmsg::mut_bytes(buf.data(), buf.size())).error() == gmsg::Error::BadLength);
    buf = good;
    CHECK(fr::Exact::parse(gmsg::mut_bytes(buf.data(), buf.size())));

    std::vector<gmsg::Error> errors;
    int delivered = 0;
    auto handler = gmsg::overloaded{[&](const gmsg::FrameError& e) { errors.push_back(e.error); },
                                   [&](const auto&...) { ++delivered; }};
    fr::Exact::Stream<> s;
    Bytes wire = good;
    wire.insert(wire.end(), longer.begin(), longer.end());
    wire.insert(wire.end(), good.begin(), good.end());
    (void)s.feed(wire, handler);
    CHECK(delivered == 2);
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.front() == gmsg::Error::BadLength);
    CHECK(s.stats().frames_with_extra == 0);
}

TEST_CASE("text protocol: extra tokens in a frame are counted, Received::extra holds them") {
    const auto with_extra = nmea("GPTXT,01,02,03,hi,more"), plain = nmea("GPTXT,01,02,03,hi");
    nm::Nmea::Stream<> s;
    (void)s.push(bv(plain));
    (void)s.push(bv(with_extra));
    auto a = s.poll_frame();
    REQUIRE(a);
    CHECK(a->extra == 0);
    auto b = s.poll_frame();
    REQUIRE(b);
    CHECK(b->extra == 1);
    CHECK(s.stats().frames_with_extra == 1);
}

TEST_CASE("length check by message type: an impossible length is rejected as soon as the header arrives") {
    // Exact: a Reading body must be exactly 5 bytes; a header claiming 30 is rejected at once (no waiting)
    {
        fr::Exact::Stream<> s;
        const std::array<std::uint8_t, 3> header{0xB5, 0x01, 30};
        (void)s.push(header);
        const auto e = s.poll_event();
        REQUIRE(e);
        REQUIRE(std::holds_alternative<gmsg::FrameError>(*e));
        CHECK(std::get<gmsg::FrameError>(*e).error == gmsg::Error::BadLength);
    }
    // Loose: a longer body is allowed (data after the message), but a Reading body of 2 bytes is impossible
    {
        fr::Loose::Stream<> s;
        const std::array<std::uint8_t, 3> short_len{0xB5, 0x01, 2};
        (void)s.push(short_len);
        const auto e = s.poll_event();
        REQUIRE(e);
        CHECK(std::get<gmsg::FrameError>(*e).error == gmsg::Error::BadLength);
        while (s.poll_event()) {}  // the rest of the rejected header is skipped (BadSync)
        CHECK(s.buffered() == 0);
        const std::array<std::uint8_t, 3> long_len{0xB5, 0x01, 30};
        (void)s.push(long_len);
        CHECK_FALSE(s.poll_event());  // could be a Reading with 25 bytes after it: waits (resync() / tick() recover)
    }
    // unknown type ids are not checked here (they fail with UnknownType once the frame is complete)
    {
        fr::Exact::Stream<> s;
        const std::array<std::uint8_t, 3> header{0xB5, 0x09, 30};
        (void)s.push(header);
        CHECK_FALSE(s.poll_event());
    }
}
