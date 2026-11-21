// Built with -fno-exceptions -fno-rtti -Werror. Replaces the global allocator with one that aborts,
// proving that decode / encode / streaming never touch the heap.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "telemetry.gmp.hpp"

namespace {
bool g_armed = false;
[[noreturn]] void alloc_abort() {
    std::fputs("unexpected heap allocation\n", stderr);
    std::abort();
}
} // namespace

void* operator new(std::size_t n) {
    if (g_armed) alloc_abort();
    if (void* p = std::malloc(n ? n : 1)) return p;
    std::abort();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace acme::telemetry;

int main() {
    g_armed = true;
    Status s;
    s.voltage = 500;
    s.flags.mode = Mode::fault;
    s.ext = Extended{1, 2};
    s.samples = {1, 2, 3, 4};
    std::array<std::uint8_t, BinaryLink::max_frame> frame{};
    auto n = BinaryLink::encode(s, frame);
    if (!n) return 1;

    int delivered = 0;
    BinaryLink::Stream<> stream;
    for (std::size_t i = 0; i < *n; ++i)  // byte at a time
        stream.feed(gmp::bytes_view(frame.data() + i, 1), [&](const StatusView& v) {
            if (v.ext() && v.ext()->pressure() == 2) ++delivered;
        });
    auto any = BinaryLink::decode(gmp::bytes_view(frame.data(), *n));
    if (!any || !std::holds_alternative<Status>(*any)) return 2;
    if (std::get<Status>(*any).samples.size() != 4) return 3;
    g_armed = false;
    if (delivered != 1) return 4;
    std::puts("embedded profile OK: no exceptions, no RTTI, no heap");
    return 0;
}
