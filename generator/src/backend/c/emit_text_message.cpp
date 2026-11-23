// C emission for text (ASCII, token separated) messages. Mirrors backend/cpp/emit_text_message.cpp: the same
// tokens, parsing rules, errors and writer calls.
//
//   struct  P_gga { double utc_time; bool has_latitude; double latitude; ... }   (optional: true -> has_<field>)
//   P_gga_read_text(tokens, n, &m) / P_gga_write_text(&m, &writer) / P_gga_decode / P_gga_encode
//   view    P_gga_view (tokenised once), accessors parse on demand: gmsg_error P_gga_view_latitude(&v, &out)
//           (GMSG_ERR_MISSING_FIELD for an empty token), P_gga_view_has_latitude(&v), strings as gmsg_str

#include <map>
#include <string>

#include "backend/c/c_common.hpp"

namespace gmpc::c {

namespace {

using namespace ir;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string try_(const std::string& call) { return "if ((e = " + call + ") != GMSG_OK) return e;"; }
std::string fail(const char* err) { return std::string("return GMSG_ERR_") + err + ";"; }

std::string char_literal(char ch) {
    switch (ch) {
        case '\'': return "'\\''";
        case '\\': return "'\\\\'";
        case '\t': return "'\\t'";
        case '\n': return "'\\n'";
        case '\r': return "'\\r'";
        default: return std::string("'") + ch + "'";
    }
}

std::string int_min(Prim p) {
    switch (p) {
        case Prim::I8: return "INT8_MIN";
        case Prim::I16: return "INT16_MIN";
        case Prim::I32: return "INT32_MIN";
        case Prim::I64: return "INT64_MIN";
        default: return "0";
    }
}

/// The C type a text field's value has (Scalar / Enum / Custom).
std::string value_type(const Ctx& c, const Message& m, const Field& f) { return element_type(c, m, f); }

/// Statements parsing gmsg_str `t` into `dst` (an lvalue of the field's value type); errors return.
void parse_into(const Ctx& c, CodeWriter& w, const Field& f, const std::string& t, const std::string& dst) {
    const auto args = t + ".data, " + t + ".size";
    switch (f.kind) {
        case FieldKind::Scalar:
            if (prim_is_float(f.prim)) {
                w.line(try_(std::string(f.prim == Prim::F32 ? "gmsg_parse_float(" : "gmsg_parse_double(") + args + ", &" + dst + ")"));
            } else if (f.prim == Prim::Char) {
                w.line(try_("gmsg_parse_char(" + args + ", &" + dst + ")"));
            } else if (f.prim == Prim::Bool) {
                w.open("{");
                w.line("int64_t x = 0;");
                w.line(try_("gmsg_parse_i64(" + args + ", 10, INT32_MIN, INT32_MAX, &x)"));
                w.line(dst + " = x != 0;");
                w.close();
            } else if (prim_is_signed(f.prim)) {
                w.open("{");
                w.line("int64_t x = 0;");
                w.line(try_("gmsg_parse_i64(" + args + ", " + std::to_string(f.base) + ", " + int_min(f.prim) + ", " + prim_max(f.prim) + ", &x)"));
                w.line(dst + " = (" + prim_type(f.prim) + ")x;");
                w.close();
            } else {
                w.open("{");
                w.line("uint64_t x = 0;");
                w.line(try_("gmsg_parse_u64(" + args + ", " + std::to_string(f.base) + ", " + prim_max(f.prim) + ", &x)"));
                w.line(dst + " = (" + prim_type(f.prim) + ")x;");
                w.close();
            }
            if (!f.limit_checks.empty())
                w.line("if (" + limit_fail(f.limit_checks, dst, f.prim, f.scale, f.offset_value) + ") " + fail("VALUE_OUT_OF_LIMITS"));
            break;
        case FieldKind::Enum: {
            const auto u = f.enum_def->underlying;
            const auto et = enum_name(c, *f.enum_def);
            w.open("{");
            if (prim_is_signed(u)) {
                w.line("int64_t x = 0;");
                w.line(try_("gmsg_parse_i64(" + args + ", " + std::to_string(f.base) + ", " + int_min(u) + ", " + prim_max(u) + ", &x)"));
            } else {
                w.line("uint64_t x = 0;");
                w.line(try_("gmsg_parse_u64(" + args + ", " + std::to_string(f.base) + ", " + prim_max(u) + ", &x)"));
            }
            if (f.enum_def->strict) w.line("if (!" + et + "_is_known((" + et + ")x)) " + fail("UNKNOWN_ENUM"));
            w.line(dst + " = (" + et + ")x;");
            w.close();
            break;
        }
        case FieldKind::Custom:
            w.line(try_(lang_value(f.custom->lang, "decode") + "(" + args + ", &" + dst + ")"));
            break;
        default: break;
    }
}

/// Statement writing value expression `v` of field `f` as a token (errors return).
std::string write_call(const Ctx& c, const Field& f, const std::string& v) {
    const auto base = std::to_string(f.base), width = std::to_string(f.width);
    switch (f.kind) {
        case FieldKind::Scalar:
            if (prim_is_float(f.prim))
                return try_(std::string(f.prim == Prim::F32 ? "gmsg_text_f32_token(w, " : "gmsg_text_f64_token(w, ") + v + ", " +
                            std::to_string(f.precision) + ", " + width + ")");
            if (f.prim == Prim::Char) return try_("gmsg_text_char_token(w, " + v + ")");
            if (f.prim == Prim::Bool) return try_("gmsg_text_i64_token(w, " + v + " ? 1 : 0, 10, 0)");
            if (prim_is_signed(f.prim)) return try_("gmsg_text_i64_token(w, (int64_t)" + v + ", " + base + ", " + width + ")");
            return try_("gmsg_text_u64_token(w, (uint64_t)" + v + ", " + base + ", " + width + ")");
        case FieldKind::Enum:
            if (prim_is_signed(f.enum_def->underlying))
                return try_("gmsg_text_i64_token(w, (int64_t)" + v + ", " + base + ", " + width + ")");
            return try_("gmsg_text_u64_token(w, (uint64_t)" + v + ", " + base + ", " + width + ")");
        default: (void)c; return "";
    }
}

/// Statement(s) printing the parsed value `v` (an lvalue) of text field `f` and its unit / conversions.
void print_text_value(const Ctx& c, CodeWriter& w, const Field& f, const std::string& v) {
    switch (f.kind) {
        case FieldKind::Scalar: w.line(print_prim(f.prim, v)); break;
        case FieldKind::Enum: w.line(print_enum(c, *f.enum_def, v)); break;
        case FieldKind::Custom: print_custom(w, *f.custom, "&" + v); break;
        default: break;
    }
    print_extras(w, f, v);
}

class TextEmitter {
public:
    TextEmitter(Ctx& c, const Message& m) : c_(c), h_(c.h), s_(c.c), m_(m), t_(type_name(c, m)), v_(view_name(c, m)) {}

    void emit() {
        const auto M = macro_name(c_, m_);
        h_.line("/* ---- " + m_.name + " (text) " + std::string(std::max<int>(0, 89 - static_cast<int>(m_.name.size())), '-') + " */");
        h_.line();
        s_.line("/* ---- " + m_.name + " (text) " + std::string(std::max<int>(0, 89 - static_cast<int>(m_.name.size())), '-') + " */");
        s_.line();
        h_.line("#define " + M + "_TOKEN_COUNT " + sz(m_.token_count));
        h_.line("#define " + M + "_SEPARATOR " + char_literal(m_.separator));
        for (const auto& f : m_.fields)
            if (f->const_value) h_.line("#define " + M + "_" + upper(f->name) + "_VALUE " + typed_literal(c_, *f, *f->const_value));
        h_.line();
        if (m_.want_struct) emit_struct();
        if (m_.want_view) emit_view();
    }

private:
    std::string tok(const Field& f) const { return "t" + sz(f.token_index); }

    void emit_struct() {
        NameSet names(c_.diag, m_.loc, "message '" + m_.name + "'");
        doc(h_, m_.doc);
        h_.open("typedef struct " + t_ + " {");
        bool any = false;
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            if (!f.has_member()) continue;
            any = true;
            std::string comment = f.doc;
            if (!f.unit.empty()) comment += (comment.empty() ? "" : " ") + std::string("[") + f.unit + "]";
            const auto tail = comment.empty() ? std::string() : "  /* " + comment + " */";
            const auto n = ident(f.name);
            if (f.optional) {
                h_.line("bool has_" + f.name + ";  /* the token is not empty */");
                names.add("has_" + f.name, "presence of " + f.name);
            }
            names.add(n, "field " + f.name);
            if (f.kind == FieldKind::String) {
                const auto cap = f.max.value_or(0);
                h_.line("char " + n + "[" + sz(cap + 1) + "];" + (tail.empty() ? "  /* NUL terminated after decode */" : tail));
                h_.line(count_type(cap) + " " + n + "_len;  /* characters in " + f.name + " (encode uses this) */");
                names.add(n + "_len", "length of " + f.name);
            } else {
                h_.line(value_type(c_, m_, f) + " " + n + ";" + tail);
            }
        }
        if (!any) h_.line("uint8_t gmsg_unused_; /* no value fields */");
        h_.close("} " + t_ + ";");
        h_.line();
        for (const char* fn : {"read_text", "write_text", "decode", "encode", "print"}) names.add(fn, "function");
        h_.line("/* Parse / write the tokens of one " + m_.name + " (tokens beyond the count are ignored). */");
        h_.line("gmsg_error " + t_ + "_read_text(const gmsg_str* tok, size_t ntok, " + t_ + "* m);");
        h_.line("gmsg_error " + t_ + "_write_text(const " + t_ + "* m, gmsg_text_writer* w);");
        h_.line("/* Decode text data[0..size) / encode into out[0..capacity) (*written may be NULL). */");
        h_.line("gmsg_error " + t_ + "_decode(const uint8_t* data, size_t size, " + t_ + "* out);");
        h_.line("/* Like _decode; *extra receives the number of tokens after the message's own (which _decode ignores). */");
        h_.line("gmsg_error " + t_ + "_decode_extra(const uint8_t* data, size_t size, " + t_ + "* out, size_t* extra);");
        h_.line("gmsg_error " + t_ + "_encode(const " + t_ + "* m, uint8_t* out, size_t capacity, size_t* written);");
        h_.line("/* Human readable dump (gmp/c/print.h). */");
        h_.line("void " + t_ + "_print(const " + t_ + "* m, gmsg_printer* p);");
        h_.line();

        // read_text
        s_.open("gmsg_error " + t_ + "_read_text(const gmsg_str* tok, size_t ntok, " + t_ + "* m) {");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line("(void)tok;");
        s_.line("(void)ntok;");
        s_.line("(void)m;");
        s_.line("(void)e;");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            if (f.kind == FieldKind::Pad) continue;
            const auto n = "m->" + ident(f.name);
            const auto t = tok(f);
            const auto i = sz(f.token_index);
            s_.open("{");
            s_.line("const gmsg_str " + t + " = " + i + " < ntok ? tok[" + i + "] : gmsg_str_make(NULL, 0);");
            if (f.kind == FieldKind::String) {
                const auto cap = sz(f.max.value_or(0));
                if (f.optional) s_.line("m->has_" + f.name + " = " + t + ".size != 0;");
                s_.line("if (" + t + ".size > " + cap + ") " + fail("OUT_OF_RANGE"));
                s_.line("gmsg_memcpy(" + n + ", " + t + ".data, " + t + ".size);");
                s_.line(n + "[" + t + ".size] = '\\0';");
                s_.line(n + "_len = (" + count_type(f.max.value_or(0)) + ")" + t + ".size;");
            } else {
                if (f.optional) {
                    s_.line("m->has_" + f.name + " = " + t + ".size != 0;");
                    s_.open("if (m->has_" + f.name + ") {");
                }
                if (f.const_value) {
                    s_.line(value_type(c_, m_, f) + " v;");
                    parse_into(c_, s_, f, t, "v");
                    s_.line("if (v != " + typed_literal(c_, f, *f.const_value) + ") " + fail("BAD_CONST"));
                } else {
                    parse_into(c_, s_, f, t, n);
                }
                if (f.optional) s_.close();
            }
            s_.close();
        }
        s_.line("return GMSG_OK;");
        s_.close();
        s_.line();

        // write_text
        s_.open("gmsg_error " + t_ + "_write_text(const " + t_ + "* m, gmsg_text_writer* w) {");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line("(void)m;");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            const auto n = "m->" + ident(f.name);
            if (f.kind == FieldKind::Pad) {
                s_.open("for (int k = 0; k < " + sz(f.len.fixed) + "; ++k) {");
                s_.line(try_("gmsg_text_empty_token(w)"));
                s_.close();
                continue;
            }
            const auto value = f.const_value ? typed_literal(c_, f, *f.const_value) : n;
            if (f.optional) {
                s_.open("if (!m->has_" + f.name + ") {");
                s_.line(try_("gmsg_text_empty_token(w)"));
                s_.dedent();
                s_.open("} else {");
            }
            if (f.kind == FieldKind::String) {
                s_.line("if (" + n + "_len > " + sz(f.max.value_or(0)) + ") " + fail("OUT_OF_RANGE"));
                s_.line(try_("gmsg_text_token(w, " + n + ", " + n + "_len)"));
            } else if (f.kind == FieldKind::Custom) {
                s_.line(try_("gmsg_text_begin_token(w)"));
                s_.line(try_(lang_value(f.custom->lang, "encode") + "(w, &" + value + ")"));
            } else {
                if (f.kind == FieldKind::Enum && f.enum_def->strict && !f.const_value)
                    s_.line("if (!" + enum_name(c_, *f.enum_def) + "_is_known(" + value + ")) " + fail("UNKNOWN_ENUM"));
                if (!f.limit_checks.empty())
                    s_.line("if (" + limit_fail(f.limit_checks, value, f.prim, f.scale, f.offset_value) + ") " +
                            fail("VALUE_OUT_OF_LIMITS"));
                s_.line(write_call(c_, f, value));
            }
            if (f.optional) s_.close();
        }
        s_.line("return e;");
        s_.close();
        s_.line();

        const auto M = macro_name(c_, m_);
        s_.open("gmsg_error " + t_ + "_decode(const uint8_t* data, size_t size, " + t_ + "* out) {");
        s_.line("gmsg_str tok[" + sz(m_.token_count ? m_.token_count : 1) + "];");
        s_.line("const size_t n = gmsg_split((const char*)data, size, " + M + "_SEPARATOR, tok, " + sz(m_.token_count) + ");");
        s_.line("gmsg_memset(out, 0, sizeof *out);");
        s_.line("return " + t_ + "_read_text(tok, n < " + sz(m_.token_count) + " ? n : " + sz(m_.token_count) + ", out);");
        s_.close();
        s_.line();
        s_.open("gmsg_error " + t_ + "_decode_extra(const uint8_t* data, size_t size, " + t_ + "* out, size_t* extra) {");
        s_.line("gmsg_str tok[" + sz(m_.token_count ? m_.token_count : 1) + "];");
        s_.line("const size_t n = gmsg_split((const char*)data, size, " + M + "_SEPARATOR, tok, " + sz(m_.token_count) + ");");
        s_.line("gmsg_error e;");
        s_.line("gmsg_memset(out, 0, sizeof *out);");
        s_.line("e = " + t_ + "_read_text(tok, n < " + sz(m_.token_count) + " ? n : " + sz(m_.token_count) + ", out);");
        s_.line("*extra = e == GMSG_OK && n > " + sz(m_.token_count) + " ? n - " + sz(m_.token_count) + " : 0;");
        s_.line("return e;");
        s_.close();
        s_.line();
        s_.open("gmsg_error " + t_ + "_encode(const " + t_ + "* m, uint8_t* out, size_t capacity, size_t* written) {");
        s_.line("gmsg_text_writer w = gmsg_text_writer_init(out, capacity, " + M + "_SEPARATOR);");
        s_.line("const gmsg_error e = " + t_ + "_write_text(m, &w);");
        s_.line("if (written) *written = e == GMSG_OK ? w.pos : 0;");
        s_.line("return e;");
        s_.close();
        s_.line();

        // print
        s_.open("void " + t_ + "_print(const " + t_ + "* m, gmsg_printer* p) {");
        s_.line("(void)m;");
        s_.line("gmsg_print_begin(p, " + string_literal(m_.name) + ");");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            if (!f.has_member()) continue;
            const auto n = "m->" + ident(f.name);
            s_.line("gmsg_print_key(p, " + string_literal(f.name) + ");");
            if (f.optional) {
                s_.open("if (!m->has_" + f.name + ") {");
                s_.line("gmsg_print_absent_value(p);");
                s_.dedent();
                s_.open("} else {");
            }
            if (f.kind == FieldKind::String) s_.line("gmsg_print_str(p, " + n + ", " + n + "_len);");
            else print_text_value(c_, s_, f, n);
            if (f.optional) s_.close();
        }
        s_.line("gmsg_print_end(p);");
        s_.close();
        s_.line();
    }

    void emit_view() {
        NameSet names(c_.diag, m_.loc, "view of message '" + m_.name + "'");
        const auto M = macro_name(c_, m_);
        const auto nt = sz(m_.token_count ? m_.token_count : 1);
        doc(h_, "Zero-copy view over a " + m_.name + " text record: tokenised once, fields parsed on access.");
        h_.open("typedef struct " + v_ + " {");
        h_.line("const uint8_t* data;");
        h_.line("size_t size;");
        h_.line("gmsg_str tok[" + nt + "];");
        h_.line("size_t ntok;  /* tokens present in the record (may differ from the token count) */");
        h_.close("} " + v_ + ";");
        h_.line();
        for (const char* fn : {"from", "unchecked", "to_struct", "present_tokens", "extra_tokens", "token", "print"}) names.add(fn, "function");
        h_.line("/* Tokenise data[0..size); never fails (fields are checked when they are read). */");
        h_.line("gmsg_error " + v_ + "_from(const uint8_t* data, size_t size, " + v_ + "* out);");
        h_.line(v_ + " " + v_ + "_unchecked(const uint8_t* data, size_t size);");
        if (m_.want_struct) h_.line("gmsg_error " + v_ + "_to_struct(const " + v_ + "* v, " + t_ + "* out);");
        h_.line("/* Human readable dump (gmp/c/print.h); same text as the struct. */");
        h_.line("void " + v_ + "_print(const " + v_ + "* v, gmsg_printer* p);");
        h_.line("static inline size_t " + v_ + "_present_tokens(const " + v_ + "* v) { return v->ntok; }");
        h_.line("/* Tokens after the message's own (ignored; a protocol with exact_size rejects them). */");
        h_.line("static inline size_t " + v_ + "_extra_tokens(const " + v_ + "* v) { return v->ntok > " + sz(m_.token_count) +
                " ? v->ntok - " + sz(m_.token_count) + " : 0; }");
        h_.line("static inline gmsg_str " + v_ + "_token(const " + v_ + "* v, size_t i) { return i < " + sz(m_.token_count) +
                " ? v->tok[i] : gmsg_str_make(NULL, 0); }");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            if (!f.has_member()) continue;
            const auto base = v_ + "_" + f.name;
            const auto t = "v->tok[" + sz(f.token_index) + "]";
            if (!f.doc.empty()) h_.line("/* " + f.doc + " */");
            names.add(f.name, "field " + f.name);
            if (f.optional) {
                names.add("has_" + f.name, "presence of " + f.name);
                h_.line("static inline bool " + v_ + "_has_" + f.name + "(const " + v_ + "* v) { return " + t + ".size != 0; }");
            }
            if (f.kind == FieldKind::String) {
                h_.line("static inline gmsg_str " + base + "(const " + v_ + "* v) { return " + t + "; }");
                continue;
            }
            const auto vt = value_type(c_, m_, f);
            h_.open("static inline gmsg_error " + base + "(const " + v_ + "* v, " + vt + "* out) {");
            h_.line("gmsg_error e = GMSG_OK;");
            h_.line("const gmsg_str t = " + t + ";");
            if (f.optional) h_.line("if (t.size == 0) " + fail("MISSING_FIELD"));
            parse_into(c_, h_, f, "t", "*out");
            h_.line("return e;");
            h_.close();
            if (f.kind == FieldKind::Scalar && !f.optional) {
                auto conv = [&](const std::string& suffix, double sc, double of) {
                    names.add(f.name + "_" + suffix, "conversion");
                    h_.open("static inline gmsg_error " + base + "_" + suffix + "(const " + v_ + "* v, double* out) {");
                    h_.line(vt + " x;");
                    h_.line("const gmsg_error e = " + base + "(v, &x);");
                    h_.line("if (e == GMSG_OK) *out = (double)x * " + double_literal(sc) + " + " + double_literal(of) + ";");
                    h_.line("return e;");
                    h_.close();
                };
                if (f.scale || f.offset_value) conv("scaled", f.scale.value_or(1.0), f.offset_value.value_or(0.0));
                for (const auto& cv : f.conversions) conv(cv.name, cv.scale.value_or(1.0), cv.offset_value.value_or(0.0));
            }
        }
        h_.line();
        s_.open("gmsg_error " + v_ + "_from(const uint8_t* data, size_t size, " + v_ + "* out) {");
        s_.line("out->data = data;");
        s_.line("out->size = size;");
        s_.line("out->ntok = gmsg_split((const char*)data, size, " + M + "_SEPARATOR, out->tok, " + sz(m_.token_count) + ");");
        s_.line("return GMSG_OK;");
        s_.close();
        s_.line();
        s_.open(v_ + " " + v_ + "_unchecked(const uint8_t* data, size_t size) {");
        s_.line(v_ + " v;");
        s_.line("(void)" + v_ + "_from(data, size, &v);");
        s_.line("return v;");
        s_.close();
        s_.line();
        if (m_.want_struct) {
            s_.open("gmsg_error " + v_ + "_to_struct(const " + v_ + "* v, " + t_ + "* out) {");
            s_.line("return " + t_ + "_decode(v->data, v->size, out);");
            s_.close();
            s_.line();
        }

        // print: each field parsed on demand; a field that fails prints <error: ...>
        s_.open("void " + v_ + "_print(const " + v_ + "* v, gmsg_printer* p) {");
        s_.line("(void)v;");
        s_.line("gmsg_print_begin(p, " + string_literal(m_.name) + ");");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            if (!f.has_member()) continue;
            const auto base = v_ + "_" + f.name;
            s_.line("gmsg_print_key(p, " + string_literal(f.name) + ");");
            if (f.optional) {
                s_.open("if (!" + v_ + "_has_" + f.name + "(v)) {");
                s_.line("gmsg_print_absent_value(p);");
                s_.dedent();
                s_.open("} else {");
            } else {
                s_.open("{");
            }
            if (f.kind == FieldKind::String) {
                s_.line("const gmsg_str s = " + base + "(v);");
                s_.line("gmsg_print_str(p, s.data, s.size);");
            } else {
                s_.line(value_type(c_, m_, f) + " x;");
                s_.line("const gmsg_error e = " + base + "(v, &x);");
                s_.open("if (e == GMSG_OK) {");
                print_text_value(c_, s_, f, "x");
                s_.dedent();
                s_.open("} else {");
                s_.line("gmsg_print_error(p, e);");
                s_.close();
            }
            s_.close();
        }
        s_.line("gmsg_print_end(p);");
        s_.close();
        s_.line();
    }

    Ctx& c_;
    CodeWriter& h_;
    CodeWriter& s_;
    const Message& m_;
    std::string t_, v_;
};

} // namespace

void emit_text_message(Ctx& c, const Message& m) { TextEmitter(c, m).emit(); }

} // namespace gmpc::c
