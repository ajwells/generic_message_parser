#pragma once
// gmp/stream_parser.hpp - incremental frame extraction from an arbitrary byte stream.
//
// `Protocol` (normally generated) must provide:
//     static gmp::FrameScan scan(gmp::bytes_view buffer);
//     template <class H> static gmp::Error process(gmp::mut_bytes frame, H& handler);
//
// The parser owns a fixed buffer of `Capacity` bytes (no heap). Data can be pushed in chunks of any
// size; complete frames are validated, matched to a message type and passed to the handler. Garbage and corrupt frames are
// skipped and reported through `handler(gmp::FrameError{...})` when the handler accepts it.
//
// The handler is either passed on every call (`feed(bytes, handler)`, Handler = void) or stored in the parser
// (`Handler` = its type, or a reference to it): then `feed(bytes)` / `next()` take no handler. Generated protocols
// add `make_stream(handler)`, `StreamWith<H>` and `CallbackStream` (runtime callbacks, see gmp/callbacks.hpp).

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

#include "gmp/framing/frame.hpp"
#include "gmp/result.hpp"

namespace gmp {

/// Delivered to stream handlers when bytes are dropped.
struct FrameError {
    Error error;
    bytes_view data;  // the offending bytes (valid only during the callback)
};

/// Build an overload set from lambdas: `gmp::overloaded{[](AView){}, [](BView){}}`.
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

/// Invoke `h(args...)` only if that call is well formed; otherwise do nothing.
template <class H, class... A>
constexpr void invoke_if(H& h, A&&... args) {
    if constexpr (std::invocable<H&, A...>) std::invoke(h, std::forward<A>(args)...);
}

struct StreamStats {
    std::uint32_t frames = 0;        // frames delivered successfully
    std::uint32_t frame_errors = 0;  // frames rejected (checksum, unknown message type, decode error)
    std::uint32_t bytes_skipped = 0; // bytes discarded while resynchronising
    std::uint32_t overflows = 0;     // times the buffer filled without a complete frame
};

namespace detail {
struct NoHandler {};
} // namespace detail

/// Handler = void: pass the handler on every feed()/next() call. Otherwise the parser stores a Handler (by value,
/// or by reference when Handler is a reference type) and feed(bytes) / next() use it.
template <class Protocol, std::size_t Capacity, class Handler = void>
class StreamParser {
    static_assert(Capacity > 0);
    using Storage = std::conditional_t<std::is_void_v<Handler>, detail::NoHandler, Handler>;

public:
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    StreamParser() requires std::default_initializable<Storage> = default;
    /// Store `h` as the handler used by feed(bytes) and next().
    template <class H>
        requires(!std::is_void_v<Handler> && std::constructible_from<Storage, H &&>)
    explicit StreamParser(H&& h) : handler_(std::forward<H>(h)) {}

    /// The stored handler (only when Handler is not void).
    auto& handler() noexcept requires(!std::is_void_v<Handler>) { return handler_; }
    const auto& handler() const noexcept requires(!std::is_void_v<Handler>) { return handler_; }

    /// Push `in` and deliver every complete frame to the stored handler. Returns the number of frames delivered.
    std::size_t feed(bytes_view in) requires(!std::is_void_v<Handler>) { return feed(in, handler_); }
    /// Process at most one scan step with the stored handler. Returns false when more input is required.
    bool next() requires(!std::is_void_v<Handler>) { return next(handler_); }

    /// Copy as much of `in` as fits into the internal buffer. Returns bytes accepted.
    std::size_t push(bytes_view in) noexcept {
        if (head_ > 0 && (Capacity - tail_) < in.size()) compact();
        const std::size_t n = std::min(in.size(), Capacity - tail_);
        std::copy_n(in.begin(), n, buf_.begin() + static_cast<std::ptrdiff_t>(tail_));
        tail_ += n;
        return n;
    }

    /// Process at most one scan step. Returns false when more input is required.
    template <class H>
    bool next(H&& handler) {
        if (head_ == tail_) { head_ = tail_ = 0; return false; }
        const bytes_view pending(buf_.data() + head_, tail_ - head_);
        const FrameScan s = Protocol::scan(pending);
        switch (s.kind) {
            case FrameScan::Kind::frame: {
                const mut_bytes frame(buf_.data() + head_, s.size);
                const Error e = Protocol::process(frame, handler);
                if (e == Error::None) {
                    ++stats_.frames;
                    consume(s.consume);
                } else {
                    ++stats_.frame_errors;
                    invoke_if(handler, FrameError{e, bytes_view(buf_.data() + head_, s.size)});
                    stats_.bytes_skipped += static_cast<std::uint32_t>(s.consume_on_error);
                    consume(s.consume_on_error);
                }
                return true;
            }
            case FrameScan::Kind::skip: {
                const std::size_t n = std::max<std::size_t>(1, std::min(s.size, pending.size()));
                if (s.error != Error::None) invoke_if(handler, FrameError{s.error, pending.first(n)});
                stats_.bytes_skipped += static_cast<std::uint32_t>(n);
                consume(n);
                return true;
            }
            case FrameScan::Kind::need_more:
            default:
                if (pending.size() >= Capacity) {
                    // Buffer full but no frame: drop one byte and resync.
                    ++stats_.overflows;
                    invoke_if(handler, FrameError{Error::BufferFull, pending.first(1)});
                    ++stats_.bytes_skipped;
                    consume(1);
                    return true;
                }
                if (head_ > 0) compact();
                return false;
        }
    }

    /// Push `in` and process every complete frame. Returns the number of frames delivered.
    template <class H>
    std::size_t feed(bytes_view in, H&& handler) {
        const auto before = stats_.frames;
        do {
            const std::size_t n = push(in);
            in = in.subspan(n);
            while (next(handler)) {}
        } while (!in.empty());
        return stats_.frames - before;
    }

    void reset() noexcept { head_ = tail_ = 0; stats_ = {}; }
    [[nodiscard]] const StreamStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return tail_ - head_; }

private:
    void consume(std::size_t n) noexcept {
        head_ += std::min(n, tail_ - head_);
        if (head_ == tail_) head_ = tail_ = 0;
    }
    void compact() noexcept {
        if (head_ == 0) return;
        std::copy(buf_.begin() + static_cast<std::ptrdiff_t>(head_), buf_.begin() + static_cast<std::ptrdiff_t>(tail_),
                  buf_.begin());
        tail_ -= head_;
        head_ = 0;
    }

    std::array<std::uint8_t, Capacity> buf_{};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    StreamStats stats_{};
    [[no_unique_address]] Storage handler_{};
};

} // namespace gmp
