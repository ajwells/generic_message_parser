// The C runtime headers must also compile as C++ (mixed C/C++ projects include generated C headers from C++).

#include <catch2/catch_test_macros.hpp>

#include "gmp/c/runtime.h"

TEST_CASE("C runtime headers compile and work as C++") {
    const std::uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(gmsg_crc32_compute(check, sizeof check) == 0xCBF43926u);
    std::uint8_t buf[4] = {};
    gmsg_writer w = gmsg_writer_init(buf, sizeof buf);
    CHECK(gmsg_write_f32(&w, GMSG_ENDIAN_BIG, 1.5f) == GMSG_OK);
    gmsg_reader r = gmsg_reader_init(buf, sizeof buf);
    float f = 0;
    CHECK(gmsg_read_f32(&r, GMSG_ENDIAN_BIG, &f) == GMSG_OK);
    CHECK(f == 1.5f);
    CHECK(gmsg_get_bits(buf, 0, 4, GMSG_MSB_FIRST) == 0x3);
}
