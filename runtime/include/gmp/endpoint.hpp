#pragma once
// gmp/endpoint.hpp - a protocol stream joined to a byte source, a byte sink and a clock.
//
//     auto ep = Link::make_endpoint(rx_ring, tx_ring, [] { return HAL_GetTick(); });   // or std::chrono::steady_clock{}
//     ep.on<&App::on_status>(app);              // listeners as on CallbackStream
//     ep.send(Command{...});                    // encode + write to the sink
//     for (;;) ep.service();                    // read what has arrived, deliver frames, idle resync
//     auto r = ep.transact<Ack>(Command{...}, 100);   // send, then wait up to 100 ticks for an Ack
//
// Endpoint<Protocol, Source, Sink, Clock, Capacity> is the protocol's CallbackStream (so on<>(), poll*(), stats()
// and the recovery calls are all there) plus:
//   - service(): read everything the source has (at most max_bytes), deliver the frames to the listeners, and
//     tick the idle resync (with a clock).
//   - send(msg) / send(header, msg) / send_raw(bytes): encode into a max_frame buffer and write it to the sink.
//     SendPolicy::whole_frame (default): all or nothing; a sink with writable() too small fails with BufferFull and
//     nothing is written (a ByteRing sink drained by a TX interrupt is then the transmit queue). A sink without
//     writable() is assumed to take whole frames (a blocking write); a short write fails with BufferFull.
//     SendPolicy::retry: keep writing the rest (calling the idle hook in between) until it is all out, or until the
//     send timeout passes (Timeout; part of the frame may have been sent).
//   - wait_for<M>(timeout[, match]): read and process input until a message M (a struct or view type) arrives for
//     which match(view) / match(header_view, view) is true, and return it decoded (a copy). Other messages go to
//     their listeners meanwhile, errors to the error listener; the awaited message is not passed to listeners.
//     Times out with Error::Timeout. wait_for_reply<M>() also returns the header (gmsg::Reply).
//   - transact<M>(request[, header], timeout[, match]): service() (frames that arrived before are delivered, so an
//     old reply is not taken for the answer), send(request), wait_for<M>().
//
// Clock: void (no timeouts: service() and send() only), a std::chrono clock type (durations in its unit, e.g.
// std::chrono::milliseconds(100)), or any callable returning a monotonic unsigned time (any unit, wraparound is
// fine; timeouts are std::uint32_t in that unit), e.g. a HAL tick counter. The idle hook (set_idle) runs whenever
// a wait or a retried send has nothing to do: __WFI(), std::this_thread::yield(), a short sleep.
//
// Single-threaded: only the source / sink may be fed from interrupts or other threads (e.g. ByteRings).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "gmp/byte_ring.hpp"
#include "gmp/callbacks.hpp"
#include "gmp/io.hpp"
#include "gmp/polled.hpp"
#include "gmp/result.hpp"
#include "gmp/stream_parser.hpp"

namespace gmsg {

enum class SendPolicy : std::uint8_t {
    whole_frame,  ///< all or nothing: BufferFull (nothing written) when the sink cannot take the frame now
    retry,        ///< write the rest until it is all out, or Timeout after the send timeout
};

/// A message with the header of its frame (wait_for_reply()).
template <class Header, class Message>
struct Reply {
    Header header;
    Message message;
};

namespace detail {

template <class T>
struct is_byte_ring : std::false_type {};
template <std::size_t N, class I>
struct is_byte_ring<ByteRing<N, I>> : std::true_type {};

template <class C>
concept ChronoClock = requires {
    typename C::time_point;
    typename C::duration;
    { C::now() } -> std::same_as<typename C::time_point>;
};

struct NoClock {};
struct NoHeader {};

template <class P>
struct header_of {
    using type = NoHeader;
};
template <class P>
    requires requires { typename P::Header; }
struct header_of<P> {
    using type = typename P::Header;
};

/// Accepts every message.
struct MatchAll {
    template <class... A>
    constexpr bool operator()(const A&...) const noexcept { return true; }
};

template <class V>
concept HasStruct = requires(const V& v) { v.to_struct(); };

template <class V, class M>
constexpr bool view_is_for() {
    if constexpr (std::is_same_v<V, M>) return true;
    else if constexpr (HasStruct<V>) return std::is_same_v<typename decltype(std::declval<const V&>().to_struct())::value_type, M>;
    else return false;
}

template <class M, class... V>
struct pick_view {
    using type = void;
};
template <class M, class V0, class... V>
struct pick_view<M, V0, V...> {
    using type = std::conditional_t<view_is_for<V0, M>(), V0, typename pick_view<M, V...>::type>;
};

template <class AnyView, class M>
struct view_for;
template <class M, class... V>
struct view_for<std::variant<V...>, M> {
    using type = typename pick_view<M, V...>::type;
    static_assert(!std::is_void_v<type>, "gmsg::Endpoint: M is not a message (struct or view) of this protocol");
};

} // namespace detail

/// State of a gmsg::Pending request.
enum class RequestState : std::uint8_t {
    idle,     ///< not started, or cancelled
    waiting,  ///< sent (or expected), waiting for the reply
    done,     ///< finished: result() holds the reply or the error (Timeout)
};

namespace detail {
/// The part of a Pending that the endpoint links into its list (type-erased by the endpoint's thunks).
struct PendingLink {
    PendingLink* next = nullptr;
    void* owner = nullptr;                                  // the endpoint while linked
    void (*unlink_fn)(void* owner, PendingLink*) = nullptr;
    // decide on a frame: sets *taken when it is the reply (a failed decode returns the error, the frame is dropped)
    Error (*offer)(PendingLink*, const void* frame_parts, const void* any_view, bool* taken) = nullptr;
    void (*finish)(PendingLink*) = nullptr;                // store the error (if any) and run the completion callback
    std::uint32_t start = 0;
    std::uint32_t timeout = 0;
    Error error = Error::None;
    RequestState state = RequestState::idle;
};
} // namespace detail

/// A request that does not block: start it with Endpoint::request(pending, message, timeout) (or expect()), keep
/// calling service(), and the reply (or Timeout) completes it: done() / result(), and the on_done callback.
/// Caller-owned (no heap, any number at once); it must outlive the wait, or cancel() (the destructor cancels).
/// `Match` (default: any) is called as match(view) or match(header_view, view); store state it needs by reference.
template <class M, class Match = detail::MatchAll>
class Pending : private detail::PendingLink {
public:
    Pending() = default;
    explicit Pending(Match match) : match_(std::move(match)) {}
    Pending(const Pending&) = delete;
    Pending& operator=(const Pending&) = delete;
    ~Pending() { cancel(); }

    [[nodiscard]] RequestState state() const noexcept { return PendingLink::state; }
    [[nodiscard]] bool waiting() const noexcept { return PendingLink::state == RequestState::waiting; }
    [[nodiscard]] bool done() const noexcept { return PendingLink::state == RequestState::done; }
    /// The reply, or the error (Timeout, or what send() returned is reported by request() itself). Only when done().
    [[nodiscard]] const Result<M>& result() const noexcept {
        GMSG_ASSERT(done() && "gmsg::Pending: result() before the request is done");
        return *result_;
    }
    /// Stop waiting; no callback runs. The request can be started again.
    void cancel() noexcept {
        if (owner && unlink_fn) unlink_fn(owner, this);
        owner = nullptr;
        if (PendingLink::state == RequestState::waiting) PendingLink::state = RequestState::idle;
    }
    [[nodiscard]] Match& match() noexcept { return match_; }

    /// Completion callback, called once from service() / a wait with the result (bound as on CallbackStream:
    /// a member function of C, a function taking C& / C*, or a plain function).
    template <auto Fn, class C>
    Pending& on_done(C& ctx) noexcept {
        done_.template bind<Fn>(ctx);
        return *this;
    }
    template <auto Fn>
    Pending& on_done() noexcept {
        done_.template bind<Fn>();
        return *this;
    }
    template <class F, class C>
        requires std::is_function_v<F>
    Pending& on_done(F* fn, C* ctx) noexcept {
        done_.bind(fn, ctx);
        return *this;
    }
    template <class F>
        requires std::is_function_v<F>
    Pending& on_done(F* fn) noexcept {
        done_.bind(fn);
        return *this;
    }

private:
    template <class, class, class, class, std::size_t>
    friend class Endpoint;

    Match match_{};
    std::optional<Result<M>> result_;
    CallbackSlot<const Result<M>&> done_;
};

template <class Protocol, class Source, class Sink, class Clock = void, std::size_t Capacity = Protocol::max_frame>
class Endpoint : public Protocol::template CallbackStream<Capacity> {
    using Parser = StreamParser<Protocol, Capacity, typename Protocol::Callbacks&>;
    using SourceT = std::remove_cvref_t<Source>;
    using SinkT = std::remove_cvref_t<Sink>;
    static constexpr bool has_clock = !std::is_void_v<Clock>;
    using ClockStore = std::conditional_t<has_clock, Clock, detail::NoClock>;
    static constexpr bool chrono_clock = has_clock && detail::ChronoClock<std::remove_cvref_t<ClockStore>>;
    using HeaderT = typename detail::header_of<Protocol>::type;
    static constexpr bool has_header = !std::is_same_v<HeaderT, detail::NoHeader>;

    template <class C, bool = chrono_clock>
    struct TimeTypes {
        using time_point = std::uint32_t;
        using duration = std::uint32_t;
    };
    template <class C>
    struct TimeTypes<C, true> {
        using time_point = typename std::remove_cvref_t<C>::time_point;
        using duration = typename std::remove_cvref_t<C>::duration;
    };

public:
    /// Time and timeout types: the chrono clock's, or std::uint32_t in the unit of a callable clock.
    using time_point = typename TimeTypes<ClockStore>::time_point;
    using duration = typename TimeTypes<ClockStore>::duration;
    /// The view type of message M (M may be the struct or the view) and its struct.
    template <class M>
    using view_t = typename detail::view_for<typename Protocol::AnyView, M>::type;
    template <class M>
    using message_t = typename decltype(std::declval<const view_t<M>&>().to_struct())::value_type;
    using Header = HeaderT;

    template <class S, class K>
        requires(!has_clock || (chrono_clock && std::default_initializable<ClockStore>))
    Endpoint(S&& source, K&& sink) : source_(std::forward<S>(source)), sink_(std::forward<K>(sink)) {}
    template <class S, class K, class C>
        requires has_clock
    Endpoint(S&& source, K&& sink, C&& clock)
        : source_(std::forward<S>(source)), sink_(std::forward<K>(sink)), clock_(std::forward<C>(clock)) {}

    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;
    ~Endpoint() {
        for (auto* l = head_; l;) {  // requests still waiting stop waiting (idle); they no longer refer to us
            auto* n = l->next;
            l->owner = nullptr;
            l->next = nullptr;
            l->state = RequestState::idle;
            l = n;
        }
    }

    [[nodiscard]] auto& source() noexcept { return source_; }
    [[nodiscard]] auto& sink() noexcept { return sink_; }
    /// The underlying CallbackStream / StreamParser.
    [[nodiscard]] auto& stream() noexcept { return static_cast<typename Protocol::template CallbackStream<Capacity>&>(*this); }

    // ---- time ---------------------------------------------------------------------------------------------

    /// Current time of the clock.
    [[nodiscard]] time_point now() requires has_clock {
        if constexpr (chrono_clock) return std::remove_cvref_t<ClockStore>::now();
        else return static_cast<std::uint32_t>(clock_());
    }

    /// Give up an incomplete frame after `t` without new bytes (checked by service() and the waits); 0 = off.
    void set_idle_timeout(duration t) noexcept requires has_clock { Parser::set_idle_timeout(to_ticks(t)); }

    /// Run fn(ctx) / fn() whenever a wait or a retried send has nothing to do.
    void set_idle(void (*fn)(void*), void* ctx) noexcept {
        idle_ctx_fn_ = fn;
        idle_fn_ = nullptr;
        idle_ctx_ = ctx;
    }
    void set_idle(void (*fn)()) noexcept {
        idle_fn_ = fn;
        idle_ctx_fn_ = nullptr;
        idle_ctx_ = nullptr;
    }

    // ---- receive ------------------------------------------------------------------------------------------

    /// Read what the source has (at most `max_bytes`), deliver every complete frame to the listeners and run
    /// the idle resync. Returns the number of frames delivered.
    std::size_t service(std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
        const Busy busy(depth_);
        const auto before = this->stats().frames;
        while (next_frame()) {}
        std::size_t total = 0;
        while (total < max_bytes) {
            const std::size_t n = read_input(max_bytes - total);
            total += n;
            while (next_frame()) {}  // also reports an overrun the read took over
            if (n == 0) break;
        }
        if constexpr (has_clock) {
            Parser::tick(ticks());
            expire_requests();
        }
        return this->stats().frames - before;
    }

    // ---- requests that do not block ----------------------------------------------------------------------

    /// Deliver what has arrived (not when called from a completion callback), send `request` and return at once; the reply (a message M accepted by the
    /// Pending's match) or Timeout after `timeout` completes `p` from a later service() / wait. Returns the send
    /// error (then `p` stays idle). A request already waiting is restarted.
    template <class M, class Match, class Req>
        requires has_clock
    Error request(Pending<M, Match>& p, const Req& request, duration timeout) {
        p.cancel();
        if (depth_ == 0) service();  // not from a callback: that would take frames from the running step
        if (auto s = send(request); !s) return s.error();
        arm(p, timeout);
        return Error::None;
    }
    template <class M, class Match, class Req>
        requires(has_clock && has_header)
    Error request(Pending<M, Match>& p, const Header& header, const Req& request, duration timeout) {
        p.cancel();
        if (depth_ == 0) service();
        if (auto s = send(header, request); !s) return s.error();
        arm(p, timeout);
        return Error::None;
    }
    /// Wait for a message M without sending anything (e.g. an unsolicited event), without blocking.
    template <class M, class Match>
        requires has_clock
    void expect(Pending<M, Match>& p, duration timeout) {
        p.cancel();
        arm(p, timeout);
    }
    /// Requests still waiting.
    [[nodiscard]] std::size_t pending() const noexcept {
        std::size_t n = 0;
        for (auto* l = head_; l; l = l->next) ++n;
        return n;
    }

    /// Wait up to `timeout` for message M (struct or view type) accepted by `match` and return it decoded.
    template <class M, class Match = detail::MatchAll>
        requires has_clock
    Result<message_t<M>> wait_for(duration timeout, Match&& match = {}) {
        std::optional<message_t<M>> out;
        const Error e = wait_impl<view_t<M>>(timeout, match, [&](const auto&, const view_t<M>& v) -> Error {
            auto m = v.to_struct();
            if (!m) return m.error();
            out.emplace(std::move(*m));
            return Error::None;
        });
        if (e != Error::None) return fail(e);
        return std::move(*out);
    }

    /// wait_for() returning the frame's header too.
    template <class M, class Match = detail::MatchAll>
        requires(has_clock && has_header)
    Result<Reply<Header, message_t<M>>> wait_for_reply(duration timeout, Match&& match = {}) {
        std::optional<Reply<Header, message_t<M>>> out;
        const Error e = wait_impl<view_t<M>>(timeout, match, [&](const auto& f, const view_t<M>& v) -> Error {
            auto h = f.header.to_struct();
            if (!h) return h.error();
            auto m = v.to_struct();
            if (!m) return m.error();
            out.emplace(Reply<Header, message_t<M>>{std::move(*h), std::move(*m)});
            return Error::None;
        });
        if (e != Error::None) return fail(e);
        return std::move(*out);
    }

    /// Deliver what has arrived, send `request`, then wait_for<M>(timeout, match).
    template <class M, class Req, class Match = detail::MatchAll>
        requires has_clock
    Result<message_t<M>> transact(const Req& request, duration timeout, Match&& match = {}) {
        service();
        if (auto s = send(request); !s) return fail(s.error());
        return wait_for<M>(timeout, std::forward<Match>(match));
    }
    template <class M, class Req, class Match = detail::MatchAll>
        requires(has_clock && has_header)
    Result<message_t<M>> transact(const Header& header, const Req& request, duration timeout, Match&& match = {}) {
        service();
        if (auto s = send(header, request); !s) return fail(s.error());
        return wait_for<M>(timeout, std::forward<Match>(match));
    }

    // ---- send ---------------------------------------------------------------------------------------------

    /// Choose how send() handles a sink that cannot take a whole frame now (see the file comment). retry without
    /// a timeout keeps trying until the frame is out.
    void set_send_policy(SendPolicy p) noexcept {
        policy_ = p;
        send_limit_ = false;
    }
    void set_send_policy(SendPolicy p, duration timeout) noexcept requires has_clock {
        policy_ = p;
        send_limit_ = true;
        send_timeout_ = timeout;
    }
    [[nodiscard]] SendPolicy send_policy() const noexcept { return policy_; }

    /// Encode `m` and write it to the sink. Returns the frame size.
    template <class M>
    Result<std::size_t> send(const M& m) {
        auto n = Protocol::encode(m, mut_bytes(tx_));
        if (!n) return fail(n.error());
        return send_raw(bytes_view(tx_.data(), *n));
    }
    /// Encode `m` with explicit header values and write it to the sink.
    template <class M>
        requires has_header
    Result<std::size_t> send(const Header& header, const M& m) {
        auto n = Protocol::encode(header, m, mut_bytes(tx_));
        if (!n) return fail(n.error());
        return send_raw(bytes_view(tx_.data(), *n));
    }
    /// Write bytes as they are (e.g. a command the schema does not describe), with the send policy.
    Result<std::size_t> send_raw(bytes_view bytes) {
        if (const Error e = write_out(bytes); e != Error::None) return fail(e);
        return bytes.size();
    }

private:
    // ---- time helpers ----
    std::uint32_t ticks() {
        if constexpr (chrono_clock)
            return static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now().time_since_epoch()).count());
        else return now();
    }
    static std::uint32_t to_ticks(duration d) noexcept {
        if constexpr (chrono_clock) {
            const auto ms = std::chrono::ceil<std::chrono::milliseconds>(d).count();
            if (ms <= 0) return 0;
            return ms > 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<std::uint32_t>(ms);
        } else {
            return d;
        }
    }
    bool elapsed(time_point start, duration limit) {
        if constexpr (chrono_clock) return now() - start >= limit;
        else return static_cast<std::uint32_t>(now() - start) >= limit;
    }
    void idle() {
        if (idle_ctx_fn_) idle_ctx_fn_(idle_ctx_);
        else if (idle_fn_) idle_fn_();
    }

    // ---- input ----
    /// Move source bytes into the parser without processing them. Called only when no overrun gap is pending
    /// (after the parser has asked for more input).
    std::size_t read_input(std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
        if constexpr (detail::is_byte_ring<SourceT>::value) {
            return Parser::push(source_, max_bytes);
        } else {
            auto read = [&] {
                return Parser::push_with([&](mut_bytes free) -> std::size_t {
                    if (free.size() > max_bytes) free = free.first(max_bytes);
                    return static_cast<std::size_t>(source_.read(free));
                });
            };
            if constexpr (requires { source_.take_overrun(); }) {
                if (source_.take_overrun()) Parser::push_gap();
                const std::size_t n = read();
                if (n != 0 || !source_.take_overrun()) return n;
                Parser::push_gap();  // the read found the loss: mark it now, then read what follows
                return read();
            } else {
                return read();
            }
        }
    }

    template <class V, class Match, class F>
    static bool matches(Match& match, const F& f, const V& v) {
        if constexpr (requires { f.header; }) {
            if constexpr (std::is_invocable_r_v<bool, Match&, decltype((f.header)), const V&>) return match(f.header, v);
            else if constexpr (std::is_invocable_r_v<bool, Match&, const V&>) return match(v);
            else static_assert(detail::always_false<V>, "gmsg::Endpoint: match must be callable as bool(const View&) or bool(const HeaderView&, const View&)");
        } else {
            if constexpr (std::is_invocable_r_v<bool, Match&, const V&>) return match(v);
            else static_assert(detail::always_false<V>, "gmsg::Endpoint: match must be callable as bool(const View&)");
        }
    }

    /// Pull frames until a V accepted by match arrives (take(frame_parts, view) decodes it), delivering the
    /// others; read input and tick in between; Timeout after `timeout`.
    template <class V, class Match, class Take>
    Error wait_impl(duration timeout, Match& match, Take&& take) {
        const Busy busy(depth_);
        const time_point start = now();
        bool expired = false;  // after the limit: one last look at what the final read brought in
        for (;;) {
            bool hit = false;
            while (Parser::pull([&](mut_bytes frame) -> Error {
                return this->template open_view<Protocol>(frame, [&](const auto& f, auto& any) -> Error {
                    if (const V* v = std::get_if<V>(&any); v && matches(match, f, *v)) {
                        const Error e = take(f, *v);  // a failed decode drops the frame like any bad frame
                        hit = e == Error::None;
                        return e;
                    }
                    return dispatch(f, any);
                });
            })) {
                this->release();
                fire_completed();
                if (hit) return Error::None;
            }
            if (expired) return Error::Timeout;
            const std::size_t n = read_input();
            Parser::tick(ticks());
            expire_requests();
            if (elapsed(start, timeout)) expired = true;
            else if (n == 0) idle();
        }
    }

    // ---- requests ----
    /// Counts nested service() / waits: request() from a completion callback does not drain input first.
    struct Busy {
        int& d;
        explicit Busy(int& depth) : d(depth) { ++d; }
        ~Busy() { --d; }
    };

    /// One step that offers frames to the waiting requests before the listeners (plain next() when none wait).
    bool next_frame() {
        if (!head_) return this->next();
        const auto s = Parser::step([&](mut_bytes frame) {
            return this->template open_view<Protocol>(frame, [&](const auto& f, auto& any) { return dispatch(f, any); });
        });
        if (s.kind == Parser::StepKind::error) Parser::report(FrameError{s.error, s.bytes});
        this->release();
        fire_completed();
        return s.kind != Parser::StepKind::need_more;
    }

    /// A frame for a waiting request (the first one that accepts it), else for the listeners.
    template <class F, class A>
    Error dispatch(const F& f, A& any) {
        for (auto* l = head_; l; l = l->next) {
            bool taken = false;
            const Error e = l->offer(l, &f, &any, &taken);
            if (e != Error::None) return e;  // its reply failed to decode: dropped like any bad frame
            if (taken) {
                unlink(l);
                complete(l, Error::None);
                return Error::None;
            }
        }
        std::visit([&](const auto& x) { this->deliver(f, x); }, any);
        return Error::None;
    }

    template <class M, class Match>
    void arm(Pending<M, Match>& p, duration timeout) {
        detail::PendingLink& l = p;
        p.result_.reset();
        l.state = RequestState::waiting;
        l.error = Error::None;
        l.start = ticks();
        l.timeout = to_ticks(timeout);
        l.owner = this;
        l.unlink_fn = [](void* owner, detail::PendingLink* x) { static_cast<Endpoint*>(owner)->unlink(x); };
        l.offer = [](detail::PendingLink* x, const void* fp, const void* av, bool* taken) -> Error {
            auto& q = static_cast<Pending<M, Match>&>(*x);
            using V = view_t<M>;
            const auto& any = *static_cast<const typename Protocol::AnyView*>(av);
            const V* v = std::get_if<V>(&any);
            if (!v || !matches(q.match_, *static_cast<const typename Protocol::Frame*>(fp), *v)) return Error::None;
            auto m = v->to_struct();
            if (!m) return m.error();
            q.result_.emplace(std::move(*m));
            *taken = true;
            return Error::None;
        };
        l.finish = [](detail::PendingLink* x) {
            auto& q = static_cast<Pending<M, Match>&>(*x);
            if (x->error != Error::None) q.result_.emplace(fail(x->error));
            q.done_(*q.result_);
        };
        l.next = nullptr;
        detail::PendingLink** at = &head_;
        while (*at) at = &(*at)->next;
        *at = &l;
    }
    void unlink(detail::PendingLink* l) noexcept {
        for (detail::PendingLink** at = &head_; *at; at = &(*at)->next)
            if (*at == l) {
                *at = l->next;
                break;
            }
        l->next = nullptr;
        l->owner = nullptr;
    }
    /// Mark done and queue the callback (run after the current step, so a callback may start a new request).
    void complete(detail::PendingLink* l, Error e) noexcept {
        l->state = RequestState::done;
        l->error = e;
        detail::PendingLink** at = &done_;
        while (*at) at = &(*at)->next;
        l->next = nullptr;
        *at = l;
    }
    void fire_completed() {
        while (auto* l = done_) {
            done_ = l->next;
            l->next = nullptr;
            l->finish(l);
        }
    }
    void expire_requests() {
        if constexpr (has_clock) {
            const std::uint32_t now = ticks();
            for (auto* l = head_; l;) {
                auto* n = l->next;
                if (static_cast<std::uint32_t>(now - l->start) >= l->timeout) {
                    unlink(l);
                    complete(l, Error::Timeout);
                }
                l = n;
            }
        }
        fire_completed();
    }

    // ---- output ----
    std::size_t sink_room() {
        if constexpr (detail::is_byte_ring<SinkT>::value) return sink_.room();
        else return static_cast<std::size_t>(sink_.writable());
    }
    std::size_t sink_write(bytes_view b) {
        if constexpr (detail::is_byte_ring<SinkT>::value) {
            const std::size_t n = std::min(b.size(), sink_.room());
            return sink_.push(b.first(n));
        } else {
            return std::min(static_cast<std::size_t>(sink_.write(b)), b.size());
        }
    }
    static constexpr bool sink_has_room = detail::is_byte_ring<SinkT>::value || requires(SinkT& s) { s.writable(); };

    Error write_out(bytes_view b) {
        if (policy_ == SendPolicy::whole_frame) {
            if constexpr (sink_has_room)
                if (sink_room() < b.size()) return Error::BufferFull;
            return sink_write(b) == b.size() ? Error::None : Error::BufferFull;
        }
        [[maybe_unused]] time_point start{};
        if constexpr (has_clock)
            if (send_limit_) start = now();
        for (;;) {
            b = b.subspan(sink_write(b));
            if (b.empty()) return Error::None;
            if constexpr (has_clock)
                if (send_limit_ && elapsed(start, send_timeout_)) return Error::Timeout;
            idle();
        }
    }

    Source source_;
    Sink sink_;
    [[no_unique_address]] ClockStore clock_{};
    std::array<std::uint8_t, Protocol::max_frame> tx_{};
    SendPolicy policy_ = SendPolicy::whole_frame;
    bool send_limit_ = false;
    duration send_timeout_{};
    int depth_ = 0;
    detail::PendingLink* head_ = nullptr;  // requests waiting, oldest first
    detail::PendingLink* done_ = nullptr;  // finished, callbacks not run yet
    void (*idle_fn_)() = nullptr;
    void (*idle_ctx_fn_)(void*) = nullptr;
    void* idle_ctx_ = nullptr;
};

/// Build an Endpoint for protocol P: lvalue sources / sinks are kept by reference, rvalues by value; the clock is
/// copied. Generated protocols call it from P::make_endpoint(source, sink[, clock]).
template <class P, std::size_t Capacity = P::max_frame, class S, class K>
auto make_endpoint(S&& source, K&& sink) {
    return Endpoint<P, S, K, void, Capacity>(std::forward<S>(source), std::forward<K>(sink));
}
template <class P, std::size_t Capacity = P::max_frame, class S, class K, class C>
auto make_endpoint(S&& source, K&& sink, C&& clock) {
    return Endpoint<P, S, K, std::decay_t<C>, Capacity>(std::forward<S>(source), std::forward<K>(sink), std::forward<C>(clock));
}

} // namespace gmsg
