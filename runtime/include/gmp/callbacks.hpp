#pragma once
// gmp/callbacks.hpp - runtime callback slots used by the generated `Protocol::Callbacks` / `CallbackStream`.
//
// A CallbackSlot<Args...> holds one listener: a function pointer plus a context pointer (no heap, no
// std::function). The parser always calls it with the full argument list (e.g. header view + message view);
// the bound function may take all of them or only the last one (the message), and is checked at compile time.
//
//   slot.bind<&App::on_status>(app);        // member function   void App::on_status(const StatusView&)
//   slot.bind<&on_status>(app);             // free function     void on_status(App&, const StatusView&)  (or App*)
//   slot.bind<&on_status>();                // free function     void on_status(const StatusView&)
//   slot.bind(&c_callback, ctx_ptr);        // runtime pointer   void c_callback(void* ctx, const StatusView&)

#include <cstddef>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace gmsg {

namespace detail {

template <class T>
inline constexpr bool always_false = false;

struct no_ctx {};

/// Can `f` be called with the optional context prefix followed by `A...`?
template <class F, class C, class... A>
constexpr bool ctx_invocable() {
    if constexpr (std::is_same_v<C, no_ctx>) return std::is_invocable_v<const F&, A...>;
    else if constexpr (std::is_void_v<C>) return std::is_invocable_v<const F&, C*, A...>;
    else return std::is_invocable_v<const F&, C&, A...> || std::is_invocable_v<const F&, C*, A...>;
}

template <class F, class C, class... A>
void ctx_invoke(const F& f, void* ctx, A&&... a) {
    if constexpr (std::is_same_v<C, no_ctx>) std::invoke(f, std::forward<A>(a)...);
    else if constexpr (std::is_void_v<C>) std::invoke(f, static_cast<C*>(ctx), std::forward<A>(a)...);
    else if constexpr (std::is_invocable_v<const F&, C&, A...>) std::invoke(f, *static_cast<C*>(ctx), std::forward<A>(a)...);
    else std::invoke(f, static_cast<C*>(ctx), std::forward<A>(a)...);
}

template <class... A>
struct arg_list {
    static constexpr std::size_t size = sizeof...(A);
    using last = std::remove_cvref_t<std::tuple_element_t<sizeof...(A) - 1, std::tuple<A...>>>;
};

} // namespace detail

/// Parameter list of a callback: member function, function pointer or (non-generic) lambda / function object.
template <class F, class = void>
struct callback_traits : callback_traits<decltype(&F::operator())> {};
template <class R, class... A> struct callback_traits<R (*)(A...)> { using args = detail::arg_list<A...>; };
template <class R, class... A> struct callback_traits<R (*)(A...) noexcept> { using args = detail::arg_list<A...>; };
template <class R, class C, class... A> struct callback_traits<R (C::*)(A...)> { using args = detail::arg_list<A...>; };
template <class R, class C, class... A> struct callback_traits<R (C::*)(A...) const> { using args = detail::arg_list<A...>; };
template <class R, class C, class... A> struct callback_traits<R (C::*)(A...) noexcept> { using args = detail::arg_list<A...>; };
template <class R, class C, class... A> struct callback_traits<R (C::*)(A...) const noexcept> { using args = detail::arg_list<A...>; };

/// The value type a callback receives last (its message view, AnyView or FrameError), without cv/ref.
/// Generated Callbacks use it to pick the slot a function is registered in.
template <class F>
using callback_message_t = typename callback_traits<std::remove_cvref_t<F>>::args::last;

/// One listener for a parser event delivered as `Args...` (e.g. `const HeaderView&, const StatusView&`).
template <class... Args>
class CallbackSlot {
public:
    using Invoke = void (*)(const CallbackSlot&, Args...);

    /// Call the listener (nothing happens when the slot is empty).
    void operator()(Args... a) const {
        if (invoke_) invoke_(*this, a...);
    }
    [[nodiscard]] explicit operator bool() const noexcept { return invoke_ != nullptr; }
    void reset() noexcept { *this = CallbackSlot{}; }

    /// Bind compile-time function `Fn` with context `ctx` (member function of C, or a function taking C& / C*).
    template <auto Fn, class C>
    void bind(C& ctx) noexcept {
        check<decltype(Fn), C>();
        ctx_ = const_cast<void*>(static_cast<const void*>(std::addressof(ctx)));
        fn_ = nullptr;
        invoke_ = [](const CallbackSlot& s, Args... a) { deliver<decltype(Fn), C>(Fn, s.ctx_, a...); };
    }
    /// Bind compile-time function `Fn` that takes no context.
    template <auto Fn>
    void bind() noexcept {
        check<decltype(Fn), detail::no_ctx>();
        ctx_ = nullptr;
        fn_ = nullptr;
        invoke_ = [](const CallbackSlot&, Args... a) { deliver<decltype(Fn), detail::no_ctx>(Fn, nullptr, a...); };
    }
    /// Bind a function pointer chosen at run time, with a context pointer passed back as its first argument
    /// (C-style `void cb(void* ctx, const View&)`, or a typed `void cb(App* app, const View&)`).
    template <class F, class C>
        requires std::is_function_v<F>
    void bind(F* fn, C* ctx) noexcept {
        check<F*, C>();
        ctx_ = const_cast<void*>(static_cast<const void*>(ctx));
        fn_ = reinterpret_cast<void (*)()>(fn);
        invoke_ = [](const CallbackSlot& s, Args... a) { deliver<F*, C>(reinterpret_cast<F*>(s.fn_), s.ctx_, a...); };
    }
    /// Bind a function pointer chosen at run time that takes no context.
    template <class F>
        requires std::is_function_v<F>
    void bind(F* fn) noexcept {
        check<F*, detail::no_ctx>();
        ctx_ = nullptr;
        fn_ = reinterpret_cast<void (*)()>(fn);
        invoke_ = [](const CallbackSlot& s, Args... a) {
            deliver<F*, detail::no_ctx>(reinterpret_cast<F*>(s.fn_), nullptr, a...);
        };
    }

private:
    static constexpr bool kTwo = sizeof...(Args) == 2;
    using Last = std::tuple_element_t<sizeof...(Args) - 1, std::tuple<Args...>>;

    // Accepted: all arguments, or (when the parser delivers a header too) the message alone.
    template <class F, class C>
    static constexpr void check() noexcept {
        if constexpr (!detail::ctx_invocable<F, C, Args...>() && !(kTwo && detail::ctx_invocable<F, C, Last>()))
            static_assert(detail::always_false<F>,
                          "gmsg callback: the function cannot be called with this slot's arguments. Expected "
                          "(View) or, for protocols with a header, (HeaderView, View), optionally after a context "
                          "parameter (C& or C*), or a member function of the context type.");
    }

    template <class F, class C>
    static void deliver(const F& f, void* ctx, Args... a) {
        if constexpr (detail::ctx_invocable<F, C, Args...>()) {
            detail::ctx_invoke<F, C>(f, ctx, a...);
        } else {
            detail::ctx_invoke<F, C>(f, ctx, std::get<sizeof...(Args) - 1>(std::forward_as_tuple(a...)));
        }
    }

    void* ctx_ = nullptr;
    void (*fn_)() = nullptr;
    Invoke invoke_ = nullptr;
};

} // namespace gmsg
