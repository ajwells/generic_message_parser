#pragma once
// gmp/polled.hpp - result of the stream parser's pull API (poll(), poll_frame(), poll_for<V>(), poll_event()).
//
// Behaves like std::optional<T>. A polled view points into the parser's buffer and is valid until the next
// push / poll / feed / next / reset on that parser. In Debug builds (NDEBUG not defined) every access through
// operator* / operator-> / value() checks that, and GMSG_ASSERT fires if the view has expired.
// Define GMSG_ASSERT(x) before including gmp headers to route the failure elsewhere (the same macro as the C
// runtime's: one argument, the condition joined with a message by &&).

#include <cstdint>
#include <optional>
#include <utility>

#ifndef GMSG_ASSERT
#include <cassert>
#define GMSG_ASSERT(x) assert(x)
#endif

namespace gmsg {

template <class T>
class Polled {
public:
    Polled() noexcept = default;
    /// `generation` is the parser's buffer generation; the value stays valid while it is unchanged.
    Polled(T value, const std::uint32_t* generation) noexcept
        : value_(std::move(value)), gen_(generation), at_(generation ? *generation : 0) {}

    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return value_.has_value(); }

    /// True while the parser buffer the value points into is unchanged (always true when empty).
    [[nodiscard]] bool valid() const noexcept { return !gen_ || *gen_ == at_; }

    const T& operator*() const noexcept {
        check();
        return *value_;
    }
    const T* operator->() const noexcept {
        check();
        return &*value_;
    }
    const T& value() const noexcept { return **this; }

private:
    void check() const noexcept {
        GMSG_ASSERT(value_.has_value() && "gmsg::Polled: no value (check it before use)");
        GMSG_ASSERT(valid() && "gmsg::Polled: the polled view expired (the parser buffer changed: push/poll/feed/next/reset)");
    }

    std::optional<T> value_{};
    const std::uint32_t* gen_ = nullptr;
    std::uint32_t at_ = 0;
};

} // namespace gmsg
