#pragma once
// Shared helpers for the C backend: naming, type mapping, literals, expressions.
//
// Naming scheme (prefix P from --opt prefix=..., else the schema namespace with '::' -> '_', else the file stem):
//   message Status        -> type  P_status, view P_status_view, functions P_status_decode(), P_status_view_from()
//   field   voltage       -> member m.voltage; view accessor P_status_view_voltage(&v)
//   enum    Mode.fault    -> typedef P_mode, constant P_MODE_FAULT
//   bits    Status.flags  -> type  P_status_flags
//   macros                -> P_STATUS_WIRE_SIZE, P_STATUS_MIN_WIRE_SIZE, P_STATUS_MAGIC_VALUE

#include <functional>
#include <map>
#include <string>

#include "backend/code_writer.hpp"
#include "diagnostics.hpp"
#include "ir/ir.hpp"

namespace gmpc::c {

struct Names {
    std::string prefix;  // e.g. "acme_telemetry_"
    std::string macro;   // e.g. "ACME_TELEMETRY_"
};

struct Ctx {
    const ir::Schema& schema;
    Diagnostics& diag;
    Names names;
    CodeWriter& h;  // header (.gmp.h)
    CodeWriter& c;  // source (.gmp.c)
};

/// CamelCase / mixedCase -> snake_case ("BinaryLinkHeader" -> "binary_link_header", "HDLCFrame" -> "hdlc_frame").
std::string snake(const std::string& name);
std::string upper(const std::string& s);

/// Identifier safe in C and C++ (keywords get a trailing underscore).
std::string ident(const std::string& name);

std::string type_name(const Ctx& c, const ir::Message& m);       // P_status
std::string view_name(const Ctx& c, const ir::Message& m);       // P_status_view
std::string enum_name(const Ctx& c, const ir::EnumDef& e);       // P_mode
std::string enum_const(const Ctx& c, const ir::EnumDef& e, const std::string& value);  // P_MODE_FAULT
std::string bits_name(const Ctx& c, const ir::Message& m, const ir::Field& f);          // P_status_flags
std::string macro_name(const Ctx& c, const ir::Message& m);      // P_STATUS

std::string prim_type(ir::Prim p);
/// Suffix of the runtime load/store/read/write functions (u8 .. f64, bool). Char uses u8.
std::string prim_suffix(ir::Prim p);
std::string endian_expr(ir::Endian e);
std::string bit_order_expr(ir::BitOrder o);
/// Largest value of an integer type, as a C expression (for length/count overflow checks).
std::string prim_max(ir::Prim p);

/// Type of a single value (Scalar/Enum/Message/Bits).
std::string element_type(const Ctx& c, const ir::Message& m, const ir::Field& f);
/// Smallest unsigned type that holds 0..n.
std::string count_type(std::size_t n);

std::string int_literal(std::int64_t v);
std::string typed_literal(const Ctx& c, const ir::Field& f, std::int64_t v);
std::string double_literal(double v);
/// Condition that is true when value `v` (an expression without side effects) fails one of `checks`; "" when there
/// are none. The scaled value is computed exactly as the _scaled() accessor.
std::string limit_fail(const std::vector<ir::LimitCheck>& checks, const std::string& v, ir::Prim prim,
                       std::optional<double> scale, std::optional<double> offset);
std::string string_literal(const std::string& s);

/// Emits a reference to a field (and optional bit field) in some context.
using RefFn = std::function<std::string(const ir::Field&, const ir::BitField*)>;
std::string expr(const Ctx& c, const ir::Expr& e, const RefFn& ref);

/// C expression computing the checksum `spec` over pointer `p` and length `n` (as uint64_t).
std::string checksum_call(const ir::ChecksumSpec& spec, const std::string& p, const std::string& n);

/// Language block value: custom->lang["c"][key]; empty if missing.
std::string lang_value(const ir::LangBlocks& lang, const std::string& key);

/// "/// text" doc comment lines (nothing if empty).
void doc(CodeWriter& w, const std::string& text);

/// True if the length/count expression of `f` is a bare reference whose field is derived from `f`.
bool is_derived_length(const ir::Field& f);

/// Registry of the generated member / function names of one message, so they never collide.
class NameSet {
public:
    NameSet(Diagnostics& diag, const SourceLoc& loc, std::string what) : diag_(diag), loc_(loc), what_(std::move(what)) {}
    void add(const std::string& name, const std::string& origin) {
        auto [it, inserted] = names_.emplace(name, origin);
        if (!inserted)
            diag_.error(loc_, what_ + ": generated C name '" + name + "' (" + origin + ") clashes with '" + it->second +
                                  "'; rename one of them");
    }

private:
    Diagnostics& diag_;
    SourceLoc loc_;
    std::string what_;
    std::map<std::string, std::string> names_;
};

// ---- print (gmp/c/print.h); generated print functions take the printer as `p` ----

/// Statement printing value `v` of primitive type `prim` (gmsg_print_u64 / _i64 / _bool / _char / _f32 / _f64).
std::string print_prim(ir::Prim prim, const std::string& v);
/// Statement printing enum value `v`: `name (0x01)`.
std::string print_enum(const Ctx& c, const ir::EnumDef& e, const std::string& v);
/// Statement(s) printing the custom value `*ptr`: the `c: { print: fn }` function, a built-in printer when the C type
/// is a standard scalar type (double, uint16_t, ...; as C++ prints such types), otherwise `<?>`.
void print_custom(CodeWriter& w, const ir::CustomType& t, const std::string& ptr);
/// True when field `f` prints a unit or conversions after its value.
bool has_print_extras(const ir::Field& f);
/// Statements printing the unit / conversions of scalar field `f` after its value `v`.
void print_extras(CodeWriter& w, const ir::Field& f, const std::string& v);

void emit_binary_message(Ctx& c, const ir::Message& m);
void emit_text_message(Ctx& c, const ir::Message& m);
/// Binary protocols with built-in framing and checksums (others are skipped by the backend for now).
bool protocol_supported(const ir::Protocol& p);
void emit_protocol(Ctx& c, const ir::Protocol& p);

} // namespace gmpc::c
