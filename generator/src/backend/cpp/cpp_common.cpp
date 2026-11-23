#include "backend/cpp/cpp_common.hpp"

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <set>
#include <sstream>

namespace gmpc::cpp {

using namespace ir;

std::string ident(const std::string& name) {
    static const std::set<std::string> reserved = {
        // C++ keywords
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr",
        "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
        // names used by generated members
        "read", "write", "decode", "encode", "from", "unchecked", "data", "wire_size", "min_wire_size",
        "max_wire_size", "to_struct", "token", "pack", "unpack", "check", "read_text", "write_text",
        "decode_text", "encode_text", "raw", "present_tokens", "print_to", "extra_bytes", "extra_tokens",
    };
    return reserved.contains(name) ? name + "_" : name;
}

std::string prim_type(Prim p) {
    switch (p) {
        case Prim::U8: return "std::uint8_t";
        case Prim::U16: return "std::uint16_t";
        case Prim::U32: return "std::uint32_t";
        case Prim::U64: return "std::uint64_t";
        case Prim::I8: return "std::int8_t";
        case Prim::I16: return "std::int16_t";
        case Prim::I32: return "std::int32_t";
        case Prim::I64: return "std::int64_t";
        case Prim::F32: return "float";
        case Prim::F64: return "double";
        case Prim::Bool: return "bool";
        case Prim::Char: return "char";
    }
    return "void";
}

std::string endian_expr(Endian e) { return e == Endian::Big ? "gmp::Endian::big" : "gmp::Endian::little"; }

std::string bit_order_expr(BitOrder o) {
    return o == BitOrder::MsbFirst ? "gmp::BitOrder::msb_first" : "gmp::BitOrder::lsb_first";
}

std::string view_name(const Message& m) { return m.name + "View"; }

std::string bits_type_name(const Message& m, const Field& f) { return m.name + "_" + f.name; }

std::string lang_value(const LangBlocks& lang, const std::string& key) {
    auto it = lang.find("cpp");
    if (it == lang.end()) return {};
    auto k = it->second.find(key);
    return k == it->second.end() ? std::string() : k->second;
}

std::string element_type(const Message& m, const Field& f) {
    switch (f.kind) {
        case FieldKind::Scalar: return prim_type(f.prim);
        case FieldKind::Enum: return f.enum_def->name;
        case FieldKind::Message: return f.message->name;
        case FieldKind::Custom: return lang_value(f.custom->lang, "type");
        case FieldKind::Bits: return bits_type_name(m, f);
        default: return "void";
    }
}

std::string value_type(const Message& m, const Field& f) {
    switch (f.kind) {
        case FieldKind::Bytes:
            if (f.len.kind == LenKind::Fixed) return "std::array<std::uint8_t, " + std::to_string(f.len.fixed) + ">";
            return "gmp::static_vector<std::uint8_t, " + std::to_string(f.max.value_or(0)) + ">";
        case FieldKind::String:
            if (f.len.kind == LenKind::Fixed) return "gmp::static_string<" + std::to_string(f.len.fixed) + ">";
            return "gmp::static_string<" + std::to_string(f.max.value_or(0)) + ">";
        case FieldKind::Array: {
            const auto et = element_type(m, *f.elem);
            if (f.len.kind == LenKind::Fixed) return "std::array<" + et + ", " + std::to_string(f.len.fixed) + ">";
            return "gmp::static_vector<" + et + ", " + std::to_string(f.max.value_or(0)) + ">";
        }
        default: return element_type(m, f);
    }
}

std::string member_type(const Message& m, const Field& f) {
    const auto t = value_type(m, f);
    if (f.when || f.optional) return "std::optional<" + t + ">";
    return t;
}

std::string int_literal(std::int64_t v) {
    if (v == INT64_MIN) return "INT64_MIN";
    return std::to_string(v);
}

std::string typed_literal(const Field& f, std::int64_t v) {
    if (f.kind == FieldKind::Enum) return "static_cast<" + f.enum_def->name + ">(" + int_literal(v) + ")";
    if (f.prim == Prim::Bool) return v ? "true" : "false";
    return "static_cast<" + prim_type(f.prim) + ">(" + int_literal(v) + ")";
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
            // exact: compare as 64-bit integers of the value's signedness
            const auto& l = c.limit;
            const std::string lit = prim_is_signed(prim)
                                        ? (l.negative ? (l.magnitude == (std::uint64_t{1} << 63) ? std::string("INT64_MIN")
                                                                                                  : "-" + std::to_string(l.magnitude) + "ll")
                                                      : std::to_string(l.magnitude) + "ll")
                                        : std::to_string(l.magnitude) + "ull";
            t = std::string(prim_is_signed(prim) ? "static_cast<std::int64_t>(" : "static_cast<std::uint64_t>(") + v + ") " +
                (c.is_min ? "<" : ">") + " " + lit;
        } else {
            std::string d = "static_cast<double>(" + v + ")";
            if (c.scaled) d += " * " + double_literal(scale.value_or(1.0)) + " + " + double_literal(offset.value_or(0.0));
            t = "!(" + d + (c.is_min ? " >= " : " <= ") + double_literal(c.limit.value) + ")";  // NaN fails
        }
        out += (out.empty() ? "" : " || ") + t;
    }
    return out;
}

std::string string_literal(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\x%02X\"\"", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

std::string expr(const Expr& e, const RefFn& ref) {
    switch (e.kind) {
        case Expr::Kind::Int: return "std::int64_t(" + int_literal(e.value) + ")";
        case Expr::Kind::Ref: return "gmp::to_int(" + ref(*e.field, e.bitfield) + ")";
        case Expr::Kind::EnumConst: return "gmp::to_int(" + e.enum_def->name + "::" + ident(e.path[1]) + ")";
        case Expr::Kind::Input: return "value";  // computed setter parameter (as std::int64_t)
        case Expr::Kind::Unary: return "(" + e.op + expr(*e.lhs, ref) + ")";
        case Expr::Kind::Binary: return "(" + expr(*e.lhs, ref) + " " + e.op + " " + expr(*e.rhs, ref) + ")";
    }
    return "0";
}

std::string checksum_impl(const ChecksumSpec& c, bool& is_function) {
    is_function = false;
    if (c.custom) {
        is_function = true;
        return lang_value(c.custom->lang, "function");
    }
    if (c.crc) {
        const auto& p = *c.crc;
        const std::string t = p.width <= 8 ? "std::uint8_t" : p.width <= 16 ? "std::uint16_t" : p.width <= 32 ? "std::uint32_t" : "std::uint64_t";
        auto hex = [](std::uint64_t v) {
            std::ostringstream os;
            os << "0x" << std::hex << std::uppercase << v << "ull";
            return os.str();
        };
        return "gmp::checksum::Crc<" + t + ", " + std::to_string(p.width) + ", " + hex(p.poly) + ", " + hex(p.init) +
               ", " + (p.refin ? "true" : "false") + ", " + (p.refout ? "true" : "false") + ", " + hex(p.xorout) + ">";
    }
    return "gmp::checksum::" + c.algo;
}

void doc(CodeWriter& w, const std::string& text) {
    if (text.empty()) return;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) w.line("/// " + line);
}

bool is_derived_length(const Field& f) {
    if (f.len.kind != LenKind::Expr || !f.len.expr) return false;
    const auto& e = *f.len.expr;
    return e.kind == Expr::Kind::Ref && e.field && !e.bitfield && e.field->derived_from == &f;
}

} // namespace gmpc::cpp

namespace gmpc::cpp {

void emit_print_extras(CodeWriter& w, const Field& f, const std::string& value) {
    if (f.kind != FieldKind::Scalar) return;
    const bool scaled = f.scale || f.offset_value;
    if (!f.unit.empty() && !scaled) w.line("gmp_p.unit(" + string_literal(f.unit) + ");");
    auto conv = [&](double sc, double of, const std::string& unit) {
        w.line("gmp_p.conversion(static_cast<double>(" + value + ") * " + double_literal(sc) + " + " + double_literal(of) +
               (unit.empty() ? "" : ", " + string_literal(unit)) + ");");
    };
    if (scaled) conv(f.scale.value_or(1.0), f.offset_value.value_or(0.0), f.unit);
    for (const auto& c : f.conversions) conv(c.scale.value_or(1.0), c.offset_value.value_or(0.0), c.unit);
}

bool has_print_extras(const Field& f) {
    return f.kind == FieldKind::Scalar && (!f.unit.empty() || f.scale || f.offset_value || !f.conversions.empty());
}

} // namespace gmpc::cpp
