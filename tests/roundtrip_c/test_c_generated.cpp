// Cross-check of generated C against generated C++ for the same schemas.
//
// The generated C (compiled as C11 in the gmp_test_schemas_c library) and the generated C++ decode the same inputs:
// valid encodings, every truncation, single-byte corruptions, trailing bytes and random bytes. For every input both
// must report the same error; on success every field (structs and views), conversion and computed value must match,
// re-encoding must give the same bytes, and print must give the same text as gmp::print (print_compare.hpp). The C headers are included from C++ here (they are C++ compatible).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

// generated C++
#include "computed.gmp.hpp"
#include "enums.gmp.hpp"
#include "limits.gmp.hpp"
#include "fieldcrc.gmp.hpp"
#include "hdlc.gmp.hpp"
#include "telemetry.gmp.hpp"
#include "print_compare.hpp"
// generated C
#include "computed.gmp.h"
#include "enums.gmp.h"
#include "limits.gmp.h"
#include "fieldcrc.gmp.h"
#include "hdlc.gmp.h"
#include "telemetry.gmp.h"

namespace {

using Bytes = std::vector<std::uint8_t>;

template <class E>
constexpr auto u(E e) { return std::to_underlying(e); }

std::string_view sv(const char* p, std::size_t n) { return {p, n}; }
std::string_view sv(gmsg_str s) { return {s.data, s.size}; }

int code(gmp::Error e) { return static_cast<int>(e); }

/// Inputs derived from valid encodings: the encodings, every truncation, byte corruptions, trailing bytes.
std::vector<Bytes> mutations(const std::vector<Bytes>& valid, unsigned seed) {
    std::vector<Bytes> out;
    std::mt19937 rng(seed);
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
        t.push_back(0xEE);
        t.push_back(0x00);
        out.push_back(t);
    }
    for (int k = 0; k < 300; ++k) {
        Bytes r(rng() % 48);
        for (auto& b : r) b = static_cast<std::uint8_t>(rng() % 4 == 0 ? rng() % 4 : rng());
        out.push_back(r);
    }
    return out;
}

template <class C>
struct CApi {
    gmsg_error (*decode)(const uint8_t*, size_t, C*);
    gmsg_error (*encode)(const C*, uint8_t*, size_t, size_t*);
    void (*print)(const C*, gmsg_printer*) = nullptr;
};

template <class CV>
struct CViewApi {
    gmsg_error (*from)(const uint8_t*, size_t, CV*);
    void (*print)(const CV*, gmsg_printer*) = nullptr;
};

/// Decode every input with both languages and compare (structs, re-encoding, views).
template <class Cpp, class CppView, class C, class CV, class SameStruct, class SameView>
void cross_check(const std::vector<Bytes>& valid, CApi<C> capi, CViewApi<CV> vapi, SameStruct same, SameView same_view,
                 unsigned seed) {
    int decoded = 0, failed = 0;
    for (const auto& in : mutations(valid, seed)) {
        const gmp::bytes_view bv(in.data(), in.size());
        INFO("input size " << in.size());
        // struct
        const auto cpp = Cpp::decode(bv);
        C c;
        const auto ce = capi.decode(in.data(), in.size(), &c);
        REQUIRE(static_cast<int>(ce) == (cpp ? 0 : code(cpp.error())));
        if (cpp) {
            ++decoded;
            same(*cpp, c);
            print_compare::same(*cpp, capi.print, c);
            std::array<std::uint8_t, 512> a{}, b{};
            const auto n = cpp->encode(a);
            std::size_t cn = 0;
            const auto e = capi.encode(&c, b.data(), b.size(), &cn);
            REQUIRE(static_cast<int>(e) == (n ? 0 : code(n.error())));
            if (n) {
                REQUIRE(cn == *n);
                CHECK(std::memcmp(a.data(), b.data(), cn) == 0);
                if (cn > 0) {  // one byte too small: both report BufferFull
                    const auto small = cpp->encode(gmp::mut_bytes(a.data(), *n - 1));
                    CHECK(static_cast<int>(capi.encode(&c, b.data(), cn - 1, nullptr)) == code(small.error()));
                }
            }
        } else {
            ++failed;
        }
        // view
        const auto cppv = CppView::from(bv);
        CV v;
        const auto ve = vapi.from(in.data(), in.size(), &v);
        REQUIRE(static_cast<int>(ve) == (cppv ? 0 : code(cppv.error())));
        if (cppv) {
            REQUIRE(v.size == cppv->wire_size());
            CHECK(v.extra == cppv->extra_bytes());
            CHECK(v.extra == in.size() - v.size);
            same_view(*cppv, v);
            print_compare::same(*cppv, vapi.print, v);
        }
    }
    // the inputs must exercise both outcomes
    CHECK(decoded > 0);
    CHECK(failed > 0);
}

template <class T>
Bytes enc(const T& m) {
    std::array<std::uint8_t, 512> buf{};
    const auto n = m.encode(buf);
    REQUIRE(n);
    return Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
}

} // namespace

// ---- telemetry: const, scale, bits (bool, enum, signed), derived count, array, conditional nested message, pad,
//      padded fixed string, bytes with an expression length, terminated string, remaining bytes ------------------
namespace at = acme::telemetry;

namespace {

void same_ext(const at::Extended& a, const acme_telemetry_extended& b) {
    CHECK(a.temperature == b.temperature);
    CHECK(a.pressure == b.pressure);
    CHECK(a.temperature_scaled() == acme_telemetry_extended_temperature_scaled(&b));
}

void same_status(const at::Status& a, const acme_telemetry_status& b) {
    CHECK(a.voltage == b.voltage);
    CHECK(a.voltage_scaled() == acme_telemetry_status_voltage_scaled(&b));
    CHECK(a.flags.armed == b.flags.armed);
    CHECK(u(a.flags.mode) == b.flags.mode);
    CHECK(a.flags.level == b.flags.level);
    CHECK(a.count == b.count);
    REQUIRE(a.samples.size() == b.count);  // `count` is the length field of samples
    for (std::size_t i = 0; i < a.samples.size(); ++i) CHECK(a.samples[i] == b.samples[i]);
    REQUIRE(a.ext.has_value() == b.has_ext);
    if (a.ext) same_ext(*a.ext, b.ext);
    CHECK(a.label.view() == sv(b.label, b.label_len));
    CHECK(b.label[b.label_len] == '\0');
}

void same_status_view(const at::StatusView& a, const acme_telemetry_status_view& b) {
    CHECK(a.voltage() == acme_telemetry_status_view_voltage(&b));
    CHECK(a.voltage_scaled() == acme_telemetry_status_view_voltage_scaled(&b));
    const auto f = acme_telemetry_status_view_flags(&b);
    CHECK(a.flags().armed == f.armed);
    CHECK(u(a.flags().mode) == f.mode);
    CHECK(a.flags().level == f.level);
    CHECK(a.count() == acme_telemetry_status_view_count(&b));
    REQUIRE(a.samples().size() == acme_telemetry_status_view_samples_count(&b));
    for (std::size_t i = 0; i < a.samples().size(); ++i) CHECK(a.samples()[i] == acme_telemetry_status_view_samples_at(&b, i));
    REQUIRE(a.ext().has_value() == acme_telemetry_status_view_has_ext(&b));
    if (a.ext()) {
        const auto e = acme_telemetry_status_view_ext(&b);
        CHECK(a.ext()->temperature() == acme_telemetry_extended_view_temperature(&e));
        CHECK(a.ext()->pressure() == acme_telemetry_extended_view_pressure(&e));
    }
    CHECK(a.label() == sv(acme_telemetry_status_view_label(&b)));
}

at::Status make_status(at::Mode mode, std::size_t n_samples, const char* label) {
    at::Status s;
    s.voltage = 1234;
    s.flags.armed = true;
    s.flags.mode = mode;
    s.flags.level = -3;
    for (std::size_t i = 0; i < n_samples; ++i) (void)s.samples.push_back(static_cast<std::int16_t>(100 * i - 250));
    if (mode == at::Mode::fault) s.ext = at::Extended{-125, 101325};
    s.label = label;
    return s;
}

} // namespace

TEST_CASE("generated C matches generated C++: telemetry Status") {
    const std::vector<Bytes> valid = {
        enc(make_status(at::Mode::run, 3, "abc")), enc(make_status(at::Mode::fault, 0, "")),
        enc(make_status(at::Mode::fault, 16, "12345678")), enc(make_status(at::Mode::idle, 1, "a b")),
    };
    cross_check<at::Status, at::StatusView>(valid, CApi<acme_telemetry_status>{acme_telemetry_status_decode, acme_telemetry_status_encode, acme_telemetry_status_print},
                                            CViewApi<acme_telemetry_status_view>{acme_telemetry_status_view_from, acme_telemetry_status_view_print}, same_status,
                                            same_status_view, 1);
}

TEST_CASE("generated C matches generated C++: telemetry Command and Log") {
    at::Command c1;
    c1.opcode = 0xBEEF;
    for (std::uint8_t i = 0; i < 5; ++i) (void)c1.args.push_back(static_cast<std::uint8_t>(i * 7));
    at::Command c2;
    c2.opcode = 1;
    cross_check<at::Command, at::CommandView>(
        {enc(c1), enc(c2)}, CApi<acme_telemetry_command>{acme_telemetry_command_decode, acme_telemetry_command_encode, acme_telemetry_command_print},
        CViewApi<acme_telemetry_command_view>{acme_telemetry_command_view_from, acme_telemetry_command_view_print},
        [](const at::Command& a, const acme_telemetry_command& b) {
            CHECK(a.opcode == b.opcode);
            CHECK(a.arg_len == b.arg_len);
            REQUIRE(a.args.size() == b.arg_len);
            CHECK(std::memcmp(a.args.data(), b.args, b.arg_len) == 0);
        },
        [](const at::CommandView& a, const acme_telemetry_command_view& b) {
            CHECK(a.opcode() == acme_telemetry_command_view_opcode(&b));
            const auto x = acme_telemetry_command_view_args(&b);
            REQUIRE(a.args().size() == x.size);
            CHECK(std::memcmp(a.args().data(), x.data, x.size) == 0);
        },
        2);

    at::Log l1;
    l1.level = 4;
    l1.text = "hello";
    (void)l1.tail.push_back(0x01);
    (void)l1.tail.push_back(0x02);
    at::Log l2;
    cross_check<at::Log, at::LogView>(
        {enc(l1), enc(l2)}, CApi<acme_telemetry_log>{acme_telemetry_log_decode, acme_telemetry_log_encode, acme_telemetry_log_print},
        CViewApi<acme_telemetry_log_view>{acme_telemetry_log_view_from, acme_telemetry_log_view_print},
        [](const at::Log& a, const acme_telemetry_log& b) {
            CHECK(a.level == b.level);
            CHECK(a.text.view() == sv(b.text, b.text_len));
            REQUIRE(a.tail.size() == b.tail_len);
            CHECK(std::memcmp(a.tail.data(), b.tail, b.tail_len) == 0);
        },
        [](const at::LogView& a, const acme_telemetry_log_view& b) {
            CHECK(a.level() == acme_telemetry_log_view_level(&b));
            CHECK(a.text() == sv(acme_telemetry_log_view_text(&b)));
            const auto t = acme_telemetry_log_view_tail(&b);
            REQUIRE(a.tail().size() == t.size);
            CHECK(std::memcmp(a.tail().data(), t.data, t.size) == 0);
        },
        3);
}

TEST_CASE("generated C: scaled setters and constants match C++") {
    at::Status a;
    acme_telemetry_status b;
    std::memset(&b, 0, sizeof b);
    for (const double v : {0.0, 3.3, 12.345, 0.005, 655.35, 1.0 / 3.0}) {
        a.set_voltage_scaled(v);
        acme_telemetry_status_set_voltage_scaled(&b, v);
        CHECK(a.voltage == b.voltage);
    }
    CHECK(ACME_TELEMETRY_STATUS_MAGIC_VALUE == at::Status::magic_value);
    CHECK(ACME_TELEMETRY_STATUS_MIN_WIRE_SIZE == at::Status::min_wire_size);
    CHECK(ACME_TELEMETRY_STATUS_MAX_WIRE_SIZE == at::Status::max_wire_size);
    CHECK(ACME_TELEMETRY_EXTENDED_WIRE_SIZE == at::Extended::wire_size);
    CHECK(std::string_view(acme_telemetry_mode_to_string(ACME_TELEMETRY_MODE_FAULT)) == at::to_string(at::Mode::fault));
    CHECK(acme_telemetry_mode_to_string(9) == nullptr);
    CHECK(acme_telemetry_mode_is_known(ACME_TELEMETRY_MODE_RUN));
    CHECK_FALSE(acme_telemetry_mode_is_known(9));
}

// ---- computed values and their setters ----------------------------------------------------------------------
namespace tc = test::computed;

namespace {

void same_packed(const tc::Packed& a, const test_computed_packed& b) {
    CHECK(a.b0 == b.b0);
    CHECK(a.b1 == b.b1);
    CHECK(a.b2 == b.b2);
    CHECK(a.wide == b.wide);
    CHECK(a.lo == b.lo);
    CHECK(a.flags.ready == b.flags.ready);
    CHECK(a.flags.level == b.flags.level);
    CHECK(u(a.unit) == b.unit);
    REQUIRE(a.items.size() == b.n);
    for (std::size_t i = 0; i < a.items.size(); ++i) CHECK(a.items[i] == b.items[i]);
    CHECK(a.rgb() == test_computed_packed_rgb(&b));
    CHECK(a.big() == test_computed_packed_big(&b));
    CHECK(a.sum() == test_computed_packed_sum(&b));
    CHECK(a.level_x2() == test_computed_packed_level_x2(&b));
    CHECK(a.length_mm() == test_computed_packed_length_mm(&b));
    CHECK(a.ratio() == test_computed_packed_ratio(&b));
    CHECK(a.first_two() == test_computed_packed_first_two(&b));
    CHECK(a.overlapping() == test_computed_packed_overlapping(&b));
}

void same_packed_view(const tc::PackedView& a, const test_computed_packed_view& b) {
    CHECK(a.wide() == test_computed_packed_view_wide(&b));
    CHECK(u(a.unit()) == test_computed_packed_view_unit(&b));
    REQUIRE(a.items().size() == test_computed_packed_view_items_count(&b));
    for (std::size_t i = 0; i < a.items().size(); ++i) CHECK(a.items()[i] == test_computed_packed_view_items_at(&b, i));
    CHECK(a.rgb() == test_computed_packed_view_rgb(&b));
    CHECK(a.big() == test_computed_packed_view_big(&b));
    CHECK(a.sum() == test_computed_packed_view_sum(&b));
    CHECK(a.level_x2() == test_computed_packed_view_level_x2(&b));
    CHECK(a.length_mm() == test_computed_packed_view_length_mm(&b));
    CHECK(a.ratio() == test_computed_packed_view_ratio(&b));
}

} // namespace

TEST_CASE("generated C matches generated C++: computed values") {
    tc::Packed p;
    p.b0 = 0x12;
    p.b1 = 0x34;
    p.b2 = 0x56;
    p.wide = 0xA0B0C0D0;
    p.lo = 1001;
    p.flags.ready = true;
    p.flags.level = 99;
    p.unit = tc::Unit::cm;
    (void)p.items.push_back(7);
    (void)p.items.push_back(8);
    tc::Packed q;
    cross_check<tc::Packed, tc::PackedView>({enc(p), enc(q)}, CApi<test_computed_packed>{test_computed_packed_decode, test_computed_packed_encode, test_computed_packed_print},
                                            CViewApi<test_computed_packed_view>{test_computed_packed_view_from, test_computed_packed_view_print}, same_packed,
                                            same_packed_view, 4);
}

TEST_CASE("generated C: computed setters match C++") {
    tc::Packed a;
    test_computed_packed b;
    std::memset(&b, 0, sizeof b);
    a.set_rgb(0xABCDEF);
    test_computed_packed_set_rgb(&b, 0xABCDEF);
    a.set_big(0x123456789ABCull);
    test_computed_packed_set_big(&b, 0x123456789ABCull);
    a.set_length_mm(4321);
    test_computed_packed_set_length_mm(&b, 4321);
    a.set_first_two(0x7788);
    test_computed_packed_set_first_two(&b, 0x7788);
    same_packed(a, b);
}

// ---- strict enums everywhere ---------------------------------------------------------------------------------
namespace te = test::enums;

TEST_CASE("generated C matches generated C++: strict enums") {
    te::Sample s;
    s.color = te::Color::green;
    s.open = te::Open::b;
    s.flags.mode = te::Mode::run;
    s.levels = {te::Level::low, te::Level::high};
    s.inner.color = te::Color::blue;
    (void)s.more.push_back(te::Color::red);
    (void)s.more.push_back(te::Color::blue);
    cross_check<te::Sample, te::SampleView>(
        {enc(s)}, CApi<test_enums_sample>{test_enums_sample_decode, test_enums_sample_encode, test_enums_sample_print},
        CViewApi<test_enums_sample_view>{test_enums_sample_view_from, test_enums_sample_view_print},
        [](const te::Sample& a, const test_enums_sample& b) {
            CHECK(u(a.color) == b.color);
            CHECK(u(a.open) == b.open);
            CHECK(u(a.flags.mode) == b.flags.mode);
            for (std::size_t i = 0; i < 2; ++i) CHECK(u(a.levels[i]) == b.levels[i]);
            CHECK(u(a.inner.color) == b.inner.color);
            CHECK(a.n == b.n);
            REQUIRE(a.more.size() == b.n);
            for (std::size_t i = 0; i < a.more.size(); ++i) CHECK(u(a.more[i]) == b.more[i]);
        },
        [](const te::SampleView& a, const test_enums_sample_view& b) {
            CHECK(u(a.color()) == test_enums_sample_view_color(&b));
            CHECK(u(a.open()) == test_enums_sample_view_open(&b));
            REQUIRE(a.levels().size() == test_enums_sample_view_levels_count(&b));
            for (std::size_t i = 0; i < 2; ++i) CHECK(u(a.levels()[i]) == test_enums_sample_view_levels_at(&b, i));
            const auto in = test_enums_sample_view_inner(&b);
            CHECK(u(a.inner().color()) == test_enums_inner_view_color(&in));
            REQUIRE(a.more().size() == test_enums_sample_view_more_count(&b));
            for (std::size_t i = 0; i < a.more().size(); ++i) CHECK(u(a.more()[i]) == test_enums_sample_view_more_at(&b, i));
        },
        5);

    te::Tail t;
    for (auto c : {te::Color::red, te::Color::green, te::Color::blue}) (void)t.rest.push_back(c);
    cross_check<te::Tail, te::TailView>(
        {enc(t), enc(te::Tail{})}, CApi<test_enums_tail>{test_enums_tail_decode, test_enums_tail_encode, test_enums_tail_print},
        CViewApi<test_enums_tail_view>{test_enums_tail_view_from, test_enums_tail_view_print},
        [](const te::Tail& a, const test_enums_tail& b) {
            REQUIRE(a.rest.size() == b.rest_count);
            for (std::size_t i = 0; i < a.rest.size(); ++i) CHECK(u(a.rest[i]) == b.rest[i]);
        },
        [](const te::TailView& a, const test_enums_tail_view& b) {
            REQUIRE(a.rest().size() == test_enums_tail_view_rest_count(&b));
            for (std::size_t i = 0; i < a.rest().size(); ++i) CHECK(u(a.rest()[i]) == test_enums_tail_view_rest_at(&b, i));
        },
        6);
}

TEST_CASE("generated C: encoding an undeclared strict enum value fails like C++") {
    te::Inner a;
    a.color = static_cast<te::Color>(9);
    test_enums_inner b;
    b.color = 9;
    std::array<std::uint8_t, 8> buf{};
    const auto n = a.encode(buf);
    REQUIRE_FALSE(n);
    CHECK(static_cast<int>(test_enums_inner_encode(&b, buf.data(), buf.size(), nullptr)) == code(n.error()));
}

// ---- value limits everywhere (scaled, raw, 64-bit, floats, bits, arrays, nested, derived, conditional) ----------
namespace tl = test::limits;

namespace {
tl::Sample limits_sample(bool edge) {
    tl::Sample s;
    s.humidity = edge ? 3146 : 30000;  // scaled 0.0002 / 51.2
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
    if (edge) s.opt = 2;
    return s;
}
} // namespace

TEST_CASE("generated C matches generated C++: value limits") {
    cross_check<tl::Sample, tl::SampleView>(
        {enc(limits_sample(true)), enc(limits_sample(false))},
        CApi<test_limits_sample>{test_limits_sample_decode, test_limits_sample_encode, test_limits_sample_print},
        CViewApi<test_limits_sample_view>{test_limits_sample_view_from, test_limits_sample_view_print},
        [](const tl::Sample& a, const test_limits_sample& b) {
            CHECK(a.humidity == b.humidity);
            CHECK(a.temp == b.temp);
            CHECK(a.big == b.big);
            CHECK(a.neg == b.neg);
            CHECK(std::bit_cast<std::uint32_t>(a.ratio) == std::bit_cast<std::uint32_t>(b.ratio));
            CHECK(std::bit_cast<std::uint64_t>(a.wide) == std::bit_cast<std::uint64_t>(b.wide));
            CHECK(a.flags.level == b.flags.level);
            CHECK(a.flags.trim == b.flags.trim);
            CHECK(a.flags.gain == b.flags.gain);
            for (std::size_t i = 0; i < 2; ++i) CHECK(a.levels[i] == b.levels[i]);
            CHECK(a.inner.pct == b.inner.pct);
            REQUIRE(a.more.size() == b.n);
            for (std::size_t i = 0; i < a.more.size(); ++i) CHECK(a.more[i] == b.more[i]);
            REQUIRE(a.opt.has_value() == b.has_opt);
            if (a.opt) CHECK(*a.opt == b.opt);
        },
        [](const tl::SampleView& a, const test_limits_sample_view& b) {
            CHECK(a.humidity() == test_limits_sample_view_humidity(&b));
            CHECK(a.humidity_scaled() == test_limits_sample_view_humidity_scaled(&b));
            CHECK(a.temp() == test_limits_sample_view_temp(&b));
            CHECK(a.big() == test_limits_sample_view_big(&b));
            CHECK(a.neg() == test_limits_sample_view_neg(&b));
            CHECK(a.flags().gain == test_limits_sample_view_flags(&b).gain);
            for (std::size_t i = 0; i < 2; ++i) CHECK(a.levels()[i] == test_limits_sample_view_levels_at(&b, i));
            REQUIRE(a.more().size() == test_limits_sample_view_more_count(&b));
            for (std::size_t i = 0; i < a.more().size(); ++i) CHECK(a.more()[i] == test_limits_sample_view_more_at(&b, i));
            REQUIRE(a.opt().has_value() == test_limits_sample_view_has_opt(&b));
            if (a.opt()) CHECK(*a.opt() == test_limits_sample_view_opt(&b));
        },
        7);

    tl::Tail t;
    for (const std::uint16_t v : std::array<std::uint16_t, 3>{500, 0, 499}) (void)t.rest.push_back(v);
    cross_check<tl::Tail, tl::TailView>(
        {enc(t), enc(tl::Tail{})}, CApi<test_limits_tail>{test_limits_tail_decode, test_limits_tail_encode, test_limits_tail_print},
        CViewApi<test_limits_tail_view>{test_limits_tail_view_from, test_limits_tail_view_print},
        [](const tl::Tail& a, const test_limits_tail& b) {
            REQUIRE(a.rest.size() == b.rest_count);
            for (std::size_t i = 0; i < a.rest.size(); ++i) CHECK(a.rest[i] == b.rest[i]);
        },
        [](const tl::TailView& a, const test_limits_tail_view& b) {
            REQUIRE(a.rest().size() == test_limits_tail_view_rest_count(&b));
            for (std::size_t i = 0; i < a.rest().size(); ++i) CHECK(a.rest()[i] == test_limits_tail_view_rest_at(&b, i));
        },
        8);
}

TEST_CASE("generated C: encoding values outside the limits fails like C++") {
    const auto bytes = enc(limits_sample(true));
    const auto cpp0 = tl::Sample::decode(gmp::bytes_view(bytes.data(), bytes.size()));
    REQUIRE(cpp0);
    test_limits_sample c0;
    REQUIRE(test_limits_sample_decode(bytes.data(), bytes.size(), &c0) == GMSG_OK);
    // the same change on both structs, then both encoders must agree
    auto check = [&](auto change_cpp, auto change_c) {
        auto a = *cpp0;
        auto b = c0;
        change_cpp(a);
        change_c(b);
        std::array<std::uint8_t, 128> buf{};
        const auto n = a.encode(buf);
        std::size_t cn = 0;
        CHECK(static_cast<int>(test_limits_sample_encode(&b, buf.data(), buf.size(), &cn)) == (n ? 0 : code(n.error())));
        return n ? gmp::Error::None : n.error();
    };
    CHECK(check([](tl::Sample&) {}, [](test_limits_sample&) {}) == gmp::Error::None);
    CHECK(check([](tl::Sample& s) { s.humidity = 3145; }, [](test_limits_sample& s) { s.humidity = 3145; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.temp = -305 - 1; }, [](test_limits_sample& s) { s.temp = -306; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.big = 5; }, [](test_limits_sample& s) { s.big = 5; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.neg = 0; }, [](test_limits_sample& s) { s.neg = 0; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.ratio = NAN; }, [](test_limits_sample& s) { s.ratio = NAN; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.wide = INFINITY; }, [](test_limits_sample& s) { s.wide = INFINITY; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.flags.level = 6; }, [](test_limits_sample& s) { s.flags.level = 6; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.flags.trim = -11; }, [](test_limits_sample& s) { s.flags.trim = -11; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.flags.gain = 201; }, [](test_limits_sample& s) { s.flags.gain = 201; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.levels[1] = 1001; }, [](test_limits_sample& s) { s.levels[1] = 1001; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.inner.pct = 101; }, [](test_limits_sample& s) { s.inner.pct = 101; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.more[0] = -51; }, [](test_limits_sample& s) { s.more[0] = -51; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { s.opt = 1; }, [](test_limits_sample& s) { s.opt = 1; }) == gmp::Error::ValueOutOfLimits);
    CHECK(check([](tl::Sample& s) { (void)s.more.push_back(0); },
                [](test_limits_sample& s) { s.more[s.n++] = 0; }) == gmp::Error::ValueOutOfLimits);  // n = 4 > 3
}

// ---- field CRCs (preset and inline), conversions, conditional with CRC, const with CRC ------------------------
namespace tf = test::fieldcrc;

TEST_CASE("generated C matches generated C++: field CRCs and conversions") {
    tf::Mixed m1;
    m1.state = tf::State::error;
    (void)m1.items.push_back(0x1234);
    (void)m1.items.push_back(0xFFFF);
    m1.level = -77;
    m1.extra = 0xCAFE;
    tf::Mixed m2;
    m2.state = tf::State::on;
    m2.level = 300;
    cross_check<tf::Mixed, tf::MixedView>(
        {enc(m1), enc(m2)}, CApi<test_fieldcrc_mixed>{test_fieldcrc_mixed_decode, test_fieldcrc_mixed_encode, test_fieldcrc_mixed_print},
        CViewApi<test_fieldcrc_mixed_view>{test_fieldcrc_mixed_view_from, test_fieldcrc_mixed_view_print},
        [](const tf::Mixed& a, const test_fieldcrc_mixed& b) {
            CHECK(u(a.state) == b.state);
            CHECK(a.count == b.count);
            REQUIRE(a.items.size() == b.count);
            for (std::size_t i = 0; i < a.items.size(); ++i) CHECK(a.items[i] == b.items[i]);
            CHECK(a.level == b.level);
            CHECK(a.level_scaled() == test_fieldcrc_mixed_level_scaled(&b));
            CHECK(a.level_percent() == test_fieldcrc_mixed_level_percent(&b));
            CHECK(a.level_centi() == test_fieldcrc_mixed_level_centi(&b));
            REQUIRE(a.extra.has_value() == b.has_extra);
            if (a.extra) CHECK(*a.extra == b.extra);
        },
        [](const tf::MixedView& a, const test_fieldcrc_mixed_view& b) {
            CHECK(u(a.state()) == test_fieldcrc_mixed_view_state(&b));
            CHECK(a.level() == test_fieldcrc_mixed_view_level(&b));
            CHECK(a.level_percent() == test_fieldcrc_mixed_view_level_percent(&b));
            REQUIRE(a.items().size() == test_fieldcrc_mixed_view_items_count(&b));
            for (std::size_t i = 0; i < a.items().size(); ++i) CHECK(a.items()[i] == test_fieldcrc_mixed_view_items_at(&b, i));
            REQUIRE(a.extra().has_value() == test_fieldcrc_mixed_view_has_extra(&b));
            if (a.extra()) CHECK(*a.extra() == test_fieldcrc_mixed_view_extra(&b));
        },
        7);
}

TEST_CASE("generated C: a length field referenced by one container is that container's size") {
    // C++ keeps a separate container size and fills the length field from it on encode, so a stale length
    // member is ignored. In C the length field is the size: the same bytes come out.
    at::Command a;
    a.opcode = 7;
    a.arg_len = 99;  // stale: ignored by C++
    for (std::uint8_t i = 0; i < 3; ++i) (void)a.args.push_back(i);
    acme_telemetry_command b;
    std::memset(&b, 0, sizeof b);
    b.opcode = 7;
    b.arg_len = 3;
    for (std::uint8_t i = 0; i < 3; ++i) b.args[i] = i;
    std::array<std::uint8_t, 64> x{}, y{};
    const auto n = a.encode(x);
    REQUIRE(n);
    std::size_t m = 0;
    REQUIRE(acme_telemetry_command_encode(&b, y.data(), y.size(), &m) == GMSG_OK);
    REQUIRE(m == *n);
    CHECK(std::memcmp(x.data(), y.data(), m) == 0);
    CHECK(y[2] == 3);  // arg_len on the wire is the number of args

    // A size larger than the capacity is rejected instead of overrunning the buffer.
    b.arg_len = 33;
    CHECK(acme_telemetry_command_encode(&b, y.data(), y.size(), &m) == GMSG_ERR_OUT_OF_RANGE);
    CHECK(m == 0);
}

// ---- custom type (BCD time via the c: block), array of variable-size messages, conditional -------------------
namespace lh = link::hdlc;

TEST_CASE("generated C matches generated C++: custom type and nested variable-size messages (hdlc Batch)") {
    lh::Batch b1;
    b1.when = test::Time{12, 34, 56};
    for (int i = 0; i < 3; ++i) {
        lh::Reading r;
        r.channel = static_cast<std::uint8_t>(i);
        r.name = i == 1 ? "temp" : "x";
        r.value = 1.5f * static_cast<float>(i);
        (void)b1.readings.push_back(r);
    }
    b1.crc_in_payload = 0xDEADBEEF;  // n > 2
    lh::Batch b2;
    b2.when = test::Time{0, 0, 0};
    cross_check<lh::Batch, lh::BatchView>(
        {enc(b1), enc(b2)}, CApi<link_hdlc_batch>{link_hdlc_batch_decode, link_hdlc_batch_encode, link_hdlc_batch_print},
        CViewApi<link_hdlc_batch_view>{link_hdlc_batch_view_from, link_hdlc_batch_view_print},
        [](const lh::Batch& a, const link_hdlc_batch& b) {
            CHECK(a.when.h == b.when.h);
            CHECK(a.when.m == b.when.m);
            CHECK(a.when.s == b.when.s);
            REQUIRE(a.readings.size() == b.n);
            for (std::size_t i = 0; i < a.readings.size(); ++i) {
                CHECK(a.readings[i].channel == b.readings[i].channel);
                CHECK(a.readings[i].name.view() == sv(b.readings[i].name, b.readings[i].name_len));
                CHECK(a.readings[i].value == b.readings[i].value);
            }
            REQUIRE(a.crc_in_payload.has_value() == b.has_crc_in_payload);
            if (a.crc_in_payload) CHECK(*a.crc_in_payload == b.crc_in_payload);
        },
        [](const lh::BatchView& a, const link_hdlc_batch_view& b) {
            test_time t;
            const auto w = a.when();
            REQUIRE(static_cast<int>(link_hdlc_batch_view_when(&b, &t)) == (w ? 0 : code(w.error())));
            if (w) CHECK((w->h == t.h && w->m == t.m && w->s == t.s));
            std::size_t n = 0;
            for (const auto r : a.readings()) {
                REQUIRE(n < link_hdlc_batch_view_readings_count(&b));
                const auto c = link_hdlc_batch_view_readings_at(&b, n);
                CHECK(r.channel() == link_hdlc_reading_view_channel(&c));
                CHECK(r.name() == sv(link_hdlc_reading_view_name(&c)));
                CHECK(r.value() == link_hdlc_reading_view_value(&c));
                ++n;
            }
            CHECK(n == link_hdlc_batch_view_readings_count(&b));
        },
        8);
}

TEST_CASE("generated C: custom type encoder errors and wire size checks match C++") {
    lh::Batch a;
    a.when = test::Time{100, 0, 0};  // not representable in BCD
    link_hdlc_batch b;
    std::memset(&b, 0, sizeof b);
    b.when.h = 100;
    std::array<std::uint8_t, 64> buf{};
    const auto n = a.encode(buf);
    REQUIRE_FALSE(n);
    CHECK(static_cast<int>(link_hdlc_batch_encode(&b, buf.data(), buf.size(), nullptr)) == code(n.error()));
}
