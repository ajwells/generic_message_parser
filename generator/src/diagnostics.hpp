#pragma once
// Diagnostics collected while loading / validating a schema.

#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gmpc {

struct SourceLoc {
    std::string file;
    int line = 0;  // 1-based, 0 = unknown
    int col = 0;
};

struct Diagnostic {
    enum class Level { Error, Warning } level;
    SourceLoc loc;
    std::string message;
};

class Diagnostics {
public:
    void error(const SourceLoc& loc, std::string msg) { items_.push_back({Diagnostic::Level::Error, loc, std::move(msg)}); }
    void warning(const SourceLoc& loc, std::string msg) {
        items_.push_back({Diagnostic::Level::Warning, loc, std::move(msg)});
    }
    [[nodiscard]] bool has_errors() const {
        for (const auto& d : items_)
            if (d.level == Diagnostic::Level::Error) return true;
        return false;
    }
    [[nodiscard]] const std::vector<Diagnostic>& items() const { return items_; }

    void print(std::ostream& os) const {
        for (const auto& d : items_) {
            os << (d.loc.file.empty() ? std::string("<schema>") : d.loc.file);
            if (d.loc.line) os << ':' << d.loc.line << ':' << d.loc.col;
            os << ": " << (d.level == Diagnostic::Level::Error ? "error" : "warning") << ": " << d.message << '\n';
        }
    }

private:
    std::vector<Diagnostic> items_;
};

/// Thrown to abort processing after an unrecoverable error has been recorded.
struct FatalError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace gmpc
