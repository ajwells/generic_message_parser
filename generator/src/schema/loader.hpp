#pragma once
// YAML -> IR loader. Performs syntactic checks only; see ir/lowering.hpp for semantic resolution.

#include <memory>
#include <string>

#include "diagnostics.hpp"
#include "ir/ir.hpp"

namespace gmpc {

/// Load a schema file. Returns nullptr when errors were recorded.
std::unique_ptr<ir::Schema> load_schema_file(const std::string& path, Diagnostics& diag);

/// Load a schema from YAML text (`file` is used for diagnostics only).
std::unique_ptr<ir::Schema> load_schema_string(const std::string& yaml, const std::string& file, Diagnostics& diag);

} // namespace gmpc
