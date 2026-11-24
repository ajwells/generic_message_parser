#pragma once
// gmp/print.hpp - human readable printing of generated messages for logs and troubleshooting.
//
// Generated structs, views and bits groups have `void print_to(gmsg::Printer&) const`. Use the free functions:
//
//   gmsg::println(msg, stdout);                  // FILE*
//   char buf[256];  gmsg::print(msg, buf);       // char buffer (always NUL terminated)
//   gmsg::static_string<256> s; gmsg::print(msg, s);   // appends; also std::string or anything with append()
//   gmsg::print(msg, std::cout);                 // anything with write(const char*, n), e.g. std::ostream
//   gmsg::print(msg, [](std::string_view t) { uart_log(t); });   // callback
//
// Output (multi-line by default, `{.style = gmsg::PrintStyle::single_line}` for one line):
//
//   Status {
//     voltage: 12600 (12.6 V)
//     state: {
//       armed: true
//       mode: run (0x01)
//     }
//     samples: [1, 2, 3]
//     fault_code: <absent>
//   }
//
// No heap, no exceptions. Bounded sinks (char buffers, static_string) report truncation as Error::BufferFull.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "gmp/containers.hpp"
#include "gmp/result.hpp"

namespace gmsg {

enum class PrintStyle : std::uint8_t {
    multi_line,   ///< one field per line, nested values indented
    single_line,  ///< everything on one line: `Status { voltage: 12600 (12.6 V), ... }`
};

struct PrintOptions {
    PrintStyle style = PrintStyle::multi_line;
    std::uint8_t indent = 2;       ///< spaces per nesting level (multi_line)
    std::size_t max_items = 64;    ///< array elements shown before "... (+N more)"; 0 = all
    std::size_t max_bytes = 64;    ///< bytes shown for bytes fields before "... (+N more)"; 0 = all
    int precision = 9;             ///< significant digits of scaled / converted values
};

/// Marks raw bytes so they print as hex: `[0A 1B 2C]`.
struct HexBytes {
    bytes_view bytes;
};
inline HexBytes hex(bytes_view b) noexcept { return {b}; }

class Printer;

/// Types the printer formats with their own `print_to` (generated structs, views, bits groups).
template <class T>
concept SelfPrinting = requires(const T& t, Printer& p) { t.print_to(p); };

/// Custom types opt in by providing `void gmsg_print(gmsg::Printer&, const T&)` next to the type (found by ADL).
template <class T>
concept CustomPrinting = requires(const T& t, Printer& p) { gmsg_print(p, t); };

/// Formats values into a type-erased character sink. Generated `print_to` methods drive it field by field.
class Printer {
public:
    /// Writes `s`; returns false when the sink is full (the printer then stops writing).
    using WriteFn = bool (*)(void* ctx, std::string_view s) noexcept;

    Printer(void* ctx, WriteFn fn, const PrintOptions& opts = {}) noexcept : ctx_(ctx), fn_(fn), opts_(opts) {}

    [[nodiscard]] const PrintOptions& options() const noexcept { return opts_; }
    [[nodiscard]] bool multi_line() const noexcept { return opts_.style == PrintStyle::multi_line; }
    /// Characters handed to the sink so far.
    [[nodiscard]] std::size_t written() const noexcept { return written_; }
    /// True when a bounded sink ran out of space.
    [[nodiscard]] bool truncated() const noexcept { return full_; }

    // ---- structure (used by generated code) ----

    /// Opens a block: `Type {` (or `{` for an unnamed group).
    void begin(std::string_view type) noexcept {
        close_note();
        if (!type.empty()) {
            put(type);
            put(" ");
        }
        put("{");
        push();
    }
    /// Closes the block opened by begin().
    void end() noexcept {
        close_note();
        const bool empty = first();
        pop();
        if (empty) put("}");
        else if (multi_line()) {
            newline();
            put("}");
        } else {
            put(" }");
        }
    }
    /// Starts a field line: `name: `.
    void key(std::string_view name) noexcept {
        close_note();
        separate();
        put(name);
        put(": ");
    }
    template <class T>
    void field(std::string_view name, const T& v) noexcept {
        key(name);
        value(v);
    }
    /// A conditional field that is not present.
    void absent(std::string_view name) noexcept {
        key(name);
        put("<absent>");
    }
    /// A value derived from other fields (`computed:`).
    template <class T>
    void computed(std::string_view name, const T& v, std::string_view unit = {}) noexcept {
        key(name);
        value(v);
        if (!unit.empty()) this->unit(unit);
        note("computed");
    }
    /// Unit of the value just printed: `12 V`.
    void unit(std::string_view u) noexcept {
        if (u.empty()) return;
        put(" ");
        put(u);
    }
    /// Engineering conversion of the value just printed, collected in parentheses: `12600 (12.6 V, 54.7 degF)`.
    /// Accepts double, std::optional<double> or gmsg::Result<double> (nothing is printed when empty / failed).
    template <class T>
    void conversion(const T& v, std::string_view u = {}) noexcept {
        if constexpr (is_optional<T>::value) {
            if (v) conversion(*v, u);
        } else if constexpr (is_expected<T>::value) {
            if (v) conversion(*v, u);
        } else {
            open_note();
            put_float(static_cast<double>(v), opts_.precision);
            unit(u);
        }
    }
    /// Free text added to the parenthesised note after a value.
    void note(std::string_view s) noexcept {
        open_note();
        put(s);
    }
    /// Raw text.
    void text(std::string_view s) noexcept { put(s); }
    /// Closes a pending note; call after the last value (gmsg::print does this).
    void flush() noexcept { close_note(); }

    // ---- values ----

    template <class T>
    void value(const T& v) noexcept {
        using U = std::remove_cvref_t<T>;
        if constexpr (SelfPrinting<U>) {
            v.print_to(*this);
        } else if constexpr (CustomPrinting<U>) {
            gmsg_print(*this, v);
        } else if constexpr (std::is_same_v<U, bool>) {
            put(v ? "true" : "false");
        } else if constexpr (std::is_same_v<U, char>) {
            put_char(v);
        } else if constexpr (std::is_enum_v<U>) {
            put_enum(v);
        } else if constexpr (std::is_integral_v<U>) {
            put_int(v);
        } else if constexpr (std::is_floating_point_v<U>) {
            put_float(v, -1);
        } else if constexpr (std::is_same_v<U, HexBytes>) {
            put_hex(v.bytes);
        } else if constexpr (std::is_convertible_v<const U&, std::string_view>) {
            put_quoted(std::string_view(v));
        } else if constexpr (is_optional<U>::value) {
            if (v) value(*v);
            else put("<absent>");
        } else if constexpr (is_expected<U>::value) {
            if (v) value(*v);
            else {
                put("<error: ");
                put(to_string(v.error()));
                put(">");
            }
        } else if constexpr (is_variant<U>::value) {
            std::visit([this](const auto& x) { value(x); }, v);
        } else if constexpr (requires { v.begin(); v.end(); v.size(); }) {
            list(v);
        } else {
            put("<?>");
        }
    }

    /// `[a, b, c]`; messages in multi-line mode get one element per line.
    template <class R>
    void list(const R& r) noexcept {
        using E = std::remove_cvref_t<decltype(*r.begin())>;
        constexpr bool blocks = SelfPrinting<E>;
        const std::size_t n = static_cast<std::size_t>(r.size());
        const std::size_t shown = opts_.max_items && n > opts_.max_items ? opts_.max_items : n;
        put("[");
        if (n == 0) {
            put("]");
            return;
        }
        const bool lines = blocks && multi_line();
        if (lines) push();
        std::size_t i = 0;
        for (const auto& e : r) {
            if (i == shown) break;
            if (lines) newline();
            else if (i) put(", ");
            value(e);
            ++i;
        }
        if (shown < n) {
            if (lines) newline();
            else put(", ");
            more(n - shown);
        }
        if (lines) {
            pop();
            newline();
        }
        put("]");
    }

    void put(std::string_view s) noexcept {
        if (full_ || s.empty()) return;
        if (!fn_(ctx_, s)) {
            full_ = true;
            return;
        }
        written_ += s.size();
    }

private:
    template <class T> struct is_optional : std::false_type {};
    template <class T> struct is_optional<std::optional<T>> : std::true_type {};
    template <class T> struct is_expected : std::false_type {};
    template <class T, class E> struct is_expected<std::expected<T, E>> : std::true_type {};
    template <class T> struct is_variant : std::false_type {};
    template <class... T> struct is_variant<std::variant<T...>> : std::true_type {};

    static constexpr unsigned kMaxDepth = 63;

    void push() noexcept {
        if (depth_ < kMaxDepth) first_ |= std::uint64_t{1} << depth_;
        ++depth_;
    }
    void pop() noexcept {
        if (depth_) --depth_;
    }
    // "first entry of the innermost block" flag lives at bit depth_-1
    [[nodiscard]] bool first() const noexcept {
        return depth_ && depth_ - 1 < kMaxDepth && ((first_ >> (depth_ - 1)) & 1u);
    }
    void clear_first() noexcept {
        if (depth_ && depth_ - 1 < kMaxDepth) first_ &= ~(std::uint64_t{1} << (depth_ - 1));
    }
    void separate() noexcept {
        if (multi_line()) newline();
        else put(first() ? " " : ", ");
        clear_first();
    }
    void newline() noexcept {
        put("\n");
        for (unsigned i = 0; i < depth_ * opts_.indent; ++i) put(" ");
    }

    void open_note() noexcept {
        put(note_open_ ? ", " : " (");
        note_open_ = true;
    }
    void close_note() noexcept {
        if (!note_open_) return;
        note_open_ = false;
        put(")");
    }
    void more(std::size_t k) noexcept {
        put("... (+");
        put_int(k);
        put(" more)");
    }

    template <class I>
    void put_int(I v) noexcept {
        char tmp[24];
        const auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
        put(std::string_view(tmp, static_cast<std::size_t>(r.ptr - tmp)));
    }
    /// precision > 0: that many significant digits; otherwise the shortest text that reads back as the same
    /// value of type F (so a float 0.9f prints as 0.9, not as the double 0.8999999761581421).
    template <class F>
    void put_float(F v, int precision) noexcept {
        char tmp[40];
        std::to_chars_result r = precision > 0
                                     ? std::to_chars(tmp, tmp + sizeof tmp, v, std::chars_format::general, precision)
                                     : std::to_chars(tmp, tmp + sizeof tmp, v);
        if (r.ec != std::errc{}) {
            put("<float>");
            return;
        }
        put(std::string_view(tmp, static_cast<std::size_t>(r.ptr - tmp)));
    }
    void put_hex_digits(std::uint64_t v, std::size_t bytes) noexcept {
        static constexpr char digits[] = "0123456789ABCDEF";
        char tmp[16];
        const std::size_t n = bytes * 2;
        for (std::size_t i = 0; i < n; ++i) tmp[n - 1 - i] = digits[(v >> (4 * i)) & 0xF];
        put(std::string_view(tmp, n));
    }
    template <class E>
    void put_enum(E v) noexcept {
        using UT = std::underlying_type_t<E>;
        const std::string_view name = to_string(v);  // generated next to each enum, found by ADL
        put(name.empty() ? std::string_view("<unknown>") : name);
        put(" (0x");
        put_hex_digits(static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<UT>>(static_cast<UT>(v))), sizeof(UT));
        put(")");
    }
    void put_escaped_char(char c, char quote) noexcept {
        switch (c) {
            case '\n': put("\\n"); return;
            case '\r': put("\\r"); return;
            case '\t': put("\\t"); return;
            case '\\': put("\\\\"); return;
            default: break;
        }
        if (c == quote) {
            const char e[2] = {'\\', c};
            put(std::string_view(e, 2));
            return;
        }
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F) {
            put("\\x");
            put_hex_digits(u, 1);
            return;
        }
        put(std::string_view(&c, 1));
    }
    void put_char(char c) noexcept {
        put("'");
        put_escaped_char(c, '\'');
        put("'");
    }
    void put_quoted(std::string_view s) noexcept {
        put("\"");
        for (char c : s) put_escaped_char(c, '"');
        put("\"");
    }
    void put_hex(bytes_view b) noexcept {
        const std::size_t shown = opts_.max_bytes && b.size() > opts_.max_bytes ? opts_.max_bytes : b.size();
        put("[");
        for (std::size_t i = 0; i < shown; ++i) {
            if (i) put(" ");
            put_hex_digits(b[i], 1);
        }
        if (shown < b.size()) {
            put(" ");
            more(b.size() - shown);
        }
        put("]");
    }

    void* ctx_;
    WriteFn fn_;
    PrintOptions opts_;
    std::size_t written_ = 0;
    unsigned depth_ = 0;
    std::uint64_t first_ = 0;
    bool note_open_ = false;
    bool full_ = false;
};

namespace detail {

struct CharBufferSink {
    char* p;
    std::size_t cap;  // including the NUL terminator
    std::size_t n = 0;
    static bool write(void* c, std::string_view s) noexcept {
        auto& b = *static_cast<CharBufferSink*>(c);
        if (b.cap == 0) return false;
        const std::size_t room = b.cap - 1 - b.n;
        const std::size_t k = s.size() < room ? s.size() : room;
        for (std::size_t i = 0; i < k; ++i) b.p[b.n + i] = s[i];
        b.n += k;
        b.p[b.n] = '\0';
        return k == s.size();
    }
};

template <class S>
bool append_to(void* c, std::string_view s) noexcept {
    auto& str = *static_cast<S*>(c);
    if constexpr (std::is_convertible_v<decltype(str.append(s)), bool>) return static_cast<bool>(str.append(s));
    else {
        str.append(s);
        return true;
    }
}

template <class S>
bool stream_write(void* c, std::string_view s) noexcept {
    static_cast<S*>(c)->write(s.data(), static_cast<std::ptrdiff_t>(s.size()));
    return true;
}

template <class F>
bool call_sink(void* c, std::string_view s) noexcept {
    auto& f = *static_cast<F*>(c);
    if constexpr (std::is_convertible_v<std::invoke_result_t<F&, std::string_view>, bool>) return static_cast<bool>(f(s));
    else {
        f(s);
        return true;
    }
}

inline bool file_write(void* c, std::string_view s) noexcept {
    return std::fwrite(s.data(), 1, s.size(), static_cast<std::FILE*>(c)) == s.size();
}

} // namespace detail

/// Prints `v` (a generated struct / view / enum / any printable value) to `sink`.
/// Sinks: `std::FILE*`, a char array or `std::span<char>` (NUL terminated, truncation = BufferFull),
/// any string with `append(std::string_view)` (gmsg::static_string, std::string; text is appended),
/// any stream with `write(const char*, n)` (std::ostream), or a callable taking `std::string_view`
/// (return false from it to stop). Returns the number of characters written.
template <class T, class Sink>
Result<std::size_t> print(const T& v, Sink&& sink, const PrintOptions& opts = {}) noexcept;

/// print() followed by a newline.
template <class T, class Sink>
Result<std::size_t> println(const T& v, Sink&& sink, const PrintOptions& opts = {}) noexcept;

namespace detail {

template <class T>
Result<std::size_t> finish(Printer& p, const T& v, bool newline) noexcept {
    p.value(v);
    p.flush();
    if (newline) p.put("\n");
    if (p.truncated()) return fail(Error::BufferFull);
    return p.written();
}

template <class T, class Sink>
Result<std::size_t> print_impl(const T& v, Sink& sink, const PrintOptions& opts, bool newline) noexcept {
    using S = std::remove_cvref_t<Sink>;
    if constexpr (std::is_same_v<S, std::FILE*>) {
        Printer p(sink, &file_write, opts);
        return finish(p, v, newline);
    } else if constexpr (std::is_array_v<S> && std::is_same_v<std::remove_extent_t<S>, char>) {
        CharBufferSink b{sink, std::extent_v<S>};
        if (b.cap) b.p[0] = '\0';
        Printer p(&b, &CharBufferSink::write, opts);
        auto r = finish(p, v, newline);
        if (!r) return r;
        return b.n;
    } else if constexpr (std::is_same_v<S, std::span<char>>) {
        CharBufferSink b{sink.data(), sink.size()};
        if (b.cap) b.p[0] = '\0';
        Printer p(&b, &CharBufferSink::write, opts);
        auto r = finish(p, v, newline);
        if (!r) return r;
        return b.n;
    } else if constexpr (requires(S& s, std::string_view t) { s.append(t); }) {
        Printer p(&sink, &append_to<S>, opts);
        return finish(p, v, newline);
    } else if constexpr (requires(S& s, const char* d) { s.write(d, 1); }) {
        Printer p(&sink, &stream_write<S>, opts);
        return finish(p, v, newline);
    } else if constexpr (std::is_invocable_v<S&, std::string_view>) {
        Printer p(&sink, &call_sink<S>, opts);
        return finish(p, v, newline);
    } else {
        static_assert(sizeof(S) == 0,
                      "gmsg::print: unsupported sink (use FILE*, char[N], std::span<char>, a string with append(), "
                      "a stream with write(), or a callable taking std::string_view)");
        return fail(Error::InvalidValue);
    }
}

} // namespace detail

template <class T, class Sink>
Result<std::size_t> print(const T& v, Sink&& sink, const PrintOptions& opts) noexcept {
    return detail::print_impl(v, sink, opts, false);
}

template <class T, class Sink>
Result<std::size_t> println(const T& v, Sink&& sink, const PrintOptions& opts) noexcept {
    return detail::print_impl(v, sink, opts, true);
}

} // namespace gmsg
