#pragma once
// gmp/text.hpp - tokenizer and numeric text codecs for ASCII protocols (NMEA, CSV-like, ...).

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "gmp/containers.hpp"
#include "gmp/result.hpp"

namespace gmsg::text {

/// Split `s` on `sep`; preserves empty tokens ("a,,b" -> "a","","b").
class Tokenizer {
public:
    constexpr Tokenizer(std::string_view s, char sep) noexcept : s_(s), sep_(sep), done_(false) {}

    constexpr std::optional<std::string_view> next() noexcept {
        if (done_) return std::nullopt;
        const auto p = s_.find(sep_);
        if (p == std::string_view::npos) {
            done_ = true;
            return s_;
        }
        auto tok = s_.substr(0, p);
        s_.remove_prefix(p + 1);
        return tok;
    }

    /// Remaining (unconsumed) text.
    [[nodiscard]] constexpr std::string_view rest() const noexcept { return done_ ? std::string_view{} : s_; }

private:
    std::string_view s_;
    char sep_;
    bool done_;
};

/// Split into exactly N token slots; returns the number of tokens found (may exceed N - extras ignored).
template <std::size_t N>
constexpr std::size_t split(std::string_view s, char sep, std::array<std::string_view, N>& out) noexcept {
    Tokenizer t(s, sep);
    std::size_t n = 0;
    while (auto tok = t.next()) {
        if (n < N) out[n] = *tok;
        ++n;
    }
    for (std::size_t i = n; i < N; ++i) out[i] = std::string_view{};
    return n;
}

template <class T>
Result<T> parse_int(std::string_view s, int base = 10) noexcept {
    static_assert(std::is_integral_v<T>);
    if (s.empty()) return fail(Error::MissingField);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    T v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (ec == std::errc::result_out_of_range) return fail(Error::OutOfRange);
    if (ec != std::errc{} || p != s.data() + s.size()) return fail(Error::ParseError);
    return v;
}

template <class T>
Result<T> parse_float(std::string_view s) noexcept {
    static_assert(std::is_floating_point_v<T>);
    if (s.empty()) return fail(Error::MissingField);
    if (s.front() == '+') s.remove_prefix(1);
    T v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec == std::errc::result_out_of_range) return fail(Error::OutOfRange);
    if (ec != std::errc{} || p != s.data() + s.size()) return fail(Error::ParseError);
    return v;
}

inline Result<char> parse_char(std::string_view s) noexcept {
    if (s.empty()) return fail(Error::MissingField);
    if (s.size() != 1) return fail(Error::ParseError);
    return s.front();
}

/// Parse exactly two hex digits (case-insensitive).
inline Result<std::uint8_t> parse_hex_byte(std::string_view s) noexcept {
    if (s.size() != 2) return fail(Error::ParseError);
    return parse_int<std::uint8_t>(s, 16);
}

inline constexpr char hex_digit(unsigned v, bool upper = true) noexcept {
    return static_cast<char>(v < 10 ? '0' + v : (upper ? 'A' : 'a') + (v - 10));
}

/// Writes text tokens into a caller supplied byte buffer, inserting separators automatically.
class TextWriter {
public:
    TextWriter(mut_bytes out, char sep) noexcept
        : out_(reinterpret_cast<char*>(out.data())), cap_(out.size()), sep_(sep) {}

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    /// Start a new token (writes the separator if this is not the first token).
    VoidResult begin_token() noexcept {
        if (!first_) {
            if (!put(sep_)) return fail(Error::BufferFull);
        }
        first_ = false;
        return {};
    }

    VoidResult raw(std::string_view s) noexcept {
        if (cap_ - pos_ < s.size()) { failed_ = true; return fail(Error::BufferFull); }
        for (char c : s) out_[pos_++] = c;
        return {};
    }

    VoidResult empty_token() noexcept { return begin_token(); }

    VoidResult token(std::string_view s) noexcept {
        if (auto r = begin_token(); !r) return r;
        return raw(s);
    }

    VoidResult token(char c) noexcept {
        if (auto r = begin_token(); !r) return r;
        return put(c) ? VoidResult{} : fail(Error::BufferFull);
    }

    /// Integer token. width > 0 zero-pads to that many digits.
    template <class T>
    VoidResult int_token(T v, int base = 10, int width = 0, bool upper = true) noexcept {
        if (auto r = begin_token(); !r) return r;
        std::array<char, 72> tmp{};
        auto [p, ec] = std::to_chars(tmp.data(), tmp.data() + tmp.size(), v, base);
        if (ec != std::errc{}) return fail(Error::InvalidValue);
        std::string_view digits(tmp.data(), static_cast<std::size_t>(p - tmp.data()));
        bool neg = !digits.empty() && digits.front() == '-';
        if (neg) { if (!put('-')) return fail(Error::BufferFull); digits.remove_prefix(1); }
        for (int i = static_cast<int>(digits.size()); i < width; ++i)
            if (!put('0')) return fail(Error::BufferFull);
        if (upper && base > 10) {
            for (char c : digits) if (!put(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c)) return fail(Error::BufferFull);
            return {};
        }
        return raw(digits);
    }

    /// Floating point token. precision >= 0 -> fixed notation with that many decimals;
    /// width > 0 zero-pads the integer part to `width` total characters (NMEA style "0123.45").
    template <class T>
    VoidResult float_token(T v, int precision = -1, int width = 0) noexcept {
        if (auto r = begin_token(); !r) return r;
        // formatted straight into the output, so any length fits that the output buffer can hold
        char* const at = out_ + pos_;
        char* const end = out_ + cap_;
        const std::to_chars_result res = precision >= 0 ? std::to_chars(at, end, v, std::chars_format::fixed, precision)
                                                        : std::to_chars(at, end, v);
        if (res.ec != std::errc{}) { failed_ = true; return fail(Error::BufferFull); }
        const auto n = static_cast<std::size_t>(res.ptr - at);
        if (width > 0 && static_cast<std::size_t>(width) > n) {  // zero padding after the sign
            const std::size_t pad = static_cast<std::size_t>(width) - n;
            if (cap_ - pos_ < n + pad) { failed_ = true; return fail(Error::BufferFull); }
            const std::size_t sign = at[0] == '-' ? 1 : 0;
            std::memmove(at + sign + pad, at + sign, n - sign);
            std::memset(at + sign, '0', pad);
            pos_ += n + pad;
            return {};
        }
        pos_ += n;
        return {};
    }

private:
    bool put(char c) noexcept {
        if (pos_ >= cap_) { failed_ = true; return false; }
        out_[pos_++] = c;
        return true;
    }

    char* out_;
    std::size_t cap_;
    std::size_t pos_ = 0;
    char sep_;
    bool first_ = true;
    bool failed_ = false;
};

} // namespace gmsg::text
