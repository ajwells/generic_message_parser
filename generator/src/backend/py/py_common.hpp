#pragma once
// Shared helpers for the Python backend: naming, literals, expressions, type mapping, print calls.
//
// Python mapping of the IR (see runtime-py/gmsg for the runtime):
//   integers  int (checked against the wire type on encode)      bool  bool        char  one-character str
//   floats    float (f32 values rounded with gmsg.to_f32)          enum  gmsg.GmpEnum (IntEnum) member, or the int
//                                                                        when undeclared (non-strict enums)
//   bytes     bytes (views: memoryview)   string  str, one character per byte (latin-1)   array  list
//   messages  dataclasses                 optional / conditional fields  None when absent  errors  gmsg.GmpError
// Expressions (lengths, counts, conditions, computed values) are evaluated with int64 semantics (gmsg.i64 wraps
// +, -, *, <<), like the std::int64_t arithmetic of the C++ backend.

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "backend/code_writer.hpp"
#include "diagnostics.hpp"
#include "ir/ir.hpp"

namespace gmpc::py {

/// Extension modules (`python: { module: ... }` of custom items): module -> local name (_ext0, ...).
struct Modules {
    std::map<std::string, std::string> vars;
    std::vector<std::string> order;
    const std::string& var(const std::string& module);
};

/// Module-level constants (e.g. CRC algorithms with inline parameters, built once): initialiser -> name.
struct Consts {
    std::map<std::string, std::string> names;
    std::vector<std::pair<std::string, std::string>> order;  // (name, initialiser)
    const std::string& name(const std::string& init, const std::string& prefix);
};

struct Ctx {
    const ir::Schema& schema;
    Diagnostics& diag;
    CodeWriter& w;
    Modules& modules;
    Consts& consts;
    std::vector<std::string>& exports;  // names of __all__, in emission order
};

/// Python-safe attribute name: Python keywords, C++ keywords (so member names match the C++ backend) and the members
/// of generated classes get a trailing underscore.
std::string ident(const std::string& name);
/// Enum member name (also avoids the members of IntEnum / gmsg.GmpEnum).
std::string enum_ident(const std::string& name);
/// True if a type name would shadow a name the generated module relies on, or is a keyword.
bool reserved_type_name(const std::string& name);

std::string prim_id(ir::Prim p);  // 'u16', 'f32', ... (quoted)
std::string endian(ir::Endian e);  // 'big' / 'little' (quoted)
std::string bit_order(ir::BitOrder o);
std::string view_name(const ir::Message& m);
std::string bits_type_name(const ir::Message& m, const ir::Field& f);

std::string string_literal(const std::string& s);
std::string bytes_literal(const std::vector<std::uint8_t>& v);
std::string double_literal(double v);
/// Integer literal of type p (value converted as static_cast<T>); True / False; a one-character string for char.
std::string int_literal(std::int64_t v, ir::Prim p);
/// The Python value of constant `v` for field `f`.
std::string typed_literal(const ir::Field& f, std::int64_t v);
/// Primitive storage type of a scalar / enum / bit field (the enum's underlying type for enums).
ir::Prim storage_prim(const ir::Field& f);
/// `v` as the field's value: `Enum.of(v)` for enum fields, else v.
std::string enum_of(const ir::Field& f, const std::string& v);

/// Condition that is true when value `v` fails one of `checks`; "" when there are none.
std::string limit_fail(const std::vector<ir::LimitCheck>& checks, const std::string& v, ir::Prim prim,
                       std::optional<double> scale, std::optional<double> offset);

/// Custom item function: `<module var>.<name>` (empty when the python block lacks the key).
std::string ext_fn(Ctx& c, const ir::LangBlocks& lang, const std::string& key);
std::string lang_value(const ir::LangBlocks& lang, const std::string& key);

/// Expression referencing a field (and optional bit field) in some context.
using RefFn = std::function<std::string(const ir::Field&, const ir::BitField*)>;
/// The expression as an int (int64 semantics).
std::string expr(const ir::Expr& e, const RefFn& ref);
/// The expression as a Python condition (comparisons and logic directly, else `expr != 0`).
std::string condition(const ir::Expr& e, const RefFn& ref);
/// An int expression converted to type p (static_cast<T>(int64)).
std::string from_int(const std::string& v, ir::Prim p);
/// An int expression converted to the value of field f (enum member for enum fields).
std::string from_int(const std::string& v, const ir::Field& f);

/// The checksum algorithm object (preset, inline CRC parameters) or custom function; `is_function` for custom ones.
std::string checksum_algo(Ctx& c, const ir::ChecksumSpec& cs, bool& is_function);
/// int checksum of data[start:end] (all strings are Python expressions).
std::string checksum_value(Ctx& c, const ir::ChecksumSpec& cs, const std::string& data, const std::string& start,
                           const std::string& end);

/// Expression printing value `v` of a single (non-array) field element with printer `p`.
std::string print_value(Ctx& c, const ir::Message& m, const ir::Field& f, const std::string& v);
/// Unit and conversions after a scalar value.
void emit_print_extras(CodeWriter& w, const ir::Field& f, const std::string& value);
/// The field value as a float for engineering conversions (static_cast<double>).
std::string to_double(const ir::Field& f, const std::string& v);

void doc(CodeWriter& w, const std::string& text);
bool is_derived_length(const ir::Field& f);

// Emitters (emit_*.cpp)
void emit_binary_message(Ctx& c, const ir::Message& m);
void emit_text_message(Ctx& c, const ir::Message& m);
void emit_protocol(Ctx& c, const ir::Protocol& p);

} // namespace gmpc::py
