// C++ emission for text (ASCII, token separated) messages.

#include "backend/cpp/cpp_common.hpp"

namespace gmpc::cpp {

namespace {

using namespace ir;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string fail(const char* err) { return std::string("return gmp::fail(gmp::Error::") + err + ");"; }

std::string sep_literal(char c) {
    if (c == '\'') return "'\\''";
    if (c == '\\') return "'\\\\'";
    if (c == '\t') return "'\\t'";
    return std::string("'") + c + "'";
}

/// `.and_then(...)` failing with ValueOutOfLimits for a parsed value outside the field's limits ("" without limits).
std::string limit_then(const Field& f) {
    if (f.limit_checks.empty()) return "";
    const auto t = prim_type(f.prim);
    return ".and_then([](" + t + " v) -> gmp::Result<" + t + "> { if (" +
           limit_fail(f.limit_checks, "v", f.prim, f.scale, f.offset_value) +
           ") return gmp::fail(gmp::Error::ValueOutOfLimits); return v; })";
}

/// Expression parsing token `t` into the field's value type (returns gmp::Result<T>).
std::string parse_call(const Message& m, const Field& f, const std::string& t) {
    switch (f.kind) {
        case FieldKind::Scalar:
            if (prim_is_float(f.prim)) return "gmp::text::parse_float<" + prim_type(f.prim) + ">(" + t + ")" + limit_then(f);
            if (f.prim == Prim::Char) return "gmp::text::parse_char(" + t + ")";
            if (f.prim == Prim::Bool)
                return "gmp::text::parse_int<int>(" + t + ").transform([](int x) { return x != 0; })";
            return "gmp::text::parse_int<" + prim_type(f.prim) + ">(" + t + ", " + std::to_string(f.base) + ")" + limit_then(f);
        case FieldKind::Enum: {
            const auto& e = f.enum_def->name;
            std::string s = "gmp::text::parse_int<" + prim_type(f.enum_def->underlying) + ">(" + t + ", " +
                            std::to_string(f.base) + ").transform([](auto x) { return static_cast<" + e + ">(x); })";
            if (f.enum_def->strict)
                s += ".and_then([](" + e + " v) -> gmp::Result<" + e +
                     "> { if (!is_known(v)) return gmp::fail(gmp::Error::UnknownEnum); return v; })";
            return s;
        }
        case FieldKind::Custom: return lang_value(f.custom->lang, "decode") + "(" + t + ")";
        default: (void)m; return "";
    }
}

/// Statements writing value expression `v` of field `f` as a token.
std::string write_call(const Field& f, const std::string& v) {
    switch (f.kind) {
        case FieldKind::Scalar:
            if (prim_is_float(f.prim))
                return "w.float_token(" + v + ", " + std::to_string(f.precision) + ", " + std::to_string(f.width) + ")";
            if (f.prim == Prim::Char) return "w.token(" + v + ")";
            if (f.prim == Prim::Bool) return "w.int_token(" + v + " ? 1 : 0)";
            return "w.int_token(" + v + ", " + std::to_string(f.base) + ", " + std::to_string(f.width) + ")";
        case FieldKind::Enum:
            return "w.int_token(static_cast<" + prim_type(f.enum_def->underlying) + ">(" + v + "), " +
                   std::to_string(f.base) + ", " + std::to_string(f.width) + ")";
        case FieldKind::String: return "w.token(std::string_view(" + v + "))";
        default: return "";
    }
}

/// print_to for a text struct (view = false) or view (view = true).
void emit_print(CodeWriter& w, const Message& m, bool view) {
    w.line(view ? "/// Human readable dump (see gmp/print.hpp); same output as the struct."
                : "/// Human readable dump (see gmp/print.hpp): gmp::print(msg, stdout).");
    w.open("void print_to(gmp::Printer& gmp_p) const noexcept {");
    w.line("gmp_p.begin(" + string_literal(m.name) + ");");
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        if (!f.has_member()) continue;
        const auto acc = "this->" + ident(f.name) + (view ? "()" : "");
        w.line("gmp_p.field(" + string_literal(f.name) + ", " + acc + ");");
        if (!has_print_extras(f)) continue;
        // unit / conversions only for a value that is present (and, for views, parsed)
        if (view) {
            w.open(f.optional ? "if (const auto gmp_v = " + acc + "; gmp_v && *gmp_v) {"
                              : "if (const auto gmp_v = " + acc + ") {");
            emit_print_extras(w, f, f.optional ? "(**gmp_v)" : "(*gmp_v)");
            w.close();
        } else if (f.optional) {
            w.open("if (" + acc + ") {");
            emit_print_extras(w, f, "(*" + acc + ")");
            w.close();
        } else {
            emit_print_extras(w, f, acc);
        }
    }
    w.line("gmp_p.end();");
    w.close();
}

void emit_struct(Ctx& c, const Message& m) {
    auto& w = c.w;
    const auto sep = sep_literal(m.separator);
    doc(w, m.doc);
    w.open("struct " + m.name + " {");
    for (const auto& f : m.fields) {
        if (!f->has_member()) continue;
        std::string comment = f->doc;
        if (!f->unit.empty()) comment += (comment.empty() ? "" : " ") + std::string("[") + f->unit + "]";
        w.line(member_type(m, *f) + " " + ident(f->name) + "{};" + (comment.empty() ? "" : "  ///< " + comment));
    }
    w.line();
    w.line("static constexpr std::size_t token_count = " + sz(m.token_count) + ";");
    w.line("static constexpr char separator = " + sep + ";");
    w.line();

    // read_text
    w.open("gmp::VoidResult read_text([[maybe_unused]] std::span<const std::string_view> tok) noexcept {");
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        if (f.kind == FieldKind::Pad) continue;
        const auto n = ident(f.name);
        const auto t = "t" + sz(f.token_index);
        w.open("{");
        w.line("const std::string_view " + t + " = " + sz(f.token_index) + " < tok.size() ? tok[" + sz(f.token_index) +
               "] : std::string_view{};");
        if (f.kind == FieldKind::String) {
            if (f.optional) {
                w.line("if (" + t + ".empty()) " + n + ".reset();");
                w.line("else if (!" + n + ".emplace().assign(" + t + ")) " + fail("OutOfRange"));
            } else {
                w.line("if (!" + n + ".assign(" + t + ")) " + fail("OutOfRange"));
            }
        } else {
            if (f.optional) w.open("if (" + t + ".empty()) { " + n + ".reset(); } else {");
            w.line("auto v = " + parse_call(m, f, t) + ";");
            w.line("if (!v) return gmp::fail(v.error());");
            if (f.const_value) w.line("if (*v != " + typed_literal(f, *f.const_value) + ") " + fail("BadConst"));
            else w.line(n + " = *v;");
            if (f.optional) w.close();
        }
        w.close();
    }
    w.line("return {};");
    w.close();

    // write_text
    w.open("gmp::VoidResult write_text([[maybe_unused]] gmp::text::TextWriter& w) const noexcept {");
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        const auto n = ident(f.name);
        if (f.kind == FieldKind::Pad) {
            w.open("for (int k = 0; k < " + sz(f.len.fixed) + "; ++k) {");
            w.line("if (auto e = w.empty_token(); !e) return e;");
            w.close();
            continue;
        }
        const auto value = f.const_value ? typed_literal(f, *f.const_value) : (f.optional ? "*" + n : n);
        if (f.optional) w.open("if (!" + n + ") { if (auto e = w.empty_token(); !e) return e; } else {");
        if (f.kind == FieldKind::Custom) {
            w.line("if (auto e = w.begin_token(); !e) return e;");
            w.line("if (auto e = " + lang_value(f.custom->lang, "encode") + "(w, " + value + "); !e) return e;");
        } else {
            if (f.kind == FieldKind::Enum && f.enum_def->strict && !f.const_value)
                w.line("if (!is_known(" + value + ")) return gmp::fail(gmp::Error::UnknownEnum);");
            if (!f.limit_checks.empty())
                w.line("if (" + limit_fail(f.limit_checks, value, f.prim, f.scale, f.offset_value) +
                       ") return gmp::fail(gmp::Error::ValueOutOfLimits);");
            w.line("if (auto e = " + write_call(f, value) + "; !e) return e;");
        }
        if (f.optional) w.close();
    }
    w.line("return {};");
    w.close();

    w.open("static gmp::Result<" + m.name + "> decode_text(std::string_view s) noexcept {");
    w.line("std::array<std::string_view, " + sz(m.token_count) + "> tok{};");
    w.line("const std::size_t n = gmp::text::split(s, separator, tok);");
    w.line(m.name + " m{};");
    w.line("if (auto e = m.read_text(std::span<const std::string_view>(tok.data(), std::min(n, tok.size()))); !e) return gmp::fail(e.error());");
    w.line("return m;");
    w.close();
    w.line("static gmp::Result<" + m.name + "> decode(gmp::bytes_view data) noexcept { return decode_text(gmp::as_string_view(data)); }");
    w.open("gmp::Result<std::size_t> encode(gmp::mut_bytes out) const noexcept {");
    w.line("gmp::text::TextWriter w(out, separator);");
    w.line("if (auto e = write_text(w); !e) return gmp::fail(e.error());");
    w.line("return w.position();");
    w.close();
    emit_print(w, m, false);
    w.line("bool operator==(const " + m.name + "&) const = default;");
    w.close("};");
    w.line();
}

void emit_view(Ctx& c, const Message& m) {
    auto& w = c.w;
    const auto name = view_name(m);
    const auto sep = sep_literal(m.separator);
    doc(w, "Zero-copy, lazily parsed view over a " + m.name + " text record. Fields are parsed on access.");
    w.open("class " + name + " {");
    w.label("public:");
    w.line("static constexpr std::size_t token_count = " + sz(m.token_count) + ";");
    w.line("static constexpr char separator = " + sep + ";");
    w.line();
    w.open("[[nodiscard]] static gmp::Result<" + name + "> from(std::string_view s) noexcept {");
    w.line(name + " v;");
    w.line("v.gmp_text_ = s;");
    w.line("v.gmp_ntok_ = gmp::text::split(s, separator, v.gmp_tok_);");
    w.line("return v;");
    w.close();
    w.line("[[nodiscard]] static gmp::Result<" + name + "> from(gmp::bytes_view data) noexcept { return from(gmp::as_string_view(data)); }");
    w.line("[[nodiscard]] static " + name + " unchecked(gmp::bytes_view data) noexcept { return *from(data); }");
    w.line();
    w.line("[[nodiscard]] std::string_view raw() const noexcept { return gmp_text_; }");
    w.line("/// Number of tokens actually present in the record (may differ from token_count).");
    w.line("[[nodiscard]] std::size_t present_tokens() const noexcept { return gmp_ntok_; }");
    w.line("[[nodiscard]] std::string_view token(std::size_t i) const noexcept { return i < token_count ? gmp_tok_[i] : std::string_view{}; }");
    w.line();
    for (const auto& fp : m.fields) {
        const auto& f = *fp;
        if (!f.has_member()) continue;
        const auto n = ident(f.name);
        const auto t = "gmp_tok_[" + sz(f.token_index) + "]";
        if (!f.doc.empty()) w.line("/// " + f.doc);
        if (f.kind == FieldKind::String) {
            if (f.optional)
                w.line("[[nodiscard]] std::optional<std::string_view> " + n + "() const noexcept { if (" + t +
                       ".empty()) return std::nullopt; return " + t + "; }");
            else
                w.line("[[nodiscard]] std::string_view " + n + "() const noexcept { return " + t + "; }");
            continue;
        }
        const auto vt = value_type(m, f);
        if (f.optional) {
            w.open("[[nodiscard]] gmp::Result<std::optional<" + vt + ">> " + n + "() const noexcept {");
            w.line("if (" + t + ".empty()) return std::optional<" + vt + ">{};");
            w.line("auto v = " + parse_call(m, f, t) + ";");
            w.line("if (!v) return gmp::fail(v.error());");
            w.line("return std::optional<" + vt + ">(std::move(*v));");
            w.close();
        } else {
            w.line("[[nodiscard]] gmp::Result<" + vt + "> " + n + "() const noexcept { return " + parse_call(m, f, t) + "; }");
        }
        if (f.kind == FieldKind::Scalar && !f.optional) {
            auto emit_conv = [&](const std::string& suffix, double sc, double of) {
                w.line("[[nodiscard]] gmp::Result<double> " + f.name + "_" + suffix + "() const noexcept { return " + n +
                       "().transform([](auto x) { return static_cast<double>(x) * " + double_literal(sc) + " + " +
                       double_literal(of) + "; }); }");
            };
            if (f.scale || f.offset_value) emit_conv("scaled", f.scale.value_or(1.0), f.offset_value.value_or(0.0));
            for (const auto& cv : f.conversions) emit_conv(cv.name, cv.scale.value_or(1.0), cv.offset_value.value_or(0.0));
        }
    }
    w.line();
    emit_print(w, m, true);
    if (m.want_struct) {
        w.line();
        w.line("[[nodiscard]] gmp::Result<" + m.name + "> to_struct() const noexcept { return " + m.name +
               "::decode_text(gmp_text_); }");
    }
    w.line();
    w.label("private:");
    w.line("std::string_view gmp_text_{};");
    w.line("std::array<std::string_view, " + sz(m.token_count) + "> gmp_tok_{};");
    w.line("std::size_t gmp_ntok_ = 0;");
    w.close("};");
    w.line();
}

} // namespace

void emit_text_message(Ctx& c, const Message& m) {
    if (m.want_struct) emit_struct(c, m);
    if (m.want_view) emit_view(c, m);
}

} // namespace gmpc::cpp
