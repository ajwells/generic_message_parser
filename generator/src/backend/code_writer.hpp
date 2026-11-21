#pragma once
// Minimal indentation-aware text emitter shared by backends.

#include <sstream>
#include <string>
#include <string_view>

namespace gmpc {

class CodeWriter {
public:
    explicit CodeWriter(std::string indent_unit = "    ") : unit_(std::move(indent_unit)) {}

    /// Emit one line at the current indentation. Empty input emits a blank line.
    CodeWriter& line(std::string_view s = {}) {
        if (!s.empty()) {
            for (int i = 0; i < depth_; ++i) out_ << unit_;
            out_ << s;
        }
        out_ << '\n';
        return *this;
    }

    /// Emit a line and increase indentation (e.g. "if (x) {").
    CodeWriter& open(std::string_view s) {
        line(s);
        ++depth_;
        return *this;
    }

    /// Decrease indentation and emit a line (e.g. "}").
    CodeWriter& close(std::string_view s = "}") {
        if (depth_ > 0) --depth_;
        line(s);
        return *this;
    }

    /// Emit a line one level out (access specifiers, case labels).
    CodeWriter& label(std::string_view s) {
        for (int i = 0; i + 1 < depth_; ++i) out_ << unit_;
        out_ << s << '\n';
        return *this;
    }

    void indent() { ++depth_; }
    void dedent() { if (depth_ > 0) --depth_; }

    /// Emit pre-formatted multi-line text, indenting each line.
    CodeWriter& block(std::string_view text) {
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto nl = text.find('\n', start);
            const auto ln = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
            if (nl == std::string_view::npos && ln.empty()) break;
            line(ln);
            if (nl == std::string_view::npos) break;
            start = nl + 1;
        }
        return *this;
    }

    [[nodiscard]] std::string str() const { return out_.str(); }

private:
    std::ostringstream out_;
    std::string unit_;
    int depth_ = 0;
};

} // namespace gmpc
