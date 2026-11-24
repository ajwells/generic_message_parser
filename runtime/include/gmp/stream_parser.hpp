#pragma once
// gmp/stream_parser.hpp - incremental frame extraction from an arbitrary byte stream.
//
// `Protocol` (normally generated) must provide:
//     static gmsg::FrameScan scan(gmsg::bytes_view buffer);
//     template <class H> static gmsg::Error process(gmsg::mut_bytes frame, H& handler);
//
// The parser owns a fixed buffer of `Capacity` bytes (no heap). Data can be pushed in chunks of any
// size; complete frames are validated, matched to a message type and passed to the handler. Garbage and corrupt frames are
// skipped and reported through `handler(gmsg::FrameError{...})` when the handler accepts it.
//
// The handler is either passed on every call (`feed(bytes, handler)`, Handler = void) or stored in the parser
// (`Handler` = its type, or a reference to it): then `feed(bytes)` / `next()` take no handler. Generated protocols
// add `make_stream(handler)`, `StreamWith<H>` and `CallbackStream` (runtime callbacks, see gmp/callbacks.hpp).
//
// Recovery on an idle link: a frame whose (corrupted) length claims more bytes than will ever arrive holds back the
// frames buffered behind it. resync() gives up on such a frame; set_idle_timeout(t) + tick(now) do that
// automatically after `t` without new bytes (the parser has no clock: `now` is any monotonic unsigned time the
// application passes). flush() drops everything buffered. These only drop data (reported to the error handler);
// frames that become complete are then delivered by the usual next() / feed() / poll() calls.

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

namespace gmsg {

/// Delivered to stream handlers when bytes are dropped.
struct FrameError {
    Error error;
    bytes_view data;  // the offending bytes (valid only during the callback)
};

/// Build an overload set from lambdas: `gmsg::overloaded{[](AView){}, [](BView){}}`.
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

/// Data after a decoded message: bytes for binary views (extra_bytes()), tokens for text views (extra_tokens()),
/// the same for the alternative held by a std::variant of views (AnyView); 0 for other types.
template <class V>
constexpr std::size_t extra_of(const V& v) noexcept {
    if constexpr (requires { v.extra_bytes(); }) return v.extra_bytes();
    else if constexpr (requires { v.extra_tokens(); }) return v.extra_tokens();
    else return 0;
}
template <class... V>
constexpr std::size_t extra_of(const std::variant<V...>& v) noexcept {
    return std::visit([](const auto& x) { return extra_of(x); }, v);
}

struct StreamStats {
    std::uint32_t frames = 0;        // frames delivered successfully
    std::uint32_t frame_errors = 0;  // frames rejected (checksum, unknown message type, decode error)
    std::uint32_t bytes_skipped = 0; // bytes discarded while resynchronising
    std::uint32_t overflows = 0;     // times the buffer filled without a complete frame
    std::uint32_t resyncs = 0;       // incomplete frames given up by resync() (also from tick())
    std::uint32_t flushes = 0;       // flush() calls that dropped buffered bytes
    std::uint32_t frames_with_extra = 0;  // delivered frames with data after their message (ignored)
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
        received_ += static_cast<std::uint32_t>(n);
        return n;
    }

    // ---------------------------------------------------------------------------------------------
    // recovery: resync / flush / idle timeout. They only drop data; frames that then become complete are
    // delivered by the next next() / feed() / poll() call.
    // ---------------------------------------------------------------------------------------------

    /// Give up on the frame at the head of the buffer when it is incomplete (e.g. its length field claims more bytes
    /// than will arrive): report it as FrameError{Truncated, its buffered bytes}, drop its first byte and let the
    /// next scan resynchronise. Returns false (and does nothing) when the buffer is empty or its head frame is
    /// complete. Errors go to the stored handler (if any) or to `handler`.
    bool resync() { return resync_impl([&](const FrameError& e) { report(e); }); }
    template <class H>
    bool resync(H&& handler) { return resync_impl([&](const FrameError& e) { invoke_if(handler, e); }); }

    /// Drop every buffered byte, reported as one FrameError{Truncated, bytes}. Returns false when nothing was buffered.
    bool flush() { return flush_impl([&](const FrameError& e) { report(e); }); }
    template <class H>
    bool flush(H&& handler) { return flush_impl([&](const FrameError& e) { invoke_if(handler, e); }); }

    /// Call resync() from tick() once the link has been idle for `t` (in the unit of the times passed to tick());
    /// 0 turns it off (the default).
    void set_idle_timeout(std::uint32_t t) noexcept { idle_timeout_ = t; }
    [[nodiscard]] std::uint32_t idle_timeout() const noexcept { return idle_timeout_; }

    /// Call periodically with the current time (any monotonic unsigned clock; wraparound is fine). Bytes pushed
    /// since the previous tick count as activity. When an incomplete frame has waited `idle_timeout()` without new
    /// bytes, it is given up as by resync() (returns true); the next resync needs another idle period.
    bool tick(std::uint32_t now) { return tick_impl(now, [&] { return resync(); }); }
    template <class H>
    bool tick(std::uint32_t now, H&& handler) { return tick_impl(now, [&] { return resync(handler); }); }

    /// Deliver the next frame (or dropped data) to `handler`. Returns false when more input is required.
    template <class H>
    bool next(H&& handler) {
        const Step s = step([&](mut_bytes frame) {
            if constexpr (requires { Protocol::process(frame, handler, &last_extra_); })
                return Protocol::process(frame, handler, &last_extra_);
            else
                return Protocol::process(frame, handler);
        });
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
                return open_view<P>(frame, [&](const auto&, auto& v) {
                    out.emplace(std::move(v));
                    return Error::None;
                });
            }))
            return {};
        return polled(std::move(*out));
    }

    /// Like poll(), with the header view (protocols with a header) and the frame bytes: P::Received.
    template <class P = Protocol>
    Polled<typename P::Received> poll_frame() {
        std::optional<typename P::Received> out;
        if (!pull([&](mut_bytes frame) -> Error {
                return open_view<P>(frame, [&](const auto& f, auto& v) {
                    if constexpr (requires { f.header; })
                        out.emplace(typename P::Received{f.header, std::move(v), bytes_view(frame)});
                    else
                        out.emplace(typename P::Received{std::move(v), bytes_view(frame)});
                    if constexpr (requires { out->extra; }) out->extra = last_extra_;
                    return Error::None;
                });
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
                return open_view<P>(frame, [&](const auto&, auto& v) {
                    return std::visit(
                        [&](const auto& view) -> Error {
                            auto m = view.to_struct();
                            if (!m) return m.error();
                            out.emplace(std::move(*m));
                            return Error::None;
                        },
                        v);
                });
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
            return open_view<P>(frame, [&](const auto&, auto& v) {
                out.emplace(std::move(v));
                return Error::None;
            });
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
            return open_view<P>(frame, [&](const auto& f, auto& v) {
                if (const V* hit = std::get_if<V>(&v)) {
                    out.emplace(*hit);
                    return Error::None;
                }
                std::visit([&](const auto& x) { deliver(f, x); }, v);
                return Error::None;
            });
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
        ticked_ = false;
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

    /// Open a frame and select its message view, then run use(frame_parts, view) -> Error. When any step fails,
    /// a protocol with escaping gets the frame back as received (P::restore_frame), so resynchronisation scans
    /// the bytes as they arrived.
    template <class P, class Use>
    Error open_view(mut_bytes frame, Use&& use) {
        auto f = P::open(frame);
        if (!f) return f.error();
        auto v = P::select_message(*f);
        if (v) last_extra_ = extra_of(*v);
        const Error e = v ? use(*f, *v) : v.error();
        if constexpr (requires { P::restore_frame(frame, f->unescaped_size); })
            if (e != Error::None) P::restore_frame(frame, f->unescaped_size);
        return e;
    }

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
                    last_extra_ = 0;
                    const Error e = accept(frame);
                    if (e == Error::None) {
                        ++stats_.frames;
                        if (last_extra_ != 0) ++stats_.frames_with_extra;
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

    /// Report an error to the stored handler, if any.
    void report(const FrameError& e) {
        if constexpr (!std::is_void_v<Handler>) invoke_if(handler_, e);
        else (void)e;
    }

    template <class Report>
    bool resync_impl(Report&& report_error) {
        release();
        if (head_ == tail_) return false;
        const bytes_view pending(buf_.data() + head_, tail_ - head_);
        if (Protocol::scan(pending).kind != FrameScan::Kind::need_more) return false;  // nothing stuck at the head
        ++stats_.resyncs;
        ++stats_.bytes_skipped;
        last_error_ = Error::Truncated;
        report_error(FrameError{Error::Truncated, pending});
        consume(1);
        ++generation_;
        return true;
    }

    template <class Report>
    bool flush_impl(Report&& report_error) {
        release();
        if (head_ == tail_) return false;
        const bytes_view pending(buf_.data() + head_, tail_ - head_);
        ++stats_.flushes;
        stats_.bytes_skipped += static_cast<std::uint32_t>(pending.size());
        last_error_ = Error::Truncated;
        report_error(FrameError{Error::Truncated, pending});
        head_ = tail_ = 0;
        ++generation_;
        return true;
    }

    template <class Resync>
    bool tick_impl(std::uint32_t now, Resync&& resync_now) {
        if (!ticked_ || received_ != seen_) {  // first tick, or bytes arrived since the last one
            ticked_ = true;
            seen_ = received_;
            last_activity_ = now;
            return false;
        }
        if (idle_timeout_ == 0 || static_cast<std::uint32_t>(now - last_activity_) < idle_timeout_) return false;
        last_activity_ = now;  // a further resync needs another idle period
        return resync_now();
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
    std::size_t last_extra_ = 0;       // data after the message of the frame being accepted
    std::uint32_t received_ = 0;       // bytes pushed (wraps; only compared for change)
    std::uint32_t seen_ = 0;           // received_ at the last tick
    std::uint32_t last_activity_ = 0;  // time of the last tick that saw new bytes (or of the last idle resync)
    std::uint32_t idle_timeout_ = 0;   // 0 = off
    bool ticked_ = false;
    [[no_unique_address]] Storage handler_{};
};

} // namespace gmsg
