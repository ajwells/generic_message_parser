#pragma once
// gmp/byte_ring.hpp - lock-free single-producer / single-consumer byte ring for interrupt-driven input.
//
// The producer (typically a UART RX interrupt) calls push(); the consumer (the main loop) drains the ring into a
// stream parser with `parser.feed(ring)` (or `parser.push(ring)` for the pull API), so decoding and callbacks never
// run in the interrupt. One producer and one consumer at a time; neither side blocks or disables interrupts.
//
//     gmsg::ByteRing<256> rx;                       // N: a power of two
//     void uart_isr() { rx.push(UART->DR); }        // producer
//     for (;;) { stream.feed(rx); ... }             // consumer
//
// Indices run freely (they wrap) as std::atomic<Index> with acquire / release ordering. `Index` may be narrowed
// (std::uint8_t, std::uint16_t) for 8- and 16-bit targets where a 32-bit atomic is not a single access; N must be
// at most half the index range (128 for uint8_t, 32768 for uint16_t).
//
// Overflow: when the ring is full the newest byte is dropped and counted (dropped()), and an overrun is marked.
// From then on the producer drops every byte until the consumer has read everything that arrived before the gap,
// so a ring holds at most one gap, always at its end. The stream that drains the ring then delivers the frames
// complete before the gap, drops the frame in progress and reports it as FrameError{Error::Overrun, bytes}.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

#include "gmp/result.hpp"

namespace gmsg {

template <std::size_t N, class Index = std::uint32_t>
class ByteRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "gmsg::ByteRing: N must be a power of two (>= 2)");
    static_assert(std::is_unsigned_v<Index>, "gmsg::ByteRing: Index must be an unsigned integer type");
    static_assert(N <= (static_cast<std::size_t>(std::numeric_limits<Index>::max()) >> 1) + 1,
                  "gmsg::ByteRing: N must be at most half the range of Index");

public:
    using index_type = Index;
    static constexpr std::size_t capacity() noexcept { return N; }

    // ---- producer (interrupt) side ----------------------------------------------------------------------------

    /// Append one byte. Returns false (and drops it) when the ring is full or an overrun is waiting to be drained.
    bool push(std::uint8_t b) noexcept {
        if (overrun_.load(std::memory_order_acquire)) return drop(1);
        const Index w = w_.load(std::memory_order_relaxed);
        const Index r = r_.load(std::memory_order_acquire);
        if (static_cast<Index>(w - r) == N) return mark_overrun(1);
        buf_[w & (N - 1)] = b;
        w_.store(static_cast<Index>(w + 1), std::memory_order_release);
        note_fill(static_cast<Index>(w + 1 - r));
        return true;
    }

    /// Append as many bytes of `in` as fit; the rest are dropped (an overrun). Returns the number appended.
    std::size_t push(bytes_view in) noexcept {
        if (in.empty()) return 0;
        if (overrun_.load(std::memory_order_acquire)) {
            drop(in.size());
            return 0;
        }
        const Index w = w_.load(std::memory_order_relaxed);
        const Index r = r_.load(std::memory_order_acquire);
        const std::size_t room = N - static_cast<Index>(w - r);
        const std::size_t n = in.size() < room ? in.size() : room;
        for (std::size_t i = 0; i < n; ++i) buf_[static_cast<Index>(w + i) & (N - 1)] = in[i];
        if (n > 0) {
            w_.store(static_cast<Index>(w + n), std::memory_order_release);
            note_fill(static_cast<Index>(w + n - r));
        }
        if (n < in.size()) mark_overrun(in.size() - n);
        return n;
    }

    // ---- consumer side ----------------------------------------------------------------------------------------

    /// The oldest buffered bytes that are contiguous in storage (the whole content, or its part up to the end of
    /// the storage when it wraps). Valid until consume().
    [[nodiscard]] bytes_view readable() const noexcept {
        const Index w = w_.load(std::memory_order_acquire);
        const Index r = r_.load(std::memory_order_relaxed);
        const std::size_t n = static_cast<Index>(w - r);
        const std::size_t start = r & (N - 1);
        return bytes_view(buf_.data() + start, n < N - start ? n : N - start);
    }

    /// Release the first `n` bytes of readable() to the producer.
    void consume(std::size_t n) noexcept {
        const Index r = r_.load(std::memory_order_relaxed);
        r_.store(static_cast<Index>(r + n), std::memory_order_release);
    }

    /// Copy up to out.size() bytes out (raw use without a parser). Returns the number copied. An overrun is not
    /// cleared here: check overrun_pending() and call clear_overrun() once the ring is empty.
    std::size_t read(std::span<std::uint8_t> out) noexcept {
        std::size_t total = 0;
        while (total < out.size()) {
            const bytes_view part = readable();
            if (part.empty()) break;
            const std::size_t n = part.size() < out.size() - total ? part.size() : out.size() - total;
            for (std::size_t i = 0; i < n; ++i) out[total + i] = part[i];
            consume(n);
            total += n;
        }
        return total;
    }

    /// Bytes buffered (consumer side; the producer may add more at any time).
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<Index>(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_relaxed));
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// True when bytes were dropped after the buffered ones; the producer drops input until clear_overrun().
    [[nodiscard]] bool overrun_pending() const noexcept { return overrun_.load(std::memory_order_acquire); }
    /// Accept new input again. Call only when the ring is empty (every byte before the gap has been read).
    void clear_overrun() noexcept { overrun_.store(false, std::memory_order_release); }

    /// Bytes dropped because the ring was full (wraps).
    [[nodiscard]] std::uint32_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    /// Highest fill level seen by the producer: size the ring from it.
    [[nodiscard]] std::size_t high_water() const noexcept { return high_water_.load(std::memory_order_relaxed); }

private:
    bool drop(std::size_t n) noexcept {
        dropped_.store(dropped_.load(std::memory_order_relaxed) + static_cast<std::uint32_t>(n),
                       std::memory_order_relaxed);
        return false;
    }
    bool mark_overrun(std::size_t n) noexcept {
        drop(n);
        overrun_.store(true, std::memory_order_release);  // after the last w_ store: the consumer sees all bytes
        return false;
    }
    void note_fill(Index fill) noexcept {
        if (fill > high_water_.load(std::memory_order_relaxed)) high_water_.store(fill, std::memory_order_relaxed);
    }

    std::array<std::uint8_t, N> buf_{};
    std::atomic<Index> w_{0};                 // written by the producer
    std::atomic<Index> r_{0};                 // written by the consumer
    std::atomic<bool> overrun_{false};        // set by the producer, cleared by the consumer
    std::atomic<std::uint32_t> dropped_{0};   // producer
    std::atomic<Index> high_water_{0};        // producer
};

} // namespace gmsg
