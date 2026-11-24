#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "gmp/checksum.hpp"

using namespace gmsg;
using namespace gmsg::checksum;

namespace {
constexpr std::string_view kCheck = "123456789";
bytes_view check_data() { return as_bytes(kCheck.data(), kCheck.size()); }
} // namespace

// Check values from the CRC RevEng catalogue.
TEST_CASE("CRC presets match catalogue check values") {
    CHECK(crc8::compute(check_data()) == 0xF4);
    CHECK(crc8_maxim::compute(check_data()) == 0xA1);
    CHECK(crc8_autosar::compute(check_data()) == 0xDF);
    CHECK(crc8_nrsc5::compute(check_data()) == 0xF7);
    {
        const std::uint8_t beef[] = {0xBE, 0xEF};  // SHT4x datasheet, Table 7
        CHECK(crc8_nrsc5::compute(bytes_view(beef, 2)) == 0x92);
    }
    CHECK(crc15_can::compute(check_data()) == 0x059E);
    CHECK(crc16_ccitt::compute(check_data()) == 0x29B1);
    CHECK(crc16_xmodem::compute(check_data()) == 0x31C3);
    CHECK(crc16_kermit::compute(check_data()) == 0x2189);
    CHECK(crc16_x25::compute(check_data()) == 0x906E);
    CHECK(crc16_modbus::compute(check_data()) == 0x4B37);
    CHECK(crc16_arc::compute(check_data()) == 0xBB3D);
    CHECK(crc24_openpgp::compute(check_data()) == 0x21CF02);
    CHECK(crc32::compute(check_data()) == 0xCBF43926u);
    CHECK(crc32c::compute(check_data()) == 0xE3069283u);
    CHECK(crc32_mpeg2::compute(check_data()) == 0x0376E6E7u);
    CHECK(crc64_ecma::compute(check_data()) == 0x6C40DF5F0B497347ull);
}

TEST_CASE("CRC incremental equals one-shot") {
    auto d = check_data();
    auto s = crc32::init();
    s = crc32::update(s, d.first(4));
    s = crc32::update(s, d.subspan(4));
    CHECK(crc32::finish(s) == crc32::compute(d));
}

TEST_CASE("CRC is constexpr") {
    constexpr std::uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    static_assert(crc16_modbus::compute(bytes_view(data)) == 0x4B37);
}

TEST_CASE("simple checksums") {
    CHECK(xor8::compute(check_data()) == 0x31);
    CHECK(sum8::compute(check_data()) == 0xDD);
    CHECK(sum16::compute(check_data()) == 0x01DD);
    CHECK(lrc8::compute(check_data()) == 0x23);
    constexpr std::string_view abcde = "abcde";
    CHECK(fletcher16::compute(as_bytes(abcde.data(), abcde.size())) == 0xC8F0);
}
