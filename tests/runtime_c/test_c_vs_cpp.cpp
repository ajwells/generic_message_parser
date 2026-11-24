// Cross-check: the C runtime (compiled as C11 in crosscheck_c.c) must give the same results as the C++ runtime
// for checksums, endian loads/stores, bit fields and error texts, on fixed and pseudo-random inputs.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <random>
#include <string_view>
#include <tuple>
#include <vector>

#include "crosscheck_c.h"
#include "gmp/bits.hpp"
#include "gmp/checksum.hpp"
#include "gmp/endian.hpp"
#include "gmp/error.hpp"

namespace cs = gmsg::checksum;

namespace {

// Same order as gmpct_checksum().
using Algos = std::tuple<cs::crc8, cs::crc8_maxim, cs::crc8_autosar, cs::crc8_nrsc5, cs::crc15_can, cs::crc16_ccitt,
                         cs::crc16_xmodem, cs::crc16_kermit, cs::crc16_x25, cs::crc16_modbus, cs::crc16_arc,
                         cs::crc24_openpgp, cs::crc32, cs::crc32c, cs::crc32_mpeg2, cs::crc64_ecma, cs::xor8, cs::sum8,
                         cs::lrc8, cs::sum16, cs::fletcher16, cs::fletcher8_ubx>;

template <std::size_t... I>
std::uint64_t cpp_checksum_impl(int algo, gmsg::bytes_view d, std::index_sequence<I...>) {
    std::uint64_t out = 0;
    ((algo == static_cast<int>(I) ? (out = static_cast<std::uint64_t>(std::tuple_element_t<I, Algos>::compute(d)), 0) : 0), ...);
    return out;
}

std::uint64_t cpp_checksum(int algo, gmsg::bytes_view d) {
    return cpp_checksum_impl(algo, d, std::make_index_sequence<std::tuple_size_v<Algos>>{});
}

std::vector<std::uint8_t> random_bytes(std::mt19937& rng, std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (auto& b : v) b = static_cast<std::uint8_t>(rng());
    return v;
}

template <class T>
std::uint64_t cpp_load_bits(const std::uint8_t* p, gmsg::Endian e) {
    const T v = gmsg::load<T>(p, e);
    if constexpr (std::is_same_v<T, float>) return std::bit_cast<std::uint32_t>(v);
    else if constexpr (std::is_same_v<T, double>) return std::bit_cast<std::uint64_t>(v);
    else if constexpr (std::is_signed_v<T>) return static_cast<std::uint64_t>(static_cast<std::int64_t>(v));
    else return static_cast<std::uint64_t>(v);
}

std::uint64_t cpp_load(int type, const std::uint8_t* p, gmsg::Endian e) {
    switch (type) {
        case GMPCT_U8: return cpp_load_bits<std::uint8_t>(p, e);
        case GMPCT_U16: return cpp_load_bits<std::uint16_t>(p, e);
        case GMPCT_U32: return cpp_load_bits<std::uint32_t>(p, e);
        case GMPCT_U64: return cpp_load_bits<std::uint64_t>(p, e);
        case GMPCT_I8: return cpp_load_bits<std::int8_t>(p, e);
        case GMPCT_I16: return cpp_load_bits<std::int16_t>(p, e);
        case GMPCT_I32: return cpp_load_bits<std::int32_t>(p, e);
        case GMPCT_I64: return cpp_load_bits<std::int64_t>(p, e);
        case GMPCT_F32: return cpp_load_bits<float>(p, e);
        case GMPCT_F64: return cpp_load_bits<double>(p, e);
    }
    return 0;
}

constexpr std::size_t type_size(int type) {
    constexpr std::array<std::size_t, 10> sizes{1, 2, 4, 8, 1, 2, 4, 8, 4, 8};
    return sizes[static_cast<std::size_t>(type)];
}

} // namespace

TEST_CASE("C runtime checksums match the C++ runtime") {
    REQUIRE(gmpct_checksum_count() == static_cast<int>(std::tuple_size_v<Algos>));
    std::mt19937 rng(12345);
    for (int round = 0; round < 300; ++round) {
        const auto data = random_bytes(rng, static_cast<std::size_t>(round % 70));
        const gmsg::bytes_view view(data.data(), data.size());
        for (int a = 0; a < gmpct_checksum_count(); ++a) {
            INFO("algo " << a << ", length " << data.size());
            const auto expected = cpp_checksum(a, view);
            CHECK(gmpct_checksum(a, data.data(), data.size()) == expected);
            CHECK(gmpct_checksum_split(a, data.data(), data.size(), data.size() / 3) == expected);
        }
    }
}

TEST_CASE("C runtime generic CRC matches C++ for non-preset parameters") {
    std::mt19937 rng(777);
    for (int round = 0; round < 100; ++round) {
        const auto d = random_bytes(rng, static_cast<std::size_t>(round));
        const gmsg::bytes_view v(d.data(), d.size());
        INFO("length " << d.size());
        // CRC-3/GSM, CRC-5/USB, CRC-7/MMC, CRC-12/UMTS (refin != refout), CRC-64/XZ
        CHECK(gmpct_crc_params(3, false, false, 0x3, 0x0, 0x7, d.data(), d.size()) ==
              cs::Crc<std::uint8_t, 3, 0x3, 0x0, false, false, 0x7>::compute(v));
        CHECK(gmpct_crc_params(5, true, true, 0x05, 0x1F, 0x1F, d.data(), d.size()) ==
              cs::Crc<std::uint8_t, 5, 0x05, 0x1F, true, true, 0x1F>::compute(v));
        CHECK(gmpct_crc_params(7, false, false, 0x09, 0x0, 0x0, d.data(), d.size()) ==
              cs::Crc<std::uint8_t, 7, 0x09, 0x0, false, false, 0x0>::compute(v));
        CHECK(gmpct_crc_params(12, false, true, 0x80F, 0x0, 0x0, d.data(), d.size()) ==
              cs::Crc<std::uint16_t, 12, 0x80F, 0x0, false, true, 0x0>::compute(v));
        CHECK(gmpct_crc_params(64, true, true, 0x42F0E1EBA9EA3693ull, ~0ull, ~0ull, d.data(), d.size()) ==
              cs::Crc<std::uint64_t, 64, 0x42F0E1EBA9EA3693ull, ~0ull, true, true, ~0ull>::compute(v));
    }
}

TEST_CASE("C runtime endian loads and stores match the C++ runtime") {
    std::mt19937 rng(99);
    for (int round = 0; round < 500; ++round) {
        const auto bytes = random_bytes(rng, 8);
        for (int type = GMPCT_U8; type <= GMPCT_F64; ++type) {
            for (const bool big : {true, false}) {
                INFO("type " << type << (big ? " big" : " little"));
                const auto e = big ? gmsg::Endian::big : gmsg::Endian::little;
                const auto bits = gmpct_load(type, bytes.data(), big ? 1 : 0);
                CHECK(bits == cpp_load(type, bytes.data(), e));
                // Storing the loaded value back reproduces the bytes (floats included: NaN payloads are kept).
                std::array<std::uint8_t, 8> out{};
                gmpct_store(type, out.data(), bits, big ? 1 : 0);
                CHECK(std::equal(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(type_size(type)), bytes.begin()));
            }
        }
    }
}

TEST_CASE("C runtime bit fields match the C++ runtime") {
    std::mt19937 rng(4242);
    for (int round = 0; round < 2000; ++round) {
        const auto bytes = random_bytes(rng, 10);
        const unsigned n = 1 + static_cast<unsigned>(rng() % 64);
        const std::size_t off = rng() % (80 - n + 1);
        for (const bool msb : {true, false}) {
            INFO("offset " << off << ", bits " << n << (msb ? " msb" : " lsb"));
            const auto order = msb ? gmsg::BitOrder::msb_first : gmsg::BitOrder::lsb_first;
            CHECK(gmpct_get_bits(bytes.data(), off, n, msb ? 1 : 0) == gmsg::extract_bits(bytes.data(), off, n, order));
            CHECK(gmpct_get_bits_signed(bytes.data(), off, n, msb ? 1 : 0) ==
                  gmsg::sign_extend(gmsg::extract_bits(bytes.data(), off, n, order), n));

            const std::uint64_t value = (static_cast<std::uint64_t>(rng()) << 32) | rng();
            auto c_buf = bytes, cpp_buf = bytes;
            gmpct_set_bits(c_buf.data(), off, n, value, msb ? 1 : 0);
            gmsg::set_bits<std::uint64_t>(cpp_buf.data(), off, n, value, order);
            CHECK(c_buf == cpp_buf);
        }
    }
}

TEST_CASE("C runtime error codes match the C++ runtime") {
    REQUIRE(gmpct_error_count() == static_cast<int>(gmsg::Error::Timeout) + 1);
    for (int e = 0; e < gmpct_error_count(); ++e) {
        INFO("error " << e);
        CHECK(std::string_view(gmpct_error_str(e)) == gmsg::to_string(static_cast<gmsg::Error>(e)));
    }
}
