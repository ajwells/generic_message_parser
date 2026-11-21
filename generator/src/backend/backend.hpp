#pragma once
// Backend interface. A backend turns a lowered, language-neutral ir::Schema into source files.
// Adding a language = implementing Backend and registering it in backend/registry.cpp.

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"
#include "ir/ir.hpp"

namespace gmpc {

struct OutputFile {
    std::string name;     // relative to the output directory
    std::string content;
};

struct BackendOptions {
    std::map<std::string, std::string> values;  // --opt key=value pass-through

    [[nodiscard]] std::string get(const std::string& key, const std::string& fallback = {}) const {
        auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
};

class Backend {
public:
    virtual ~Backend() = default;
    /// Name used with `gmpc --lang <name>`; also the key of language blocks in the schema (cpp: {...}).
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual std::string_view description() const = 0;
    /// Generate output files. Language specific validation errors go to `diag`.
    virtual std::vector<OutputFile> generate(const ir::Schema& schema, const BackendOptions& opts,
                                             Diagnostics& diag) const = 0;
};

const std::vector<std::unique_ptr<Backend>>& backends();
const Backend* find_backend(std::string_view name);

} // namespace gmpc
