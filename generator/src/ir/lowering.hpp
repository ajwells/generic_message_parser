#pragma once
// Semantic lowering: resolve type references and expressions, compute layout, validate.

#include "diagnostics.hpp"
#include "ir/ir.hpp"

namespace gmpc {

/// Resolve and lay out the schema in place. Returns false if errors were recorded.
bool lower_schema(ir::Schema& schema, Diagnostics& diag);

/// Structural / naming checks run before lowering (schema/validator.cpp).
void validate_declarations(const ir::Schema& schema, Diagnostics& diag);

/// Cross-cutting checks run after lowering (schema/validator.cpp).
void validate_lowered(const ir::Schema& schema, Diagnostics& diag);

} // namespace gmpc
