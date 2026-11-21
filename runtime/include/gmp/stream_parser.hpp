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
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "gmp/framing/frame.hpp"
#include "gmp/polled.hpp"
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
    /// Releases the frame returned by the previous poll (its view expires).
    std::size_t push(bytes_view in) noexcept {
        release();
        if (head_ > 0 && (Capacity - tail_) < in.size()) compact();
        const std::size_t n = std::min(in.size(), Capacity - tail_);
        std::copy_n(in.begin(), n, buf_.begin() + static_cast<std::ptrdiff_t>(tail_));
        tail_ += n;
        return n;
    }

    /// Deliver the next frame (or dropped data) to `handler`. Returns false when more input is required.
    template <class H>
    bool next(H&& handler) {
        const Step s = step([&](mut_bytes frame) { return Protocol::process(frame, handler); });
        if (s.kind == StepKind::error) invoke_if(handler, FrameError{s.error, s.bytes});
        release();
        return s.kind != StepKind::need_more;
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

    // ---------------------------------------------------------------------------------------------
    // pull API: the caller asks for the next message instead of being called back.
    // A returned view points into the parser buffer and stays valid until the next push / poll / feed /
    // next / reset (checked in Debug builds, see gmp/polled.hpp). Bad frames and garbage are skipped.
    // ---------------------------------------------------------------------------------------------

    /// Next valid message, or empty when more input is needed. Errors are counted in stats(), kept in
    /// last_error() and passed to the stored handler's FrameError overload (if any); message listeners are
    /// never called.
    template <class P = Protocol>
    Polled<typename P::AnyView> poll() {
        std::optional<typename P::AnyView> out;
        if (!pull([&](mut_bytes frame) -> Error {
                auto f = P::open(frame);
                if (!f) return f.error();
                auto v = P::select_message(*f);
                if (!v) return v.error();
                out.emplace(std::move(*v));
                return Error::None;
            }))
            return {};
        return polled(std::move(*out));
    }

    /// Like poll(), with the header view (protocols with a header) and the frame bytes: P::Received.
    template <class P = Protocol>
    Polled<typename P::Received> poll_frame() {
        std::optional<typename P::Received> out;
        if (!pull([&](mut_bytes frame) -> Error {
                auto f = P::open(frame);
                if (!f) return f.error();
                auto v = P::select_message(*f);
                if (!v) return v.error();
                if constexpr (requires { f->header; })
                    out.emplace(typename P::Received{f->header, std::move(*v), bytes_view(frame)});
                else
                    out.emplace(typename P::Received{std::move(*v), bytes_view(frame)});
                return Error::None;
            }))
            return {};
        return polled(std::move(*out));
    }

    /// Like poll(), decoded into the message struct (a copy that stays valid). A frame whose struct decode
    /// fails (e.g. more elements than `max`) is skipped and reported like any other bad frame.
    template <class P = Protocol>
    std::optional<typename P::AnyMessage> poll_message() {
        std::optional<typename P::AnyMessage> out;
        if (!pull([&](mut_bytes frame) -> Error {
                auto f = P::open(frame);
                if (!f) return f.error();
                auto v = P::select_message(*f);
                if (!v) return v.error();
                return std::visit(
                    [&](const auto& view) -> Error {
                        auto m = view.to_struct();
                        if (!m) return m.error();
                        out.emplace(std::move(*m));
                        return Error::None;
                    },
                    *v);
            }))
            return std::nullopt;
        return out;
    }

    /// The next message or error, in stream order; empty when more input is needed. Errors are returned
    /// here (also counted and kept in last_error()) and not passed to any handler.
    template <class P = Protocol>
    Polled<std::variant<typename P::AnyView, FrameError>> poll_event() {
        std::optional<typename P::AnyView> out;
        const Step s = step([&](mut_bytes frame) -> Error {
            auto f = P::open(frame);
            if (!f) return f.error();
            auto v = P::select_message(*f);
            if (!v) return v.error();
            out.emplace(std::move(*v));
            return Error::None;
        });
        using Event = std::variant<typename P::AnyView, FrameError>;
        if (s.kind == StepKind::need_more) return {};
        if (s.kind == StepKind::error) return polled(Event(FrameError{s.error, s.bytes}));
        return polled(Event(std::move(*out)));
    }

    /// Wait (without blocking) for a message of type V, e.g. the reply to a request. Other messages that arrive
    /// meanwhile go to the stored handler (CallbackStream listeners / make_stream overloads), errors to its
    /// FrameError overload. Returns empty when more input is needed and V has not arrived yet.
    template <class V, class P = Protocol>
    Polled<V> poll_for() requires(!std::is_void_v<Handler>) {
        std::optional<V> out;
        while (pull([&](mut_bytes frame) -> Error {
            auto f = P::open(frame);
            if (!f) return f.error();
            auto v = P::select_message(*f);
            if (!v) return v.error();
            if (const V* hit = std::get_if<V>(&*v)) {
                out.emplace(*hit);
                return Error::None;
            }
            std::visit([&](const auto& x) { deliver(*f, x); }, *v);
            return Error::None;
        })) {
            if (out) return polled(std::move(*out));
            release();  // delivered to the handler: done with that frame
        }
        return {};
    }

    /// Error of the most recently dropped frame or bytes (Error::None if nothing was dropped since reset()).
    [[nodiscard]] Error last_error() const noexcept { return last_error_; }

    void reset() noexcept {
        head_ = tail_ = 0;
        stats_ = {};
        held_ = 0;
        holding_ = false;
        last_error_ = Error::None;
        ++generation_;
    }
    [[nodiscard]] const StreamStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return tail_ - head_; }

private:
    enum class StepKind : std::uint8_t { need_more, frame, error };
    struct Step {
        StepKind kind = StepKind::need_more;
        Error error = Error::None;
        bytes_view bytes{};  // the frame, or the dropped bytes (valid until release())
    };

    /// Scan to the next frame or reportable error. On a frame, `accept(frame)` validates it (and delivers or
    /// captures the message). The frame / dropped bytes stay buffered until release(), so views and
    /// FrameError::data remain valid until the parser is used again. Silent skips are consumed directly.
    template <class Accept>
    Step step(Accept&& accept) {
        release();
        for (;;) {
            if (head_ == tail_) {
                head_ = tail_ = 0;
                return {};
            }
            const bytes_view pending(buf_.data() + head_, tail_ - head_);
            const FrameScan s = Protocol::scan(pending);
            switch (s.kind) {
                case FrameScan::Kind::frame: {
                    const mut_bytes frame(buf_.data() + head_, s.size);
                    const Error e = accept(frame);
                    if (e == Error::None) {
                        ++stats_.frames;
                        hold(s.consume);
                        return {StepKind::frame, Error::None, bytes_view(frame)};
                    }
                    ++stats_.frame_errors;
                    stats_.bytes_skipped += static_cast<std::uint32_t>(s.consume_on_error);
                    hold(s.consume_on_error);
                    last_error_ = e;
                    return {StepKind::error, e, bytes_view(frame)};
                }
                case FrameScan::Kind::skip: {
                    const std::size_t n = std::max<std::size_t>(1, std::min(s.size, pending.size()));
                    stats_.bytes_skipped += static_cast<std::uint32_t>(n);
                    if (s.error == Error::None) {  // silent skip (e.g. an empty frame between delimiters)
                        consume(n);
                        continue;
                    }
                    hold(n);
                    last_error_ = s.error;
                    return {StepKind::error, s.error, pending.first(n)};
                }
                case FrameScan::Kind::need_more:
                default:
                    if (pending.size() >= Capacity) {
                        // Buffer full but no frame: drop one byte and resync.
                        ++stats_.overflows;
                        ++stats_.bytes_skipped;
                        hold(1);
                        last_error_ = Error::BufferFull;
                        return {StepKind::error, Error::BufferFull, pending.first(1)};
                    }
                    if (head_ > 0) compact();
                    return {};
            }
        }
    }

    /// Step until a frame is accepted (true) or more input is needed (false). Errors go to the stored
    /// handler's FrameError overload, if any.
    template <class Accept>
    bool pull(Accept&& accept) {
        for (;;) {
            const Step s = step(accept);
            if (s.kind == StepKind::frame) return true;
            if (s.kind == StepKind::need_more) return false;
            if constexpr (!std::is_void_v<Handler>) invoke_if(handler_, FrameError{s.error, s.bytes});
        }
    }

    /// Hand a message to the stored handler, with the header when it accepts one (as Protocol::process does).
    template <class F, class V>
    void deliver(const F& f, const V& v) {
        if constexpr (requires { f.header; }) {
            if constexpr (std::invocable<Storage&, decltype((f.header)), const V&>) {
                handler_(f.header, v);
                return;
            }
        }
        if constexpr (std::invocable<Storage&, const V&>) handler_(v);
    }

    template <class T>
    Polled<T> polled(T&& v) noexcept {
        return Polled<T>(std::forward<T>(v), &generation_);
    }

    void hold(std::size_t n) noexcept {
        held_ = n;
        holding_ = true;
    }
    /// Drop the frame kept by the last step; any view into it expires.
    void release() noexcept {
        if (!holding_) return;
        consume(held_);
        held_ = 0;
        holding_ = false;
        ++generation_;
    }
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
    std::size_t held_ = 0;          // bytes of the frame kept for the caller (released on the next call)
    bool holding_ = false;
    Error last_error_ = Error::None;
    std::uint32_t generation_ = 0;  // changes whenever held bytes are released (expires polled views)
    StreamStats stats_{};
    [[no_unique_address]] Storage handler_{};
};

} // namespace gmp
