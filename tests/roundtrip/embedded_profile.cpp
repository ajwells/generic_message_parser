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
        stream.feed(gmsg::bytes_view(frame.data() + i, 1), [&](const StatusView& v) {
            if (v.ext() && v.ext()->pressure() == 2) ++delivered;
        });
    auto any = BinaryLink::decode(gmsg::bytes_view(frame.data(), *n));
    if (!any || !std::holds_alternative<Status>(*any)) return 2;
    if (std::get<Status>(*any).samples.size() != 4) return 3;
    // pretty printing into fixed buffers and a callback
    char text[256];
    auto printed = gmsg::print(s, text, {.style = gmsg::PrintStyle::single_line});
    if (!printed || std::string_view(text).find("mode: fault (0x02)") == std::string_view::npos) return 5;
    gmsg::static_string<64> small;
    if (gmsg::print(*any, small) || small.size() != 64) return 6;  // truncated -> BufferFull
    std::size_t chars = 0;
    if (!gmsg::print(s, [&](std::string_view t) { chars += t.size(); }) || chars == 0) return 7;
    // stored handlers: compile-time (make_stream) and runtime callbacks (CallbackStream)
    int stored = 0;
    auto bound = BinaryLink::make_stream([&](const StatusView&) { ++stored; });
    if (bound.feed(gmsg::bytes_view(frame.data(), *n)) != 1 || stored != 1) return 8;
    struct Counter {
        int frames = 0;
        void on_status(const BinaryLink::HeaderView&, const StatusView&) { ++frames; }
    } counter;
    BinaryLink::CallbackStream<> cbs;
    cbs.on<&Counter::on_status>(counter);
    if (cbs.feed(gmsg::bytes_view(frame.data(), *n)) != 1 || counter.frames != 1) return 9;
    // pull API
    BinaryLink::Stream<> pull;
    pull.push(gmsg::bytes_view(frame.data(), *n));
    auto polled = pull.poll_frame();
    if (!polled || !std::holds_alternative<StatusView>(polled->message)) return 10;
    pull.push(gmsg::bytes_view(frame.data(), *n));
    auto copy = pull.poll_message();
    if (!copy || std::get<Status>(*copy).samples.size() != 4) return 11;
    g_armed = false;
    if (delivered != 1) return 4;
    std::puts("embedded profile OK: no exceptions, no RTTI, no heap");
    return 0;
}
