// Rough throughput numbers for decode / encode / streaming. Build in Release for meaningful results.
#include <chrono>
#include <cstdio>
#include <string_view>
#include <vector>

#include "nmea.gmp.hpp"
#include "telemetry.gmp.hpp"

namespace {

template <class F>
void run(const char* name, std::size_t bytes_per_iter, std::size_t iters, F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t sink = 0;
    for (std::size_t i = 0; i < iters; ++i) sink += f();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%-34s %8.1f ns/op %9.1f MB/s  (sink %zu)\n", name, s * 1e9 / static_cast<double>(iters),
                static_cast<double>(bytes_per_iter * iters) / s / 1e6, sink);
}

} // namespace

int main() {
    using namespace acme::telemetry;
    Status st;
    st.voltage = 1234;
    st.flags.mode = Mode::fault;
    st.ext = Extended{10, 20};
    st.samples = {1, 2, 3, 4, 5, 6, 7, 8};
    st.label = "bench";
    std::array<std::uint8_t, BinaryLink::max_frame> frame{};
    const std::size_t n = *BinaryLink::encode(st, frame);
    const gmp::bytes_view fv(frame.data(), n);
    constexpr std::size_t N = 2'000'000;

    run("binary: parse frame + view access", n, N, [&] {
        auto v = BinaryLink::parse(fv);
        return v ? static_cast<std::size_t>(std::get<StatusView>(*v).voltage()) : 0;
    });
    run("binary: parse frame + struct decode", n, N, [&] {
        auto v = BinaryLink::decode(fv);
        return v ? std::get<Status>(*v).samples.size() : 0;
    });
    run("binary: encode frame", n, N, [&] {
        std::array<std::uint8_t, BinaryLink::max_frame> out;
        return *BinaryLink::encode(st, out);
    });

    std::vector<std::uint8_t> stream;
    for (int i = 0; i < 1000; ++i) stream.insert(stream.end(), fv.begin(), fv.end());
    run("binary: stream 1000 frames", stream.size(), N / 1000, [&] {
        BinaryLink::Stream<> sp;
        std::size_t got = 0;
        sp.feed(stream, [&](const StatusView&) { ++got; });
        return got;
    });

    constexpr std::string_view gga = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    const auto gv = gmp::as_bytes(gga.data(), gga.size());
    run("nmea: parse GGA + 3 lazy fields", gga.size(), N, [&] {
        auto v = nav::nmea::Nmea::parse(gv);
        if (!v) return std::size_t{0};
        const auto& g = std::get<nav::nmea::GGAView>(*v);
        return static_cast<std::size_t>(*g.satellites() + *g.utc_time() + (*g.altitude()).value_or(0));
    });
    run("nmea: parse GGA + struct decode", gga.size(), N, [&] {
        auto v = nav::nmea::Nmea::decode(gv);
        return v ? static_cast<std::size_t>(std::get<nav::nmea::GGA>(*v).satellites) : 0;
    });
    return 0;
}
