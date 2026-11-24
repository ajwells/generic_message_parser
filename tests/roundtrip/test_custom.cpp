#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <string_view>
#include <vector>

#include "custom.gmp.hpp"

using Catch::Matchers::WithinAbs;

TEST_CASE("custom text codec (ddmm.mmmm coordinates)") {
    constexpr std::string_view line = "POS,4807.0380,\n";
    auto any = ext::TextPos::parse(gmsg::as_bytes(line.data(), line.size()));
    REQUIRE(any);
    const auto& p = std::get<ext::PosView>(*any);
    CHECK_THAT(*p.lat(), WithinAbs(48.1173, 1e-9));
    auto lon = p.lon();
    REQUIRE(lon);
    CHECK_FALSE(lon->has_value());  // empty optional token

    ext::Pos pos{48.5, 11.25};
    std::array<std::uint8_t, 64> out{};
    auto n = ext::TextPos::encode(pos, out);
    REQUIRE(n);
    CHECK(std::string_view(reinterpret_cast<const char*>(out.data()), *n) == "POS,4830.0000,1115.0000\n");
    auto back = ext::TextPos::decode(gmsg::bytes_view(out.data(), *n));
    REQUIRE(back);
    CHECK_THAT(*std::get<ext::Pos>(*back).lon, WithinAbs(11.25, 1e-9));
}

TEST_CASE("custom framer drives stream parsing") {
    // frames: [len][kind][data...]; a 0x00 byte is garbage
    const std::vector<std::uint8_t> stream{3, 1, 0xAA, 0xBB, 0, 0, 2, 7, 0x01, 1, 9};
    std::vector<std::pair<int, std::size_t>> got;
    int errors = 0;
    ext::Custom::Stream<> sp;
    sp.feed(stream, gmsg::overloaded{
                        [&](const ext::Custom::HeaderView& h, const ext::BlobView& b) {
                            CHECK(h.n() == b.wire_size());
                            got.emplace_back(b.kind(), b.data_().size());
                        },
                        [&](const gmsg::FrameError&) { ++errors; },
                    });
    REQUIRE(got.size() == 3);
    CHECK(got[0] == std::pair<int, std::size_t>{1, 2});
    CHECK(got[1] == std::pair<int, std::size_t>{7, 1});
    CHECK(got[2] == std::pair<int, std::size_t>{9, 0});
    CHECK(errors == 2);
}
