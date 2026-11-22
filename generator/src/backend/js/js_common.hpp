#pragma once
// Shared helpers for the JavaScript backend: naming, literals, expressions, type mapping, print calls.
//
// JavaScript mapping of the IR (see runtime-js/gmsg.js for the runtime):
//   integers  Number, BigInt for u64 / i64        bool  boolean        char  one-character string
//   floats    Number (f32 values rounded with Math.fround)              enum  a frozen name -> value object
//   bytes     Uint8Array        string  JS string, one character per byte (latin1)        array  Array
//   optional / conditional fields   undefined when absent        errors  thrown gmsg.GmpError
// Expressions (lengths, counts, conditions, computed values) are evaluated as 64-bit signed BigInt, like the
// std::int64_t arithmetic of the C++ backend.

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "backend/code_writer.hpp"
#include "diagnostics.hpp"
#include "ir/ir.hpp"

namespace gmpc::js {

/// Extension modules (`js: { module: ... }` of custom items): module id -> local variable name.
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
    std::vector<std::string>& exports;  // names returned by the module factory, in emission order
};

/// JS-safe member name: generated member names (and, for the same names as the C++ backend, C++ keywords) get a
/// trailing underscore.
std::string ident(const std::string& name);
/// True if a type name would shadow a JavaScript global the generated code relies on, or is a reserved word.
bool reserved_type_name(const std::string& name);

std::string prim_id(ir::Prim p);  // 'u16', 'f32', ...
bool is_big(ir::Prim p);           // u64 / i64: BigInt
std::string endian(ir::Endian e);  // 'big' / 'little'
std::string bit_order(ir::BitOrder o);
std::string view_name(const ir::Message& m);
std::string bits_type_name(const ir::Message& m, const ir::Field& f);

std::string string_literal(const std::string& s);
std::string double_literal(double v);
/// Integer literal of type p: Number, or `123n` for 64-bit types (value converted as static_cast<T>).
std::string int_literal(std::int64_t v, ir::Prim p);
/// BigInt literal of an int64 value.
std::string big_literal(std::int64_t v);
/// The JS value of constant `v` for field `f` (enum -> its underlying value, bool, char, number, BigInt).
std::string typed_literal(const ir::Field& f, std::int64_t v);
/// Primitive storage type of a scalar / enum / bit field (the enum's underlying type for enums).
ir::Prim storage_prim(const ir::Field& f);

/// Custom item function: `<module var>.<name>` (empty when the js block lacks the key).
std::string ext_fn(Ctx& c, const ir::LangBlocks& lang, const std::string& key);
std::string lang_value(const ir::LangBlocks& lang, const std::string& key);

/// Expression referencing a field (and optional bit field) in some context; returns a JS value expression.
using RefFn = std::function<std::string(const ir::Field&, const ir::BitField*)>;
/// The expression as a BigInt (int64 semantics).
std::string expr(const ir::Expr& e, const RefFn& ref);
/// Convert a BigInt expression to the JS value of type p (static_cast<T>(int64)).
std::string from_int(const std::string& big, ir::Prim p);
/// Convert a BigInt expression to the value of field f's element type (enum / scalar).
std::string from_int(const std::string& big, const ir::Field& f);

/// gmsg checksum algorithm expression for a checksum spec (preset, inline CRC parameters or custom function).
/// `is_function` is set for custom checksums (called as fn(bytes)).
std::string checksum_algo(Ctx& c, const ir::ChecksumSpec& cs, bool& is_function);
/// BigInt checksum of bytes[start, end) (all strings are JS expressions).
std::string checksum_value(Ctx& c, const ir::ChecksumSpec& cs, const std::string& bytes, const std::string& start,
                           const std::string& end);

/// Statements printing value expression `v` of a single (non-array) field element with printer `p`.
std::string print_value(Ctx& c, const ir::Message& m, const ir::Field& f, const std::string& v);
/// Unit and conversions after a scalar value.
void emit_print_extras(CodeWriter& w, const ir::Field& f, const std::string& value);
bool has_print_extras(const ir::Field& f);

void doc(CodeWriter& w, const std::string& text);
bool is_derived_length(const ir::Field& f);

// Emitters (emit_*.cpp)
void emit_binary_message(Ctx& c, const ir::Message& m);
void emit_text_message(Ctx& c, const ir::Message& m);
void emit_protocol(Ctx& c, const ir::Protocol& p);

} // namespace gmpc::js
