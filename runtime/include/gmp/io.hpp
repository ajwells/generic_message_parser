#pragma once
// gmp/io.hpp - byte sources and sinks for gmsg::Endpoint (gmp/endpoint.hpp).
//
// A byte source has
//     std::size_t read(gmsg::mut_bytes out);   // copy up to out.size() bytes that have arrived; never block:
//                                              // return 0 when nothing is there
// and optionally
//     bool take_overrun();                     // true (once) when input was lost before the bytes the next
//                                              // read() returns; the endpoint reports it as Error::Overrun
// A byte sink has
//     std::size_t write(gmsg::bytes_view in);  // send what it can now, return how many bytes it took
// and optionally
//     std::size_t writable();                  // room for write() right now (all-or-nothing sends check it)
//
// A gmsg::ByteRing works as either without an adapter: as a source the endpoint drains it like
// StreamParser::feed(ring) (overruns included), as a sink it pushes frames for a TX interrupt to send.
//
// Adapters here: NoSource / NoSink (one-way endpoints), SpanSource / SpanSink (memory: tests, replaying a
// capture), FileSource / FileSink (FILE*), source_fn / sink_fn (any callable) and DmaRingSource (a circular DMA
// receive buffer). Vendor HAL adapters stay in application code: a small struct with read() / write() is enough.

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "gmp/result.hpp"

namespace gmsg {

template <class S>
concept ByteSource = requires(S& s, mut_bytes out) {
    { s.read(out) } -> std::convertible_to<std::size_t>;
};

template <class S>
concept ByteSink = requires(S& s, bytes_view in) {
    { s.write(in) } -> std::convertible_to<std::size_t>;
};

/// Source of an endpoint that only sends.
struct NoSource {
    static constexpr std::size_t read(mut_bytes) noexcept { return 0; }
};

/// Sink of an endpoint that only receives: every send fails with BufferFull.
struct NoSink {
    static constexpr std::size_t writable() noexcept { return 0; }
    static constexpr std::size_t write(bytes_view) noexcept { return 0; }
};

/// Reads from memory (a capture, a test vector); at most `chunk` bytes per read() (0 = no limit).
class SpanSource {
public:
    constexpr SpanSource() noexcept = default;
    constexpr explicit SpanSource(bytes_view data, std::size_t chunk = 0) noexcept : data_(data), chunk_(chunk) {}

    std::size_t read(mut_bytes out) noexcept {
        std::size_t n = std::min(out.size(), data_.size() - pos_);
        if (chunk_ != 0) n = std::min(n, chunk_);
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(pos_), n, out.begin());
        pos_ += n;
        return n;
    }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }
    /// Start again with new data (keeps the chunk size).
    void reset(bytes_view data) noexcept {
        data_ = data;
        pos_ = 0;
    }

private:
    bytes_view data_{};
    std::size_t pos_ = 0;
    std::size_t chunk_ = 0;
};

/// Writes into a caller buffer; written() is what was sent so far.
class SpanSink {
public:
    constexpr SpanSink() noexcept = default;
    constexpr explicit SpanSink(mut_bytes buffer) noexcept : buf_(buffer) {}

    [[nodiscard]] std::size_t writable() const noexcept { return buf_.size() - used_; }
    std::size_t write(bytes_view in) noexcept {
        const std::size_t n = std::min(in.size(), writable());
        std::copy_n(in.begin(), n, buf_.begin() + static_cast<std::ptrdiff_t>(used_));
        used_ += n;
        return n;
    }
    [[nodiscard]] bytes_view written() const noexcept { return bytes_view(buf_.data(), used_); }
    void clear() noexcept { used_ = 0; }

private:
    mut_bytes buf_{};
    std::size_t used_ = 0;
};

/// Reads a FILE* (e.g. a recorded capture). fread() blocks on pipes and terminals until out.size() bytes or EOF:
/// for live input use source_fn() with a non-blocking read.
class FileSource {
public:
    explicit FileSource(std::FILE* f) noexcept : f_(f) {}
    std::size_t read(mut_bytes out) noexcept { return out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f_); }

private:
    std::FILE* f_;
};

/// Writes to a FILE* and flushes after every frame.
class FileSink {
public:
    explicit FileSink(std::FILE* f) noexcept : f_(f) {}
    std::size_t write(bytes_view in) noexcept {
        const std::size_t n = in.empty() ? 0 : std::fwrite(in.data(), 1, in.size(), f_);
        std::fflush(f_);
        return n;
    }

private:
    std::FILE* f_;
};

/// A source calling fn(mut_bytes) -> bytes read.
template <class F>
class FnSource {
public:
    explicit FnSource(F f) : f_(std::move(f)) {}
    std::size_t read(mut_bytes out) { return static_cast<std::size_t>(f_(out)); }

private:
    F f_;
};

/// A sink calling fn(bytes_view) -> bytes taken.
template <class F>
class FnSink {
public:
    explicit FnSink(F f) : f_(std::move(f)) {}
    std::size_t write(bytes_view in) { return static_cast<std::size_t>(f_(in)); }

private:
    F f_;
};

/// `gmsg::source_fn([&](gmsg::mut_bytes out) { return uart_read(out.data(), out.size()); })`
template <class F>
FnSource<std::decay_t<F>> source_fn(F&& f) {
    return FnSource<std::decay_t<F>>(std::forward<F>(f));
}
/// `gmsg::sink_fn([&](gmsg::bytes_view in) { return uart_write(in.data(), in.size()); })`
template <class F>
FnSink<std::decay_t<F>> sink_fn(F&& f) {
    return FnSink<std::decay_t<F>>(std::forward<F>(f));
}

/// Source over a circular DMA receive buffer of N bytes that the hardware fills continuously. `pos()` returns the
/// index the DMA writes next (0..N-1), e.g. `N - __HAL_DMA_GET_COUNTER(huart.hdmarx)` on STM32. Start the DMA at
/// index 0 before the first read(), or call restart() once it runs.
///
/// Overruns (the DMA lapping unread bytes) can only be seen when the half-transfer and transfer-complete
/// interrupts call on_half() (construct with track_laps = true): read() then knows how many bytes were written
/// and reports a lap through take_overrun() (the endpoint turns it into Error::Overrun), and it checks after
/// copying that the copied bytes were not overwritten meanwhile. Without lap tracking the reader must keep up
/// (read at least once per N bytes); a lap is then invisible.
template <std::size_t N, class Pos>
class DmaRingSource {
    static_assert(N >= 2 && N % 2 == 0, "gmsg::DmaRingSource: N must be even (two halves)");

public:
    DmaRingSource(const volatile std::uint8_t* buffer, Pos pos, bool track_laps = false)
        : buf_(buffer), pos_(std::move(pos)), track_(track_laps) {}

    /// Call from the DMA half-transfer and transfer-complete interrupts (lap tracking).
    void on_half() noexcept { halves_.store(halves_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    /// Forget everything buffered and continue from the DMA's current position.
    void restart() noexcept {
        h_ = halves_.load(std::memory_order_acquire);
        const std::size_t p = index();
        k_ = (p + N - base(h_)) % N;
        p_ = p;
        r_ = p;
        unread_ = 0;
        lost_ = false;
    }

    std::size_t read(mut_bytes out) noexcept {
        if (lost_) return 0;  // take_overrun() first
        sample();
        if (unread_ > N) return overrun();
        const std::size_t n = std::min(out.size(), unread_);
        for (std::size_t i = 0; i < n; ++i) out[i] = buf_[(r_ + i) % N];
        if (track_) {
            sample();
            if (unread_ > N) return overrun();  // the DMA overwrote bytes while they were copied
        }
        r_ = (r_ + n) % N;
        unread_ -= n;
        return n;
    }

    /// True once after the DMA lapped unread input (lap tracking only).
    bool take_overrun() noexcept {
        const bool l = lost_;
        lost_ = false;
        return l;
    }
    /// Laps seen so far.
    [[nodiscard]] std::uint32_t overruns() const noexcept { return overruns_; }
    /// Bytes received and not read yet (as of the last read()).
    [[nodiscard]] std::size_t unread() const noexcept { return unread_; }

private:
    static constexpr std::size_t base(std::uint32_t halves) noexcept { return (halves & 1u) ? N / 2 : 0; }
    std::size_t index() { return static_cast<std::size_t>(pos_()) % N; }

    /// Add the bytes written since the last sample to unread_.
    void sample() {
        std::size_t delta;
        if (track_) {
            const std::uint32_t h = halves_.load(std::memory_order_acquire);
            const std::size_t k = (index() + N - base(h)) % N;  // bytes past the last half boundary
            const auto d = static_cast<std::int64_t>(static_cast<std::uint32_t>(h - h_)) *
                               static_cast<std::int64_t>(N / 2) +
                           static_cast<std::int64_t>(k) - static_cast<std::int64_t>(k_);
            delta = d < 0 ? 0 : static_cast<std::size_t>(std::min<std::int64_t>(d, 4 * static_cast<std::int64_t>(N)));
            h_ = h;
            k_ = k;
            p_ = (base(h) + k) % N;
        } else {
            const std::size_t p = index();
            delta = (p + N - p_) % N;
            p_ = p;
        }
        unread_ = std::min(unread_ + delta, 4 * N);
    }

    std::size_t overrun() noexcept {
        lost_ = true;
        ++overruns_;
        r_ = p_;  // continue at the write position: everything buffered is suspect
        unread_ = 0;
        return 0;
    }

    const volatile std::uint8_t* buf_;
    Pos pos_;
    bool track_;
    std::atomic<std::uint32_t> halves_{0};  // written by the DMA interrupts
    std::uint32_t h_ = 0;      // halves at the last sample
    std::size_t k_ = 0;        // bytes past the half boundary at the last sample
    std::size_t p_ = 0;        // write index at the last sample
    std::size_t r_ = 0;        // read index
    std::size_t unread_ = 0;
    bool lost_ = false;
    std::uint32_t overruns_ = 0;
};

/// DmaRingSource with the size taken from the buffer: `gmsg::dma_source(rx_buf, [] { return 256 - DMA->CNDTR; })`.
template <std::size_t N, class Pos>
DmaRingSource<N, std::decay_t<Pos>> dma_source(const volatile std::uint8_t (&buffer)[N], Pos&& pos, bool track_laps = false) {
    return DmaRingSource<N, std::decay_t<Pos>>(buffer, std::forward<Pos>(pos), track_laps);
}

} // namespace gmsg
