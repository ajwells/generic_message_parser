#include "backend/c/c_common.hpp"

#include <cctype>
#include <cstdio>
#include <iomanip>
#include <set>
#include <sstream>

namespace gmpc::c {

using namespace ir;

std::string snake(const std::string& name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char ch = name[i];
        const bool up = std::isupper(static_cast<unsigned char>(ch)) != 0;
        if (up && i > 0) {
            const char prev = name[i - 1];
            const bool prev_lower_or_digit = std::islower(static_cast<unsigned char>(prev)) || std::isdigit(static_cast<unsigned char>(prev));
            const bool next_lower = i + 1 < name.size() && std::islower(static_cast<unsigned char>(name[i + 1]));
            const bool prev_upper = std::isupper(static_cast<unsigned char>(prev)) != 0;
            if (prev != '_' && (prev_lower_or_digit || (prev_upper && next_lower))) out += '_';
        }
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string upper(const std::string& s) {
    std::string out = s;
    for (auto& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return out;
}

std::string ident(const std::string& name) {
    // C11 and C++ keywords (generated headers are also compiled as C++), plus common macros.
    static const std::set<std::string> reserved = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern",
        "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short", "signed",
        "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
        "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary", "_Noreturn",
        "_Static_assert", "_Thread_local", "bool", "true", "false", "alignas", "alignof", "and", "and_eq", "asm",
        "bitand", "bitor", "catch", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "consteval",
        "constexpr", "constinit", "const_cast", "co_await", "co_return", "co_yield", "decltype", "delete",
        "dynamic_cast", "explicit", "export", "friend", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "reinterpret_cast", "requires",
        "static_assert", "static_cast", "template", "this", "thread_local", "throw", "try", "typeid", "typename",
        "using", "virtual", "wchar_t", "xor", "xor_eq", "assert", "errno", "NULL",
    };
    return reserved.contains(name) ? name + "_" : name;
}

std::string type_name(const Ctx& c, const Message& m) { return c.names.prefix + snake(m.name); }
std::string view_name(const Ctx& c, const Message& m) { return type_name(c, m) + "_view"; }
std::string enum_name(const Ctx& c, const EnumDef& e) { return c.names.prefix + snake(e.name); }
std::string enum_const(const Ctx& c, const EnumDef& e, const std::string& value) {
    return c.names.macro + upper(snake(e.name)) + "_" + upper(value);
}
std::string bits_name(const Ctx& c, const Message& m, const Field& f) { return type_name(c, m) + "_" + f.name; }
std::string macro_name(const Ctx& c, const Message& m) { return c.names.macro + upper(snake(m.name)); }

std::string prim_type(Prim p) {
    switch (p) {
        case Prim::U8: return "uint8_t";
        case Prim::U16: return "uint16_t";
        case Prim::U32: return "uint32_t";
        case Prim::U64: return "uint64_t";
        case Prim::I8: return "int8_t";
        case Prim::I16: return "int16_t";
        case Prim::I32: return "int32_t";
        case Prim::I64: return "int64_t";
        case Prim::F32: return "float";
        case Prim::F64: return "double";
        case Prim::Bool: return "bool";
        case Prim::Char: return "char";
    }
    return "void";
}

std::string prim_suffix(Prim p) {
    switch (p) {
        case Prim::U8: case Prim::Char: return "u8";
        case Prim::U16: return "u16";
        case Prim::U32: return "u32";
        case Prim::U64: return "u64";
        case Prim::I8: return "i8";
        case Prim::I16: return "i16";
        case Prim::I32: return "i32";
        case Prim::I64: return "i64";
        case Prim::F32: return "f32";
        case Prim::F64: return "f64";
        case Prim::Bool: return "bool";
    }
    return "u8";
}

std::string endian_expr(Endian e) { return e == Endian::Big ? "GMSG_ENDIAN_BIG" : "GMSG_ENDIAN_LITTLE"; }
std::string bit_order_expr(BitOrder o) { return o == BitOrder::MsbFirst ? "GMSG_MSB_FIRST" : "GMSG_LSB_FIRST"; }

std::string prim_max(Prim p) {
    switch (p) {
        case Prim::U8: return "UINT8_MAX";
        case Prim::U16: return "UINT16_MAX";
        case Prim::U32: return "UINT32_MAX";
        case Prim::U64: return "UINT64_MAX";
        case Prim::I8: return "INT8_MAX";
        case Prim::I16: return "INT16_MAX";
        case Prim::I32: return "INT32_MAX";
        case Prim::I64: return "INT64_MAX";
        default: return "0";
    }
}

std::string lang_value(const LangBlocks& lang, const std::string& key) {
    auto it = lang.find("c");
    if (it == lang.end()) return {};
    auto k = it->second.find(key);
    return k == it->second.end() ? std::string() : k->second;
}

std::string print_prim(Prim prim, const std::string& v) {
    switch (prim) {
        case Prim::Bool: return "gmsg_print_bool(p, " + v + ");";
        case Prim::Char: return "gmsg_print_char(p, " + v + ");";
        case Prim::F32: return "gmsg_print_f32(p, " + v + ");";
        case Prim::F64: return "gmsg_print_f64(p, " + v + ");";
        default: break;
    }
    if (prim_is_signed(prim)) return "gmsg_print_i64(p, (int64_t)" + v + ");";
    return "gmsg_print_u64(p, (uint64_t)" + v + ");";
}

std::string print_enum(const Ctx& c, const EnumDef& e, const std::string& v) {
    return "gmsg_print_enum(p, " + enum_name(c, e) + "_to_string(" + v + "), (uint64_t)" + v + ", " +
           std::to_string(prim_size(e.underlying)) + ");";
}

void print_custom(CodeWriter& w, const CustomType& t, const std::string& ptr) {
    const auto fn = lang_value(t.lang, "print");
    if (!fn.empty()) {
        w.line(fn + "(p, " + ptr + ");");
        return;
    }
    static const std::map<std::string, Prim> builtin = {
        {"double", Prim::F64},   {"float", Prim::F32},     {"bool", Prim::Bool},    {"_Bool", Prim::Bool},
        {"char", Prim::Char},    {"int8_t", Prim::I8},     {"int16_t", Prim::I16},  {"int32_t", Prim::I32},
        {"int64_t", Prim::I64},  {"uint8_t", Prim::U8},    {"uint16_t", Prim::U16}, {"uint32_t", Prim::U32},
        {"uint64_t", Prim::U64}, {"int", Prim::I32},       {"long", Prim::I64},     {"long long", Prim::I64},
        {"short", Prim::I16},    {"unsigned", Prim::U32},  {"unsigned int", Prim::U32}, {"size_t", Prim::U64},
        {"unsigned long", Prim::U64}, {"unsigned long long", Prim::U64}, {"unsigned short", Prim::U16},
    };
    const auto it = builtin.find(lang_value(t.lang, "type"));
    if (it != builtin.end()) w.line(print_prim(it->second, "(*" + ptr + ")"));
    else w.line("gmsg_print_text(p, \"<?>\", 3);");
}

bool has_print_extras(const Field& f) {
    return f.kind == FieldKind::Scalar && (!f.unit.empty() || f.scale || f.offset_value || !f.conversions.empty());
}

void print_extras(CodeWriter& w, const Field& f, const std::string& v) {
    if (f.kind != FieldKind::Scalar) return;
    const bool scaled = f.scale || f.offset_value;
    if (!f.unit.empty() && !scaled) w.line("gmsg_print_unit(p, " + string_literal(f.unit) + ");");
    auto conv = [&](double sc, double of, const std::string& unit) {
        w.line("gmsg_print_conversion(p, (double)" + v + " * " + double_literal(sc) + " + " + double_literal(of) + ", " +
               (unit.empty() ? std::string("NULL") : string_literal(unit)) + ");");
    };
    if (scaled) conv(f.scale.value_or(1.0), f.offset_value.value_or(0.0), f.unit);
    for (const auto& cv : f.conversions) conv(cv.scale.value_or(1.0), cv.offset_value.value_or(0.0), cv.unit);
}

std::string element_type(const Ctx& c, const Message& m, const Field& f) {
    switch (f.kind) {
        case FieldKind::Custom: return lang_value(f.custom->lang, "type");
        case FieldKind::Scalar: return prim_type(f.prim);
        case FieldKind::Enum: return enum_name(c, *f.enum_def);
        case FieldKind::Message: return type_name(c, *f.message);
        case FieldKind::Bits: return bits_name(c, m, f);
        default: return "void";
    }
}

std::string count_type(std::size_t n) {
    if (n <= 0xFF) return "uint8_t";
    if (n <= 0xFFFF) return "uint16_t";
    return "uint32_t";
}

std::string int_literal(std::int64_t v) {
    if (v == INT64_MIN) return "INT64_MIN";
    return "INT64_C(" + std::to_string(v) + ")";
}

std::string typed_literal(const Ctx& c, const Field& f, std::int64_t v) {
    if (f.kind == FieldKind::Enum) return "((" + enum_name(c, *f.enum_def) + ")" + int_literal(v) + ")";
    if (f.prim == Prim::Bool) return v ? "true" : "false";
    return "((" + prim_type(f.prim) + ")" + int_literal(v) + ")";
}

std::string double_literal(double v) {
    std::ostringstream os;
    os << std::setprecision(17) << v;
    auto s = os.str();
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

std::string limit_fail(const std::vector<LimitCheck>& checks, const std::string& v, Prim prim, std::optional<double> scale,
                       std::optional<double> offset) {
    std::string out;
    for (const auto& c : checks) {
        std::string t;
        if (c.integer) {
            const auto& l = c.limit;
            const bool sgn = prim_is_signed(prim);
            const std::string lit = sgn ? (l.negative ? (l.magnitude == (std::uint64_t{1} << 63) ? std::string("INT64_MIN")
                                                                                                : "-INT64_C(" + std::to_string(l.magnitude) + ")")
                                                      : "INT64_C(" + std::to_string(l.magnitude) + ")")
                                        : "UINT64_C(" + std::to_string(l.magnitude) + ")";
            t = std::string(sgn ? "(int64_t)" : "(uint64_t)") + "(" + v + ") " + (c.is_min ? "<" : ">") + " " + lit;
        } else {
            std::string d = "(double)(" + v + ")";
            if (c.scaled) d += " * " + double_literal(scale.value_or(1.0)) + " + " + double_literal(offset.value_or(0.0));
            t = "!(" + d + (c.is_min ? " >= " : " <= ") + double_literal(c.limit.value) + ")";  /* NaN fails */
        }
        out += (out.empty() ? "" : " || ") + t;
    }
    return out;
}

std::string string_literal(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20 || ch >= 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\x%02X\"\"", ch);
                    out += buf;
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out + "\"";
}

std::string expr(const Ctx& c, const Expr& e, const RefFn& ref) {
    switch (e.kind) {
        case Expr::Kind::Int: return int_literal(e.value);
        case Expr::Kind::Ref: return "(int64_t)(" + ref(*e.field, e.bitfield) + ")";
        case Expr::Kind::EnumConst: return "(int64_t)" + enum_const(c, *e.enum_def, e.path[1]);
        case Expr::Kind::Input: return "value";  // computed setter parameter (as int64_t)
        case Expr::Kind::Unary: return "(" + e.op + expr(c, *e.lhs, ref) + ")";
        case Expr::Kind::Binary: return "(" + expr(c, *e.lhs, ref) + " " + e.op + " " + expr(c, *e.rhs, ref) + ")";
    }
    return "0";
}

std::string checksum_call(const ChecksumSpec& spec, const std::string& p, const std::string& n) {
    if (spec.crc) {
        const auto& q = *spec.crc;
        auto hex = [](std::uint64_t v) {
            std::ostringstream os;
            os << "UINT64_C(0x" << std::hex << std::uppercase << v << ")";
            return os.str();
        };
        // Compound literal: only used in the .c file, which is always compiled as C.
        return "gmsg_crc_compute(&(const gmsg_crc_params){" + std::to_string(q.width) + ", " +
               (q.refin ? "true" : "false") + ", " + (q.refout ? "true" : "false") + ", " + hex(q.poly) + ", " +
               hex(q.init) + ", " + hex(q.xorout) + "}, " + p + ", " + n + ")";
    }
    if (spec.custom) return "(uint64_t)" + lang_value(spec.custom->lang, "function") + "(" + p + ", " + n + ")";
    return "(uint64_t)gmsg_" + spec.algo + "_compute(" + p + ", " + n + ")";
}

void doc(CodeWriter& w, const std::string& text) {
    if (text.empty()) return;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) w.line("/* " + line + " */");
}

bool is_derived_length(const Field& f) {
    if (f.len.kind != LenKind::Expr || !f.len.expr) return false;
    const auto& e = *f.len.expr;
    return e.kind == Expr::Kind::Ref && e.field && !e.bitfield && e.field->derived_from == &f;
}

} // namespace gmpc::c
