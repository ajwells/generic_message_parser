// JavaScript emission for text (ASCII, token separated) messages: a struct class and a lazily parsed view class.
// Semantics follow backend/cpp/emit_text_message.cpp.

#include "backend/js/js_common.hpp"

namespace gmpc::js {

namespace {

using namespace ir;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string fail(const char* err) { return std::string("gmsg.fail(Err.") + err + ");"; }
std::string q(const std::string& s) { return "'" + s + "'"; }


/// `s` (an expression parsing a token) wrapped so values outside the field's limits throw ValueOutOfLimits.
std::string with_limits(const Field& f, const std::string& s) {
    if (f.limit_checks.empty()) return s;
    return "((v) => { if (" + limit_fail(f.limit_checks, "v", f.prim, f.scale, f.offset_value) + ") " +
           "gmsg.fail(Err.ValueOutOfLimits); return v; })(" + s + ")";
}

/// Expression parsing token `t` into the field's value (throws GmpError).
std::string parse_call(Ctx& c, const Field& f, const std::string& t) {
    switch (f.kind) {
        case FieldKind::Scalar:
            if (f.prim == Prim::F32) return with_limits(f, "gmsg.text.parseFloat(" + t + ", true)");
            if (f.prim == Prim::F64) return with_limits(f, "gmsg.text.parseFloat(" + t + ")");
            if (f.prim == Prim::Char) return "gmsg.text.parseChar(" + t + ")";
            if (f.prim == Prim::Bool) return "(gmsg.text.parseInt(" + t + ", 'i32') !== 0)";
            return with_limits(f, "gmsg.text.parseInt(" + t + ", " + q(prim_id(f.prim)) + ", " + std::to_string(f.base) + ")");
        case FieldKind::Enum: {
            const auto s = "gmsg.text.parseInt(" + t + ", " + q(prim_id(f.enum_def->underlying)) + ", " + std::to_string(f.base) + ")";
            if (f.enum_def->strict) return "gmsg.knownEnum(" + f.enum_def->name + ", " + s + ")";
            return s;
        }
        case FieldKind::Custom: return ext_fn(c, f.custom->lang, "decode") + "(" + t + ")";
        default: return "undefined";
    }
}

/// Statement writing value expression `v` of field `f` as a token with TextWriter `w`.
std::string write_call(Ctx& c, const Field& f, const std::string& v) {
    switch (f.kind) {
        case FieldKind::Scalar:
            if (f.prim == Prim::F32)
                return "w.floatToken(Math.fround(" + v + "), " + std::to_string(f.precision) + ", " + std::to_string(f.width) + ", true);";
            if (f.prim == Prim::F64)
                return "w.floatToken(" + v + ", " + std::to_string(f.precision) + ", " + std::to_string(f.width) + ");";
            if (f.prim == Prim::Char) return "w.token(gmsg.checkChar(" + v + "));";
            if (f.prim == Prim::Bool) return "w.intToken(" + v + " ? 1 : 0);";
            return "w.intToken(gmsg.checkInt(" + v + ", " + q(prim_id(f.prim)) + "), " + std::to_string(f.base) + ", " +
                   std::to_string(f.width) + ");";
        case FieldKind::Enum:
            return "w.intToken(gmsg.checkInt(" + v + ", " + q(prim_id(f.enum_def->underlying)) + "), " + std::to_string(f.base) +
                   ", " + std::to_string(f.width) + ");";
        case FieldKind::String: return "w.token(" + v + ");";
        case FieldKind::Custom: return "w.beginToken(); " + ext_fn(c, f.custom->lang, "encode") + "(w, " + v + ");";
        default: return "";
    }
}

void emit_print(Ctx& c, const Message& m, bool view) {
    auto& w = c.w;
    w.open("printTo(p) {");
    w.line("p.begin(" + string_literal(m.name) + ");");
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        if (!f.has_member()) continue;
        const auto acc = "this." + ident(f.name);
        w.open("{");
        if (view) {
            // parse errors print <error: ...>; empty optional tokens <absent>
            w.line("const [e, x] = gmsg.attempt(() => " + acc + ");");
            w.line("p.key(" + string_literal(f.name) + ");");
            w.line("if (e) p.error(e);");
            w.line("else if (x === undefined) p.text('<absent>');");
            w.open("else {");
        } else {
            w.line("const x = " + acc + ";");
            w.line("p.key(" + string_literal(f.name) + ");");
            w.line("if (x === undefined) p.text('<absent>');");
            w.open("else {");
        }
        w.line(print_value(c, m, f, "x"));
        emit_print_extras(w, f, "x");
        w.close();
        w.close();
    }
    w.line("p.end();");
    w.close();
}

void emit_struct(Ctx& c, const Message& m) {
    auto& w = c.w;
    const auto sep = string_literal(std::string(1, m.separator));
    doc(w, m.doc);
    w.open("class " + m.name + " {");
    w.open("constructor(init) {");
    for (const auto& f : m.fields) {
        if (!f->has_member()) continue;
        std::string init;
        if (f->optional) init = "undefined";
        else if (f->kind == FieldKind::String) init = "''";
        else if (f->kind == FieldKind::Custom) init = "undefined";
        else init = typed_literal(*f, 0);
        w.line("this." + ident(f->name) + " = " + init + ";" + (f->unit.empty() ? "" : "  // [" + f->unit + "]"));
    }
    w.line("if (init) Object.assign(this, init);");
    w.close();
    w.line("static messageName = " + string_literal(m.name) + ";");
    w.line("static tokenCount = " + sz(m.token_count) + ";");
    w.line("static separator = " + sep + ";");
    w.line();

    // readText
    w.line("/** Fill a new message from its tokens (an array of strings; missing tokens count as empty). */");
    w.open("static readText(tok) {");
    w.line("const m = new " + m.name + "();");
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        if (f.kind == FieldKind::Pad) continue;
        const auto n = "m." + ident(f.name);
        const auto t = "t" + sz(f.token_index);
        w.open("{");
        w.line("const " + t + " = " + sz(f.token_index) + " < tok.length ? tok[" + sz(f.token_index) + "] : '';");
        if (f.kind == FieldKind::String) {
            const auto max = sz(f.max.value_or(0));
            if (f.optional) w.line("if (" + t + " === '') " + n + " = undefined;");
            w.line(std::string(f.optional ? "else " : "") + "if (" + t + ".length > " + max + ") " + fail("OutOfRange"));
            w.line(std::string(f.optional ? "else " : "") + n + " = " + t + ";");
        } else {
            if (f.optional) w.open("if (" + t + " === '') { " + n + " = undefined; } else {");
            w.line("const v = " + parse_call(c, f, t) + ";");
            if (f.const_value) w.line("if (v !== " + typed_literal(f, *f.const_value) + ") " + fail("BadConst"));
            else w.line(n + " = v;");
            if (f.optional) w.close();
        }
        w.close();
    }
    w.line("return m;");
    w.close();

    // writeText
    w.line("/** Write the tokens of `m` with a gmsg.text.TextWriter. */");
    w.open("static writeText(w, m) {");
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        const auto n = "m." + ident(f.name);
        if (f.kind == FieldKind::Pad) {
            w.line("for (let k = 0; k < " + sz(f.len.fixed) + "; k++) w.emptyToken();");
            continue;
        }
        const auto value = f.const_value ? typed_literal(f, *f.const_value) : n;
        if (f.optional) w.open("if (" + n + " === undefined || " + n + " === null) { w.emptyToken(); } else {");
        if (f.kind == FieldKind::String)
            w.line("if (" + value + ".length > " + sz(f.max.value_or(0)) + ") " + fail("OutOfRange"));
        if (f.kind == FieldKind::Enum && f.enum_def->strict && !f.const_value)
            w.line("if (!" + f.enum_def->name + ".isKnown(" + value + ")) " + fail("UnknownEnum"));
        if (!f.limit_checks.empty())
            w.line("if (" + limit_fail(f.limit_checks, value, f.prim, f.scale, f.offset_value) + ") " + fail("ValueOutOfLimits"));
        w.line(write_call(c, f, value));
        if (f.optional) w.close();
    }
    w.close();

    w.line("/** Parse a record (the text between the separators, without the type token). Throws GmpError. */");
    w.open("static decodeText(s) {");
    w.line("return " + m.name + ".readText(gmsg.text.split(s, " + sep + ").slice(0, " + sz(m.token_count) + "));");
    w.close();
    w.line("static decode(bytes) { return " + m.name + ".decodeText(gmsg.latin1String(gmsg.toBytes(bytes))); }");
    w.line("/** Like decode(bytes): { message, extra }, `extra` = tokens after the message's own (which decode ignores). */");
    w.open("static decodeWithExtra(bytes) {");
    w.line("const all = gmsg.text.split(typeof bytes === 'string' ? bytes : gmsg.latin1String(gmsg.toBytes(bytes)), " + sep + ");");
    w.line("return { message: " + m.name + ".readText(all.slice(0, " + sz(m.token_count) + ")), extra: Math.max(0, all.length - " +
           sz(m.token_count) + ") };");
    w.close();
    w.line("/** The record as text. */");
    w.line("static encodeText(m) { const w = new gmsg.text.TextWriter(" + sep + "); " + m.name + ".writeText(w, m); return w.toString(); }");
    w.line("static encode(m) { return gmsg.latin1Bytes(" + m.name + ".encodeText(m)); }");
    w.line("encode() { return " + m.name + ".encode(this); }");
    w.line("encodeText() { return " + m.name + ".encodeText(this); }");
    if (m.want_view) w.line("static view(text) { return " + view_name(m) + ".from(text); }");
    emit_print(c, m, false);
    w.line("toString(opts) { return gmsg.print(this, opts); }");
    w.close("}");
    w.line();
    c.exports.push_back(m.name);
}

void emit_view(Ctx& c, const Message& m) {
    auto& w = c.w;
    const auto name = view_name(m);
    const auto sep = string_literal(std::string(1, m.separator));
    doc(w, "Lazily parsed view over a " + m.name + " text record; fields are parsed on access (and throw GmpError).");
    w.open("class " + name + " {");
    w.line("#text;");
    w.line("#tok;");
    w.line("#n;");
    w.line("constructor(text, tok, n) { this.#text = text; this.#tok = tok; this.#n = n; }");
    w.line("static messageName = " + string_literal(m.name) + ";");
    w.line("static tokenCount = " + sz(m.token_count) + ";");
    w.line("static separator = " + sep + ";");
    std::string fields;
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        fields += (fields.empty() ? "" : ", ") + std::string("{ name: ") + string_literal(f.name) + ", key: " +
                  (f.has_member() ? string_literal(ident(f.name)) : std::string("null")) + ", token: " + sz(f.token_index) +
                  ", tokens: " + sz(f.token_count) + " }";
    }
    w.line("/** The fields in token order: { name, key (the property, null for padding / constants), token, tokens }. */");
    w.line("static fields = [" + fields + "];");
    w.open("static from(data) {");
    w.line("const text = typeof data === 'string' ? data : gmsg.latin1String(gmsg.toBytes(data));");
    w.line("const all = gmsg.text.split(text, " + sep + ");");
    w.line("return new " + name + "(text, all.slice(0, " + sz(m.token_count) + "), all.length);");
    w.close();
    w.line("static unchecked(data) { return " + name + ".from(data); }");
    w.line("/** The record text. */");
    w.line("get raw() { return this.#text; }");
    w.line("/** Number of tokens actually present in the record (may differ from tokenCount). */");
    w.line("get presentTokens() { return this.#n; }");
    w.line("/** Tokens after the message's own (ignored; a protocol with exact_size rejects them). */");
    w.line("get extraTokens() { return Math.max(0, this.#n - " + sz(m.token_count) + "); }");
    w.line("token(i) { return i < this.#tok.length ? this.#tok[i] : ''; }");
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        if (!f.has_member()) continue;
        const auto n = ident(f.name);
        const auto t = "this.token(" + sz(f.token_index) + ")";
        if (f.kind == FieldKind::String) {
            if (f.optional) w.line("get " + n + "() { const t = " + t + "; return t === '' ? undefined : t; }");
            else w.line("get " + n + "() { return " + t + "; }");
            continue;
        }
        if (f.optional) w.line("get " + n + "() { const t = " + t + "; return t === '' ? undefined : " + parse_call(c, f, "t") + "; }");
        else w.line("get " + n + "() { return " + parse_call(c, f, t) + "; }");
        if (f.kind == FieldKind::Scalar && !f.optional) {
            auto conv = [&](const std::string& suffix, double sc, double of) {
                const auto x = f.prim == Prim::F32 ? std::string("Math.fround(this.") + n + ")" : "Number(this." + n + ")";
                w.line("get " + f.name + "_" + suffix + "() { return " + x + " * " + double_literal(sc) + " + " + double_literal(of) + "; }");
            };
            if (f.scale || f.offset_value) conv("scaled", f.scale.value_or(1.0), f.offset_value.value_or(0.0));
            for (const auto& cv : f.conversions) conv(cv.name, cv.scale.value_or(1.0), cv.offset_value.value_or(0.0));
        }
    }
    emit_print(c, m, true);
    w.line("toString(opts) { return gmsg.print(this, opts); }");
    if (m.want_struct) w.line("toStruct() { return " + m.name + ".decodeText(this.#text); }");
    w.close("}");
    w.line();
    c.exports.push_back(name);
}

} // namespace

void emit_text_message(Ctx& c, const Message& m) {
    if (m.want_struct) emit_struct(c, m);
    if (m.want_view) emit_view(c, m);
}

} // namespace gmpc::js
