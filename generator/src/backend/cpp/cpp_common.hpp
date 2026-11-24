#pragma once
// Shared helpers for the C++ backend: naming, type mapping, literals, expressions.

#include <functional>
#include <string>

#include "backend/code_writer.hpp"
#include "diagnostics.hpp"
#include "ir/ir.hpp"

namespace gmpc::cpp {

struct Ctx {
    const ir::Schema& schema;
    Diagnostics& diag;
    CodeWriter& w;
};

/// C++-safe identifier (keywords and generated member names get a trailing underscore).
std::string ident(const std::string& name);

std::string prim_type(ir::Prim p);
std::string endian_expr(ir::Endian e);
std::string bit_order_expr(ir::BitOrder o);
std::string view_name(const ir::Message& m);
std::string bits_type_name(const ir::Message& m, const ir::Field& f);

/// Type of a single value of the field (no optional wrapper). For arrays: the container type.
std::string value_type(const ir::Message& m, const ir::Field& f);
/// Element type of a scalar-ish field (Scalar/Enum/Message/Custom) - used for array elements.
std::string element_type(const ir::Message& m, const ir::Field& f);
/// Struct member type (optional<> for conditional / optional fields).
std::string member_type(const ir::Message& m, const ir::Field& f);

/// Integer literal converted to the field's C++ type.
std::string typed_literal(const ir::Field& f, std::int64_t v);
std::string int_literal(std::int64_t v);
std::string double_literal(double v);
/// Condition that is true when value `v` (an expression without side effects) fails one of `checks`; "" when there
/// are none. `scale` / `offset` give the scaled value, computed exactly as the <name>_scaled() accessor.
std::string limit_fail(const std::vector<ir::LimitCheck>& checks, const std::string& v, ir::Prim prim,
                       std::optional<double> scale, std::optional<double> offset);
std::string string_literal(const std::string& s);

/// Language block value: custom->lang["cpp"][key]; empty if missing.
std::string lang_value(const ir::LangBlocks& lang, const std::string& key);

/// Emits a reference to a field (and optional bit field) in some context.
using RefFn = std::function<std::string(const ir::Field&, const ir::BitField*)>;
std::string expr(const ir::Expr& e, const RefFn& ref);

/// C++ expression naming the checksum algorithm type, e.g. gmsg::checksum::crc16_ccitt.
/// For custom checksums returns the function name and sets is_function.
std::string checksum_impl(const ir::ChecksumSpec& c, bool& is_function);

/// "/// text" doc comment lines (nothing if empty).
void doc(CodeWriter& w, const std::string& text);

/// True if expression `e` (a length/count) is a bare reference whose field is derived from `f`.
bool is_derived_length(const ir::Field& f);

/// `print_to` support: unit and engineering conversions printed after scalar value expression `value`
/// (statements use the printer parameter `gmsg_p`).
void emit_print_extras(CodeWriter& w, const ir::Field& f, const std::string& value);
bool has_print_extras(const ir::Field& f);

// Emitters (implemented in emit_*.cpp)
void emit_binary_message(Ctx& c, const ir::Message& m);
void emit_text_message(Ctx& c, const ir::Message& m);
void emit_protocol(Ctx& c, const ir::Protocol& p);

} // namespace gmpc::cpp
