#include "backend/js/js_common.hpp"

#include <cstdio>
#include <iomanip>
#include <sstream>

namespace gmpc::js {

using namespace ir;

const std::string& Modules::var(const std::string& module) {
    auto it = vars.find(module);
    if (it != vars.end()) return it->second;
    order.push_back(module);
    return vars[module] = "$ext" + std::to_string(order.size() - 1);
}

const std::string& Consts::name(const std::string& init, const std::string& prefix) {
    auto it = names.find(init);
    if (it != names.end()) return it->second;
    const auto n = prefix + std::to_string(order.size());
    order.emplace_back(n, init);
    return names[init] = n;
}

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
        "decode_text", "encode_text", "raw", "present_tokens", "print_to",
        // JavaScript: Object.prototype members and the members of generated classes
        "constructor", "prototype", "__proto__", "toString", "toLocaleString", "valueOf", "toJSON", "hasOwnProperty",
        "isPrototypeOf", "propertyIsEnumerable", "printTo", "toStruct", "wireSize", "messageName", "ranges",
        "readText", "writeText", "decodeText", "encodeText", "presentTokens", "view",
    };
    return reserved.contains(name) ? name + "_" : name;
}

bool reserved_type_name(const std::string& name) {
    static const std::set<std::string> names = {
        // reserved words
        "await", "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do",
        "else", "enum", "export", "extends", "false", "finally", "for", "function", "if", "implements", "import",
        "in", "instanceof", "interface", "let", "new", "null", "package", "private", "protected", "public",
        "return", "static", "super", "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while",
        "with", "yield", "arguments", "eval",
        // globals and names the generated module uses
        "Object", "Array", "Number", "String", "Boolean", "BigInt", "Math", "Uint8Array", "DataView", "ArrayBuffer",
        "Error", "Symbol", "Map", "Set", "JSON", "Function", "Promise", "Reflect", "undefined", "NaN", "Infinity",
        "globalThis", "module", "require", "exports", "gmsg", "root", "factory",
    };
    return names.contains(name);
}

std::string prim_id(Prim p) {
    switch (p) {
        case Prim::U8: return "u8";
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
        case Prim::Char: return "char";
    }
    return "u8";
}

bool is_big(Prim p) { return p == Prim::U64 || p == Prim::I64; }

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
                    std::snprintf(buf, sizeof buf, "\\x%02X", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "'";
}

std::string double_literal(double v) {
    std::ostringstream os;
    os << std::setprecision(17) << v;
    return os.str();
}

std::string big_literal(std::int64_t v) { return std::to_string(v) + "n"; }

std::string int_literal(std::int64_t v, Prim p) {
    const auto u = static_cast<std::uint64_t>(v);
    switch (p) {
        case Prim::U8: return std::to_string(static_cast<std::uint8_t>(u));
        case Prim::U16: return std::to_string(static_cast<std::uint16_t>(u));
        case Prim::U32: return std::to_string(static_cast<std::uint32_t>(u));
        case Prim::U64: return std::to_string(u) + "n";
        case Prim::I8: return std::to_string(static_cast<std::int8_t>(u));
        case Prim::I16: return std::to_string(static_cast<std::int16_t>(u));
        case Prim::I32: return std::to_string(static_cast<std::int32_t>(u));
        case Prim::I64: return std::to_string(v) + "n";
        case Prim::F32: return double_literal(static_cast<double>(static_cast<float>(v)));
        case Prim::F64: return double_literal(static_cast<double>(v));
        case Prim::Bool: return v ? "true" : "false";
        case Prim::Char: return string_literal(std::string(1, static_cast<char>(v)));
    }
    return std::to_string(v);
}

Prim storage_prim(const Field& f) { return f.kind == FieldKind::Enum && f.enum_def ? f.enum_def->underlying : f.prim; }

std::string typed_literal(const Field& f, std::int64_t v) { return int_literal(v, storage_prim(f)); }

std::string lang_value(const LangBlocks& lang, const std::string& key) {
    auto it = lang.find("js");
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
        case Expr::Kind::Int: return big_literal(e.value);
        case Expr::Kind::Ref: return "gmsg.toInt(" + ref(*e.field, e.bitfield) + ")";
        case Expr::Kind::EnumConst: {
            const auto v = e.enum_def->find(e.path[1]);
            return big_literal(to_int_of(v.value_or(0), e.enum_def->underlying));
        }
        case Expr::Kind::Input: return "value";
        case Expr::Kind::Unary: {
            const auto x = expr(*e.lhs, ref);
            if (e.op == "!") return "(" + x + " === 0n ? 1n : 0n)";
            if (e.op == "-") return "BigInt.asIntN(64, -" + x + ")";
            return "(~" + x + ")";
        }
        case Expr::Kind::Binary: {
            const auto a = expr(*e.lhs, ref), b = expr(*e.rhs, ref);
            const auto& op = e.op;
            if (op == "*" || op == "+" || op == "-" || op == "<<") return "BigInt.asIntN(64, " + a + " " + op + " " + b + ")";
            if (op == "/") return "gmsg.idiv(" + a + ", " + b + ")";
            if (op == "%") return "gmsg.imod(" + a + ", " + b + ")";
            if (op == ">>" || op == "&" || op == "|" || op == "^") return "(" + a + " " + op + " " + b + ")";
            if (op == "==") return "(" + a + " === " + b + " ? 1n : 0n)";
            if (op == "!=") return "(" + a + " !== " + b + " ? 1n : 0n)";
            if (op == "<" || op == "<=" || op == ">" || op == ">=") return "(" + a + " " + op + " " + b + " ? 1n : 0n)";
            if (op == "&&" || op == "||") return "(" + a + " !== 0n " + op + " " + b + " !== 0n ? 1n : 0n)";
            return "0n";
        }
    }
    return "0n";
}

std::string from_int(const std::string& big, Prim p) {
    switch (p) {
        case Prim::U8: return "Number(BigInt.asUintN(8, " + big + "))";
        case Prim::U16: return "Number(BigInt.asUintN(16, " + big + "))";
        case Prim::U32: return "Number(BigInt.asUintN(32, " + big + "))";
        case Prim::U64: return "BigInt.asUintN(64, " + big + ")";
        case Prim::I8: return "Number(BigInt.asIntN(8, " + big + "))";
        case Prim::I16: return "Number(BigInt.asIntN(16, " + big + "))";
        case Prim::I32: return "Number(BigInt.asIntN(32, " + big + "))";
        case Prim::I64: return "BigInt.asIntN(64, " + big + ")";
        case Prim::F32: return "Math.fround(Number(" + big + "))";
        case Prim::F64: return "Number(" + big + ")";
        case Prim::Bool: return "(" + big + " !== 0n)";
        case Prim::Char: return "String.fromCharCode(Number(BigInt.asUintN(8, " + big + ")))";
    }
    return big;
}

std::string from_int(const std::string& big, const Field& f) { return from_int(big, storage_prim(f)); }

std::string checksum_algo(Ctx& c, const ChecksumSpec& cs, bool& is_function) {
    is_function = false;
    if (cs.custom) {
        is_function = true;
        return ext_fn(c, cs.custom->lang, "function");
    }
    if (cs.crc) {
        const auto& p = *cs.crc;
        auto num = [&](std::uint64_t v) { return p.width > 32 ? std::to_string(v) + "n" : std::to_string(v); };
        return c.consts.name("gmsg.checksum.crc({ width: " + std::to_string(p.width) + ", poly: " + num(p.poly) +
                                 ", init: " + num(p.init) + ", refin: " + (p.refin ? "true" : "false") +
                                 ", refout: " + (p.refout ? "true" : "false") + ", xorout: " + num(p.xorout) + " })",
                             "$crc");
    }
    return "gmsg.checksum." + cs.algo;
}

std::string checksum_value(Ctx& c, const ChecksumSpec& cs, const std::string& bytes, const std::string& start,
                           const std::string& end) {
    bool fn = false;
    const auto algo = checksum_algo(c, cs, fn);
    if (fn) return "BigInt.asUintN(64, BigInt(" + algo + "(" + bytes + ".subarray(" + start + ", " + end + "))))";
    return "BigInt(" + algo + ".compute(" + bytes + ", " + start + ", " + end + "))";
}

namespace {

/// The field value as a Number for engineering conversions (static_cast<double>).
std::string num_of(const Field& f, const std::string& v) {
    switch (storage_prim(f)) {
        case Prim::F32: return "Math.fround(" + v + ")";
        case Prim::F64: return v;
        case Prim::Bool: return "(" + v + " ? 1 : 0)";
        case Prim::Char: return "Number(gmsg.toInt(" + v + "))";
        default: return "Number(" + v + ")";
    }
}

} // namespace

std::string print_value(Ctx& c, const Message& m, const Field& f, const std::string& v) {
    switch (f.kind) {
        case FieldKind::Scalar:
            switch (f.prim) {
                case Prim::F32: return "p.float(Math.fround(" + v + "), true);";
                case Prim::F64: return "p.float(" + v + ");";
                case Prim::Bool: return "p.bool(" + v + ");";
                case Prim::Char: return "p.char(" + v + ");";
                default: return "p.int(" + v + ");";
            }
        case FieldKind::Enum:
            return "p.enumValue(" + f.enum_def->name + ".nameOf(" + v + "), " + v + ", " +
                   std::to_string(prim_size(f.enum_def->underlying)) + ");";
        case FieldKind::Bits: return bits_type_name(m, f) + ".prototype.printTo.call(" + v + ", p);";
        case FieldKind::Bytes: return "p.hex(" + v + ");";
        case FieldKind::String: return "p.str(" + v + ");";
        case FieldKind::Message: return "gmsg.printNested(p, " + v + ", " + f.message->name + ");";
        case FieldKind::Custom: {
            const auto hook = ext_fn(c, f.custom->lang, "print");
            // without a hook: numbers, BigInts and strings print as such (as C++ prints a scalar custom type), else <?>
            return hook.empty() ? "p.value(" + v + ");" : hook + "(p, " + v + ");";
        }
        case FieldKind::Array: {
            const auto& e = *f.elem;
            const bool blocks = e.kind == FieldKind::Message;
            return "p.list(" + v + ", (x) => { " + print_value(c, m, e, "x") + " }" + (blocks ? ", true" : "") + ");";
        }
        default: return "p.text('<?>');";
    }
}

void emit_print_extras(CodeWriter& w, const Field& f, const std::string& value) {
    if (f.kind != FieldKind::Scalar) return;
    const bool scaled = f.scale || f.offset_value;
    if (!f.unit.empty() && !scaled) w.line("p.unit(" + string_literal(f.unit) + ");");
    auto conv = [&](double sc, double of, const std::string& unit) {
        w.line("p.conversion(" + num_of(f, value) + " * " + double_literal(sc) + " + " + double_literal(of) +
               (unit.empty() ? "" : ", " + string_literal(unit)) + ");");
    };
    if (scaled) conv(f.scale.value_or(1.0), f.offset_value.value_or(0.0), f.unit);
    for (const auto& cv : f.conversions) conv(cv.scale.value_or(1.0), cv.offset_value.value_or(0.0), cv.unit);
}

bool has_print_extras(const Field& f) {
    return f.kind == FieldKind::Scalar && (!f.unit.empty() || f.scale || f.offset_value || !f.conversions.empty());
}

void doc(CodeWriter& w, const std::string& text) {
    if (text.empty()) return;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) w.line("// " + line);
}

bool is_derived_length(const Field& f) {
    if (f.len.kind != LenKind::Expr || !f.len.expr) return false;
    const auto& e = *f.len.expr;
    return e.kind == Expr::Kind::Ref && e.field && !e.bitfield && e.field->derived_from == &f;
}

} // namespace gmpc::js
