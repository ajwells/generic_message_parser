// Python emission for text (ASCII, token separated) messages: a dataclass and a lazily parsed view class.
// Semantics follow backend/js/emit_text_message.cpp (and so the C++ backend).

#include "backend/py/py_common.hpp"

namespace gmpc::py {

namespace {

using namespace ir;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string fail(const char* err) { return std::string("gmsg.fail(Err.") + err + ")"; }

/// `s` (an expression parsing a token) wrapped so values outside the field's limits raise ValueOutOfLimits.
std::string with_limits(const Field& f, const std::string& s) {
    if (f.limit_checks.empty()) return s;
    return "gmsg.limited(" + s + ", lambda v: " + limit_fail(f.limit_checks, "v", f.prim, f.scale, f.offset_value) + ")";
}

/// Expression parsing token `t` into the field's value (raises GmpError).
std::string parse_call(Ctx& c, const Field& f, const std::string& t) {
    switch (f.kind) {
        case FieldKind::Scalar:
            if (f.prim == Prim::F32) return with_limits(f, "gmsg.text.parse_float(" + t + ", True)");
            if (f.prim == Prim::F64) return with_limits(f, "gmsg.text.parse_float(" + t + ")");
            if (f.prim == Prim::Char) return "gmsg.text.parse_char(" + t + ")";
            if (f.prim == Prim::Bool) return "(gmsg.text.parse_int(" + t + ", 'i32') != 0)";
            return with_limits(f, "gmsg.text.parse_int(" + t + ", " + prim_id(f.prim) + ", " + std::to_string(f.base) + ")");
        case FieldKind::Enum: {
            const auto s = "gmsg.text.parse_int(" + t + ", " + prim_id(f.enum_def->underlying) + ", " + std::to_string(f.base) + ")";
            if (f.enum_def->strict) return "gmsg.known_enum(" + f.enum_def->name + ", " + s + ")";
            return f.enum_def->name + ".of(" + s + ")";
        }
        case FieldKind::Custom: return ext_fn(c, f.custom->lang, "decode") + "(" + t + ")";
        default: return "None";
    }
}

/// Statement writing value expression `v` of field `f` as a token with TextWriter `w`.
std::string write_call(Ctx& c, const Field& f, const std::string& v) {
    switch (f.kind) {
        case FieldKind::Scalar:
            if (f.prim == Prim::F32)
                return "w.float_token(gmsg.to_f32(" + v + "), " + std::to_string(f.precision) + ", " + std::to_string(f.width) + ", True)";
            if (f.prim == Prim::F64)
                return "w.float_token(" + v + ", " + std::to_string(f.precision) + ", " + std::to_string(f.width) + ")";
            if (f.prim == Prim::Char) return "w.token(gmsg.check_char(" + v + "))";
            if (f.prim == Prim::Bool) return "w.int_token(1 if " + v + " else 0)";
            return "w.int_token(gmsg.check_int(" + v + ", " + prim_id(f.prim) + "), " + std::to_string(f.base) + ", " +
                   std::to_string(f.width) + ")";
        case FieldKind::Enum:
            return "w.int_token(gmsg.check_int(" + v + ", " + prim_id(f.enum_def->underlying) + "), " + std::to_string(f.base) +
                   ", " + std::to_string(f.width) + ")";
        case FieldKind::String: return "w.token(" + v + ")";
        case FieldKind::Custom: return "w.begin_token(); " + ext_fn(c, f.custom->lang, "encode") + "(w, " + v + ")";
        default: return "";
    }
}

std::string field_type(const Field& f) {
    std::string t;
    switch (f.kind) {
        case FieldKind::Scalar:
            t = f.prim == Prim::F32 || f.prim == Prim::F64 ? "float" : f.prim == Prim::Bool ? "bool" : f.prim == Prim::Char ? "str" : "int";
            break;
        case FieldKind::Enum: t = f.enum_def->name + " | int"; break;
        case FieldKind::String: t = "str"; break;
        default: t = "object"; break;
    }
    return f.optional && f.kind != FieldKind::Custom ? t + " | None" : t;
}

void text_methods(CodeWriter& w) {
    w.line("def to_text(self, **options) -> str:");
    w.line("    return gmsg.to_text(self, **options)");
    w.line();
    w.line("def __str__(self) -> str:");
    w.line("    return gmsg.to_text(self)");
}

void emit_print(Ctx& c, const Message& m, bool view) {
    auto& w = c.w;
    w.line();
    w.open("def print_to(self, p: gmsg.Printer) -> None:");
    w.line("p.begin(" + string_literal(m.name) + ")");
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        if (!f.has_member()) continue;
        const auto acc = "self." + ident(f.name);
        if (view) {
            // parse errors print <error: ...>; empty optional tokens <absent>
            w.line("e, x = gmsg.attempt(lambda: " + acc + ")");
            w.line("p.key(" + string_literal(f.name) + ")");
            w.line("if e: p.error_value(e)");
            w.line("elif x is None: p.raw_text('<absent>')");
        } else {
            w.line("x = " + acc);
            w.line("p.key(" + string_literal(f.name) + ")");
            w.line("if x is None: p.raw_text('<absent>')");
        }
        w.open("else:");
        w.line(print_value(c, m, f, "x"));
        emit_print_extras(w, f, "x");
        w.dedent();
    }
    w.line("p.end()");
    w.dedent();
}

void emit_struct(Ctx& c, const Message& m) {
    auto& w = c.w;
    const auto& n = m.name;
    const auto sep = string_literal(std::string(1, m.separator));
    const auto count = sz(m.token_count);
    w.line("@_dataclass(slots=True)");
    w.open("class " + n + ":");
    doc(w, m.doc);
    for (const auto& f : m.fields) {
        if (!f->has_member()) continue;
        std::string init;
        if (f->optional || f->kind == FieldKind::Custom) init = "None";
        else if (f->kind == FieldKind::String) init = "''";
        else if (f->kind == FieldKind::Enum) init = f->enum_def->name + ".of(" + typed_literal(*f, 0) + ")";
        else init = typed_literal(*f, 0);
        w.line(ident(f->name) + ": " + field_type(*f) + " = " + init + (f->unit.empty() ? "" : "  # [" + f->unit + "]"));
    }
    w.line("message_name: _ClassVar[str] = " + string_literal(n));
    w.line("token_count: _ClassVar[int] = " + count);
    w.line("separator: _ClassVar[str] = " + sep);

    // read_text
    w.line();
    w.line("@staticmethod");
    w.open("def read_text(tok: list[str]) -> " + n + ":");
    w.line("\"\"\"Fill a new message from its tokens (missing tokens count as empty).\"\"\"");
    w.line("m = " + n + "()");
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        if (f.kind == FieldKind::Pad) continue;
        const auto dst = "m." + ident(f.name);
        const auto t = "t" + sz(f.token_index);
        w.line(t + " = tok[" + sz(f.token_index) + "] if " + sz(f.token_index) + " < len(tok) else ''");
        if (f.kind == FieldKind::String) {
            const auto max = sz(f.max.value_or(0));
            if (f.optional) {
                w.line("if " + t + " == '': " + dst + " = None");
                w.line("elif len(" + t + ") > " + max + ": " + fail("OUT_OF_RANGE"));
                w.line("else: " + dst + " = " + t);
            } else {
                w.line("if len(" + t + ") > " + max + ": " + fail("OUT_OF_RANGE"));
                w.line(dst + " = " + t);
            }
            continue;
        }
        if (f.optional) {
            w.line("if " + t + " == '': " + dst + " = None");
            w.open("else:");
        }
        w.line("v = " + parse_call(c, f, t));
        if (f.const_value) w.line("if v != " + typed_literal(f, *f.const_value) + ": " + fail("BAD_CONST"));
        else w.line(dst + " = v");
        if (f.optional) w.dedent();
    }
    w.line("return m");
    w.dedent();

    // write_text
    w.line();
    w.line("@staticmethod");
    w.open("def write_text(w: gmsg.text.TextWriter, m: " + n + ") -> None:");
    w.line("\"\"\"Write the tokens of m.\"\"\"");
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        const auto src = "m." + ident(f.name);
        if (f.kind == FieldKind::Pad) {
            w.open("for _ in range(" + sz(f.len.fixed) + "):");
            w.line("w.empty_token()");
            w.dedent();
            continue;
        }
        const auto value = f.const_value ? typed_literal(f, *f.const_value) : src;
        if (f.optional) {
            w.line("if " + src + " is None: w.empty_token()");
            w.open("else:");
        }
        if (f.kind == FieldKind::String)
            w.line("if len(" + value + ") > " + sz(f.max.value_or(0)) + ": " + fail("OUT_OF_RANGE"));
        if (f.kind == FieldKind::Enum && f.enum_def->strict && !f.const_value)
            w.line("if not " + f.enum_def->name + ".is_known(" + value + "): " + fail("UNKNOWN_ENUM"));
        if (!f.limit_checks.empty())
            w.line("if " + limit_fail(f.limit_checks, value, f.prim, f.scale, f.offset_value) + ": " + fail("VALUE_OUT_OF_LIMITS"));
        w.line(write_call(c, f, value));
        if (f.optional) w.dedent();
    }
    w.dedent();

    w.line();
    w.line("@staticmethod");
    w.open("def decode_text(s: str) -> " + n + ":");
    w.line("\"\"\"Parse a record (the text between the separators, without the type token). Raises gmsg.GmpError.\"\"\"");
    w.line("return " + n + ".read_text(gmsg.text.split(s, " + sep + ")[:" + count + "])");
    w.dedent();
    w.line();
    w.line("@staticmethod");
    w.open("def decode(data) -> " + n + ":");
    w.line("return " + n + ".decode_text(data if isinstance(data, str) else gmsg.latin1_string(gmsg.to_bytes(data)))");
    w.dedent();
    w.line();
    w.line("@staticmethod");
    w.open("def decode_with_extra(data) -> tuple[" + n + ", int]:");
    w.line("\"\"\"Like decode(data): (message, extra), extra = tokens after the message's own (which decode ignores).\"\"\"");
    w.line("tok = gmsg.text.split(data if isinstance(data, str) else gmsg.latin1_string(gmsg.to_bytes(data)), " + sep + ")");
    w.line("return " + n + ".read_text(tok[:" + count + "]), max(0, len(tok) - " + count + ")");
    w.dedent();
    w.line();
    w.open("def encode_text(self) -> str:");
    w.line("\"\"\"The record as text.\"\"\"");
    w.line("w = gmsg.text.TextWriter(" + sep + ")");
    w.line(n + ".write_text(w, self)");
    w.line("return w.text()");
    w.dedent();
    w.line();
    w.open("def encode(self) -> bytes:");
    w.line("return gmsg.latin1_bytes(self.encode_text())");
    w.dedent();
    if (m.want_view) {
        w.line();
        w.line("@staticmethod");
        w.open("def view(text) -> " + view_name(m) + ":");
        w.line("return " + view_name(m) + ".from_text(text)");
        w.dedent();
    }
    emit_print(c, m, false);
    w.line();
    text_methods(w);
    w.dedent();
    w.line();
    w.line();
    c.exports.push_back(n);
}

void emit_view(Ctx& c, const Message& m) {
    auto& w = c.w;
    const auto name = view_name(m);
    const auto sep = string_literal(std::string(1, m.separator));
    const auto count = sz(m.token_count);
    w.open("class " + name + ":");
    w.line("# Lazily parsed view over a " + m.name + " text record; fields are parsed on access (and raise GmpError).");
    w.line("__slots__ = ('_text', '_tok', '_n')");
    w.line("message_name = " + string_literal(m.name));
    w.line("token_count = " + count);
    w.line("separator = " + sep);
    w.line("# The fields in token order (key: the attribute, None for padding / constants).");
    w.open("fields = (");
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        w.line("gmsg.TextField(" + string_literal(f.name) + ", " + (f.has_member() ? string_literal(ident(f.name)) : std::string("None")) +
               ", " + sz(f.token_index) + ", " + sz(f.token_count) + "),");
    }
    w.close(")");
    w.line();
    w.open("def __init__(self, text: str, tok: list[str], n: int) -> None:");
    w.line("self._text, self._tok, self._n = text, tok, n");
    w.dedent();
    w.line();
    w.line("@staticmethod");
    w.open("def from_text(data) -> " + name + ":");
    w.line("\"\"\"A view over a record (str, or bytes with one character per byte).\"\"\"");
    w.line("text = data if isinstance(data, str) else gmsg.latin1_string(gmsg.to_bytes(data))");
    w.line("tok = gmsg.text.split(text, " + sep + ")");
    w.line("return " + name + "(text, tok[:" + count + "], len(tok))");
    w.dedent();
    w.line();
    w.line("@staticmethod");
    w.open("def unchecked(data) -> " + name + ":");
    w.line("return " + name + ".from_text(data)");
    w.dedent();
    w.line();
    w.line("@property");
    w.open("def raw(self) -> str:");
    w.line("\"\"\"The record text.\"\"\"");
    w.line("return self._text");
    w.dedent();
    w.line();
    w.line("@property");
    w.open("def present_tokens(self) -> int:");
    w.line("\"\"\"Number of tokens actually present in the record (may differ from token_count).\"\"\"");
    w.line("return self._n");
    w.dedent();
    w.line();
    w.line("@property");
    w.open("def extra_tokens(self) -> int:");
    w.line("\"\"\"Tokens after the message's own (ignored; a protocol with exact_size rejects them).\"\"\"");
    w.line("return max(0, self._n - " + count + ")");
    w.dedent();
    w.line();
    w.open("def token(self, i: int) -> str:");
    w.line("return self._tok[i] if i < len(self._tok) else ''");
    w.dedent();
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        if (!f.has_member()) continue;
        const auto n = ident(f.name);
        const auto t = "self.token(" + sz(f.token_index) + ")";
        w.line();
        w.line("@property");
        w.open("def " + n + "(self) -> " + field_type(f) + ":");
        if (f.kind == FieldKind::String) {
            if (f.optional) {
                w.line("t = " + t);
                w.line("return None if t == '' else t");
            } else {
                w.line("return " + t);
            }
            w.dedent();
            continue;
        }
        if (f.optional) {
            w.line("t = " + t);
            w.line("return None if t == '' else " + parse_call(c, f, "t"));
        } else {
            w.line("return " + parse_call(c, f, t));
        }
        w.dedent();
        if (f.kind == FieldKind::Scalar && !f.optional) {
            auto conv = [&](const std::string& suffix, double sc, double of) {
                const auto x = f.prim == Prim::F32 ? "gmsg.to_f32(self." + n + ")" : "float(self." + n + ")";
                w.line();
                w.line("@property");
                w.open("def " + f.name + "_" + suffix + "(self) -> float:");
                w.line("return " + x + " * " + double_literal(sc) + " + " + double_literal(of));
                w.dedent();
            };
            if (f.scale || f.offset_value) conv("scaled", f.scale.value_or(1.0), f.offset_value.value_or(0.0));
            for (const auto& cv : f.conversions) conv(cv.name, cv.scale.value_or(1.0), cv.offset_value.value_or(0.0));
        }
    }
    emit_print(c, m, true);
    w.line();
    text_methods(w);
    w.line();
    w.open("def __repr__(self) -> str:");
    w.line("return '<" + name + " ' + gmsg.to_text(self, style='single_line') + '>'");
    w.dedent();
    if (m.want_struct) {
        w.line();
        w.open("def to_struct(self) -> " + m.name + ":");
        w.line("return " + m.name + ".decode_text(self._text)");
        w.dedent();
    }
    w.dedent();
    w.line();
    w.line();
    c.exports.push_back(name);
}

} // namespace

void emit_text_message(Ctx& c, const Message& m) {
    if (m.want_struct) emit_struct(c, m);
    if (m.want_view) emit_view(c, m);
}

} // namespace gmpc::py
