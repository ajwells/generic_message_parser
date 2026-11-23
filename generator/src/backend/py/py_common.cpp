#include "backend/py/py_common.hpp"

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace gmpc::py {

using namespace ir;

const std::string& Modules::var(const std::string& module) {
    auto it = vars.find(module);
    if (it != vars.end()) return it->second;
    order.push_back(module);
    return vars[module] = "_ext" + std::to_string(order.size() - 1);
}

const std::string& Consts::name(const std::string& init, const std::string& prefix) {
    auto it = names.find(init);
    if (it != names.end()) return it->second;
    const auto n = prefix + std::to_string(order.size());
    order.emplace_back(n, init);
    return names[init] = n;
}

namespace {

const std::set<std::string>& python_keywords() {
    static const std::set<std::string> k = {
        "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del",
        "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda",
        "nonlocal", "not", "or", "pass", "raise", "return", "try", "while", "with", "yield",
    };
    return k;
}

} // namespace

std::string ident(const std::string& name) {
    static const std::set<std::string> reserved = {
        // the C++ backend's list (keywords and generated members), so member names are the same in both languages
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr",
        "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
        "read", "write", "decode", "encode", "from", "unchecked", "data", "wire_size", "min_wire_size",
        "max_wire_size", "to_struct", "token", "pack", "unpack", "check", "read_text", "write_text",
        "decode_text", "encode_text", "raw", "present_tokens", "print_to", "extra_bytes", "extra_tokens",
        // Python: members of generated classes
        "message_name", "decode_with_extra", "view", "ranges", "to_text", "from_bytes", "from_text", "token_count",
        "separator", "fields",
    };
    return reserved.contains(name) || python_keywords().contains(name) ? name + "_" : name;
}

std::string enum_ident(const std::string& name) {
    static const std::set<std::string> reserved = {"name", "value", "of", "name_of", "is_known", "mro"};
    return reserved.contains(name) || python_keywords().contains(name) || name.starts_with("_") ? name + "_" : name;
}

bool reserved_type_name(const std::string& name) {
    static const std::set<std::string> names = {
        // builtins and names the generated module uses
        "int", "float", "bool", "str", "bytes", "bytearray", "memoryview", "len", "range", "list", "tuple", "dict",
        "set", "isinstance", "property", "staticmethod", "classmethod", "object", "chr", "ord", "min", "max", "type",
        "super", "print", "Exception", "gmsg", "Err", "annotations", "_dataclass", "_field", "_ClassVar", "_struct",
    };
    return names.contains(name) || python_keywords().contains(name) || name.starts_with("_ext") || name.starts_with("_st") ||
           name.starts_with("_crc");
}

std::string prim_id(Prim p) {
    switch (p) {
        case Prim::U8: return "'u8'";
        case Prim::U16: return "'u16'";
        case Prim::U32: return "'u32'";
        case Prim::U64: return "'u64'";
        case Prim::I8: return "'i8'";
        case Prim::I16: return "'i16'";
        case Prim::I32: return "'i32'";
        case Prim::I64: return "'i64'";
        case Prim::F32: return "'f32'";
        case Prim::F64: return "'f64'";
        case Prim::Bool: return "'bool'";
        case Prim::Char: return "'char'";
    }
    return "'u8'";
}

std::string endian(Endian e) { return e == Endian::Big ? "'big'" : "'little'"; }
std::string bit_order(BitOrder o) { return o == BitOrder::MsbFirst ? "'msb_first'" : "'lsb_first'"; }
std::string view_name(const Message& m) { return m.name + "View"; }
std::string bits_type_name(const Message& m, const Field& f) { return m.name + "_" + f.name; }

std::string string_literal(const std::string& s) {
    std::string out = "'";
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\x%02x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "'";
}

std::string bytes_literal(const std::vector<std::uint8_t>& v) {
    std::string out = "b'";
    for (auto c : v) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\x%02x", c);
        out += buf;
    }
    return out + "'";
}

std::string double_literal(double v) {
    if (std::isnan(v)) return "float('nan')";
    if (std::isinf(v)) return v < 0 ? "float('-inf')" : "float('inf')";
    std::ostringstream os;
    os << std::setprecision(17) << v;
    auto s = os.str();
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

std::string int_literal(std::int64_t v, Prim p) {
    const auto u = static_cast<std::uint64_t>(v);
    switch (p) {
        case Prim::U8: return std::to_string(static_cast<std::uint8_t>(u));
        case Prim::U16: return std::to_string(static_cast<std::uint16_t>(u));
        case Prim::U32: return std::to_string(static_cast<std::uint32_t>(u));
        case Prim::U64: return std::to_string(u);
        case Prim::I8: return std::to_string(static_cast<std::int8_t>(u));
        case Prim::I16: return std::to_string(static_cast<std::int16_t>(u));
        case Prim::I32: return std::to_string(static_cast<std::int32_t>(u));
        case Prim::I64: return std::to_string(v);
        case Prim::F32: return double_literal(static_cast<double>(static_cast<float>(v)));
        case Prim::F64: return double_literal(static_cast<double>(v));
        case Prim::Bool: return v ? "True" : "False";
        case Prim::Char: return string_literal(std::string(1, static_cast<char>(v)));
    }
    return std::to_string(v);
}

Prim storage_prim(const Field& f) { return f.kind == FieldKind::Enum && f.enum_def ? f.enum_def->underlying : f.prim; }

std::string typed_literal(const Field& f, std::int64_t v) { return int_literal(v, storage_prim(f)); }

std::string enum_of(const Field& f, const std::string& v) {
    if (f.kind == FieldKind::Enum && f.enum_def) return f.enum_def->name + ".of(" + v + ")";
    return v;
}

std::string limit_fail(const std::vector<LimitCheck>& checks, const std::string& v, Prim prim, std::optional<double> scale,
                       std::optional<double> offset) {
    std::string out;
    for (const auto& c : checks) {
        std::string t;
        if (c.integer) {
            const auto& l = c.limit;
            t = v + (c.is_min ? " < " : " > ") + (l.negative ? "-" : "") + std::to_string(l.magnitude);
        } else {
            std::string d = prim == Prim::F32 ? "gmsg.to_f32(" + v + ")" : prim == Prim::F64 ? v : "float(" + v + ")";
            if (c.scaled) d += " * " + double_literal(scale.value_or(1.0)) + " + " + double_literal(offset.value_or(0.0));
            t = "not (" + d + (c.is_min ? " >= " : " <= ") + double_literal(c.limit.value) + ")";  // NaN fails
        }
        out += (out.empty() ? "" : " or ") + t;
    }
    return out;
}

std::string lang_value(const LangBlocks& lang, const std::string& key) {
    auto it = lang.find("python");
    if (it == lang.end()) return {};
    auto k = it->second.find(key);
    return k == it->second.end() ? std::string() : k->second;
}

std::string ext_fn(Ctx& c, const LangBlocks& lang, const std::string& key) {
    const auto fn = lang_value(lang, key);
    if (fn.empty()) return {};
    return c.modules.var(lang_value(lang, "module")) + "." + fn;
}

namespace {

/// to_int of a constant of type p: the int64 the C++ code compares with.
std::int64_t to_int_of(std::int64_t v, Prim p) {
    const auto u = static_cast<std::uint64_t>(v);
    switch (p) {
        case Prim::U8: return static_cast<std::uint8_t>(u);
        case Prim::U16: return static_cast<std::uint16_t>(u);
        case Prim::U32: return static_cast<std::uint32_t>(u);
        case Prim::I8: return static_cast<std::int8_t>(u);
        case Prim::I16: return static_cast<std::int16_t>(u);
        case Prim::I32: return static_cast<std::int32_t>(u);
        case Prim::Bool: return v != 0;
        default: return v;
    }
}

} // namespace

std::string expr(const Expr& e, const RefFn& ref) {
    switch (e.kind) {
        case Expr::Kind::Int: return std::to_string(e.value);
        case Expr::Kind::Ref: return "gmsg.to_int(" + ref(*e.field, e.bitfield) + ")";
        case Expr::Kind::EnumConst: {
            const auto v = e.enum_def->find(e.path[1]);
            return std::to_string(to_int_of(v.value_or(0), e.enum_def->underlying));
        }
        case Expr::Kind::Input: return "value";
        case Expr::Kind::Unary: {
            const auto x = expr(*e.lhs, ref);
            if (e.op == "!") return "(1 if " + x + " == 0 else 0)";
            if (e.op == "-") return "gmsg.i64(-" + x + ")";
            return "(~" + x + ")";
        }
        case Expr::Kind::Binary: {
            const auto a = expr(*e.lhs, ref), b = expr(*e.rhs, ref);
            const auto& op = e.op;
            if (op == "*" || op == "+" || op == "-" || op == "<<") return "gmsg.i64(" + a + " " + op + " " + b + ")";
            if (op == "/") return "gmsg.idiv(" + a + ", " + b + ")";
            if (op == "%") return "gmsg.imod(" + a + ", " + b + ")";
            if (op == ">>" || op == "&" || op == "|" || op == "^") return "(" + a + " " + op + " " + b + ")";
            if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=")
                return "(1 if " + a + " " + op + " " + b + " else 0)";
            if (op == "&&") return "(1 if " + a + " != 0 and " + b + " != 0 else 0)";
            if (op == "||") return "(1 if " + a + " != 0 or " + b + " != 0 else 0)";
            return "0";
        }
    }
    return "0";
}

std::string condition(const Expr& e, const RefFn& ref) {
    if (e.kind == Expr::Kind::Unary && e.op == "!") return "not (" + condition(*e.lhs, ref) + ")";
    if (e.kind == Expr::Kind::Binary) {
        const auto& op = e.op;
        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=")
            return expr(*e.lhs, ref) + " " + op + " " + expr(*e.rhs, ref);
        if (op == "&&" || op == "||")
            return "(" + condition(*e.lhs, ref) + (op == "&&" ? " and " : " or ") + condition(*e.rhs, ref) + ")";
    }
    return expr(e, ref) + " != 0";
}

std::string from_int(const std::string& v, Prim p) {
    switch (p) {
        case Prim::F32: return "gmsg.to_f32(" + v + ")";
        case Prim::F64: return "float(" + v + ")";
        case Prim::Bool: return "(" + v + " != 0)";
        case Prim::Char: return "chr(gmsg.wrap_int(" + v + ", 'u8'))";
        default: return "gmsg.wrap_int(" + v + ", " + prim_id(p) + ")";
    }
}

std::string from_int(const std::string& v, const Field& f) { return enum_of(f, from_int(v, storage_prim(f))); }

std::string checksum_algo(Ctx& c, const ChecksumSpec& cs, bool& is_function) {
    is_function = false;
    if (cs.custom) {
        is_function = true;
        return ext_fn(c, cs.custom->lang, "function");
    }
    if (cs.crc) {
        const auto& p = *cs.crc;
        return c.consts.name("gmsg.checksum.crc(" + std::to_string(p.width) + ", " + std::to_string(p.poly) + ", " +
                                 std::to_string(p.init) + ", " + (p.refin ? "True" : "False") + ", " +
                                 (p.refout ? "True" : "False") + ", " + std::to_string(p.xorout) + ")",
                             "_crc");
    }
    return "gmsg.checksum." + cs.algo;
}

std::string checksum_value(Ctx& c, const ChecksumSpec& cs, const std::string& data, const std::string& start,
                           const std::string& end) {
    bool fn = false;
    const auto algo = checksum_algo(c, cs, fn);
    if (fn) return "gmsg.wrap_int(" + algo + "(" + data + "[" + start + ":" + end + "]), 'u64')";
    return algo + ".compute(" + data + ", " + start + ", " + end + ")";
}

std::string to_double(const Field& f, const std::string& v) {
    switch (storage_prim(f)) {
        case Prim::F32: return "gmsg.to_f32(" + v + ")";
        case Prim::F64: return "float(" + v + ")";
        case Prim::Bool: return "(1.0 if " + v + " else 0.0)";
        case Prim::Char: return "float(gmsg.to_int(" + v + "))";
        default: return "float(" + v + ")";
    }
}

std::string print_value(Ctx& c, const Message& m, const Field& f, const std::string& v) {
    switch (f.kind) {
        case FieldKind::Scalar:
            switch (f.prim) {
                case Prim::F32: return "p.float_value(gmsg.to_f32(" + v + "), True)";
                case Prim::F64: return "p.float_value(" + v + ")";
                case Prim::Bool: return "p.bool_value(" + v + ")";
                case Prim::Char: return "p.char_value(" + v + ")";
                default: return "p.int_value(" + v + ")";
            }
        case FieldKind::Enum:
            return "p.enum_value(" + f.enum_def->name + ".name_of(" + v + "), " + v + ", " +
                   std::to_string(prim_size(f.enum_def->underlying)) + ")";
        case FieldKind::Bits:
        case FieldKind::Message: return v + ".print_to(p)";
        case FieldKind::Bytes: return "p.hex_bytes(" + v + ")";
        case FieldKind::String: return "p.str_value(" + v + ")";
        case FieldKind::Custom: {
            const auto hook = ext_fn(c, f.custom->lang, "print");
            // without a hook: numbers and strings print as such (as C++ prints a scalar custom type), else <?>
            return hook.empty() ? "p.value(" + v + ")" : hook + "(p, " + v + ")";
        }
        case FieldKind::Array: {
            const auto& e = *f.elem;
            const bool blocks = e.kind == FieldKind::Message;
            return "p.list_value(" + v + ", lambda x: " + print_value(c, m, e, "x") + (blocks ? ", True" : "") + ")";
        }
        default: return "p.raw_text('<?>')";
    }
}

void emit_print_extras(CodeWriter& w, const Field& f, const std::string& value) {
    if (f.kind != FieldKind::Scalar) return;
    const bool scaled = f.scale || f.offset_value;
    if (!f.unit.empty() && !scaled) w.line("p.unit(" + string_literal(f.unit) + ")");
    auto conv = [&](double sc, double of, const std::string& unit) {
        w.line("p.conversion(" + to_double(f, value) + " * " + double_literal(sc) + " + " + double_literal(of) +
               (unit.empty() ? "" : ", " + string_literal(unit)) + ")");
    };
    if (scaled) conv(f.scale.value_or(1.0), f.offset_value.value_or(0.0), f.unit);
    for (const auto& cv : f.conversions) conv(cv.scale.value_or(1.0), cv.offset_value.value_or(0.0), cv.unit);
}

void doc(CodeWriter& w, const std::string& text) {
    if (text.empty()) return;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) w.line(line.empty() ? "#" : "# " + line);
}

bool is_derived_length(const Field& f) {
    if (f.len.kind != LenKind::Expr || !f.len.expr) return false;
    const auto& e = *f.len.expr;
    return e.kind == Expr::Kind::Ref && e.field && !e.bitfield && e.field->derived_from == &f;
}

} // namespace gmpc::py
