// C++ emission for binary messages: bits group types, eager structs and lazy views.

#include <string>

#include "backend/cpp/cpp_common.hpp"
#include "schema/expr.hpp"

namespace gmpc::cpp {

namespace {

using namespace ir;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string dv(int depth) { return std::to_string(depth); }

const char* kFailPrefix = "return gmp::fail(";

std::string fail(const char* err) { return std::string(kFailPrefix) + "gmp::Error::" + err + ");"; }

/// Expression computing a field-level checksum over `covered` (a gmp::bytes_view expression).
std::string crc_call(const ChecksumSpec& c, const std::string& covered) {
    bool fn = false;
    const auto impl = checksum_impl(c, fn);
    return "static_cast<std::uint64_t>(" + (fn ? impl + "(" + covered + ")" : impl + "::compute(" + covered + ")") + ")";
}

/// Bytes of a scalar/enum value itself (without its field-level checksum).
std::size_t value_bytes(const Field& f) { return prim_size(f.prim); }

/// Enum field whose decode must reject values that are not declared (`strict: true`).
bool strict_enum(const Field& f) { return f.kind == FieldKind::Enum && f.enum_def && f.enum_def->strict; }

/// A bits group whose raw bytes need `check()`: constant bit fields or strict enum bit fields.
bool bits_need_check(const Field& f) {
    for (const auto& b : f.bits)
        if (b.const_value || (b.enum_def && b.enum_def->strict)) return true;
    return false;
}

/// Doc comment for a computed value: its doc, unit and defining expression.
void computed_doc(CodeWriter& w, const Computed& c) {
    if (!c.doc.empty()) doc(w, c.doc);
    w.line("/// = " + expr_to_string(*c.value) + (c.unit.empty() ? "" : "  [" + c.unit + "]"));
}

/// Engineering conversions of a numeric scalar: `scale`/`offset` as <name>_scaled, then each named conversion.
struct ConvAccessor {
    std::string suffix;
    double scale, offset;
    std::string unit;
};
std::vector<ConvAccessor> conversions_of(const Field& f) {
    std::vector<ConvAccessor> out;
    if (f.kind != FieldKind::Scalar) return out;
    if (f.scale || f.offset_value) out.push_back({"scaled", f.scale.value_or(1.0), f.offset_value.value_or(0.0), f.unit});
    for (const auto& c : f.conversions) out.push_back({c.name, c.scale.value_or(1.0), c.offset_value.value_or(0.0), c.unit});
    return out;
}

// -------------------------------------------------------------------------------------------------
// bits groups
// -------------------------------------------------------------------------------------------------
std::string bitfield_type(const BitField& b) { return b.enum_def ? b.enum_def->name : prim_type(b.prim); }

void emit_bits_type(Ctx& c, const Message& m, const Field& f) {
    auto& w = c.w;
    const auto name = bits_type_name(m, f);
    const auto order = bit_order_expr(f.bit_order);
    doc(w, f.doc.empty() ? "Bit fields of " + m.name + "." + f.name : f.doc);
    w.open("struct " + name + " {");
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value) continue;
        w.line(bitfield_type(b) + " " + ident(b.name) + "{};" + (b.doc.empty() ? "" : "  ///< " + b.doc));
    }
    w.line();
    w.line("static constexpr std::size_t wire_size = " + sz(f.bits_bytes) + ";");
    w.line();
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value || !(b.scale || b.offset_value)) continue;
        const auto scale = double_literal(b.scale.value_or(1.0));
        const auto off = double_literal(b.offset_value.value_or(0.0));
        w.line("[[nodiscard]] constexpr double " + ident(b.name) + "_scaled() const noexcept { return static_cast<double>(" +
               ident(b.name) + ") * " + scale + " + " + off + "; }");
    }
    w.open("static constexpr " + name + " unpack(const std::uint8_t* p) noexcept {");
    w.line(name + " g{};");
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value) continue;
        w.line("g." + ident(b.name) + " = gmp::get_bits<" + bitfield_type(b) + ">(p, " + sz(b.offset) + ", " +
               sz(b.bits) + ", " + order + ");");
    }
    w.line("return g;");
    w.close();
    w.open("constexpr void pack(std::uint8_t* p) const noexcept {");
    for (const auto& b : f.bits) {
        if (b.pad) {
            w.line("gmp::set_bits<std::uint64_t>(p, " + sz(b.offset) + ", " + sz(b.bits) + ", 0, " + order + ");");
        } else if (b.const_value) {
            w.line("gmp::set_bits<std::uint64_t>(p, " + sz(b.offset) + ", " + sz(b.bits) + ", " +
                   int_literal(*b.const_value) + "ull, " + order + ");");
        } else {
            w.line("gmp::set_bits<" + bitfield_type(b) + ">(p, " + sz(b.offset) + ", " + sz(b.bits) + ", " +
                   ident(b.name) + ", " + order + ");");
        }
    }
    w.close();
    w.open("static constexpr gmp::VoidResult check([[maybe_unused]] const std::uint8_t* p) noexcept {");
    for (const auto& b : f.bits) {
        if (b.const_value) {
            w.line("if (gmp::extract_bits(p, " + sz(b.offset) + ", " + sz(b.bits) + ", " + order + ") != " +
                   int_literal(*b.const_value) + "ull) " + fail("BadConst"));
        } else if (b.enum_def && b.enum_def->strict) {
            w.line("if (!is_known(gmp::get_bits<" + bitfield_type(b) + ">(p, " + sz(b.offset) + ", " + sz(b.bits) + ", " +
                   order + "))) " + fail("UnknownEnum"));
        }
    }
    w.line("return {};");
    w.close();
    w.line("bool operator==(const " + name + "&) const = default;");
    w.close("};");
    w.line();
}

// -------------------------------------------------------------------------------------------------
// struct: read
// -------------------------------------------------------------------------------------------------
class StructEmitter {
public:
    StructEmitter(Ctx& c, const Message& m) : c_(c), w_(c.w), m_(m) {
        base_ref_ = [](const Field& f, const BitField* b) {
            if (f.const_value) return typed_literal(f, *f.const_value);
            std::string s = "this->" + ident(f.name);
            if (b) s += "." + ident(b->name);
            return s;
        };
        // On encode, a derived length/count field takes its value from the container it describes,
        // so expressions must see that value rather than the (possibly stale) member.
        write_ref_ = [this](const Field& f, const BitField* b) {
            if (f.derived_from && !b) return "static_cast<" + prim_type(f.prim) + ">(" + derived_count(f) + ")";
            return base_ref_(f, b);
        };
        ref_ = base_ref_;
    }

    static std::string derived_count(const Field& f) {
        const auto n = "this->" + ident(f.derived_from->name);
        return f.derived_from->when ? "(" + n + " ? " + n + "->size() : 0u)" : n + ".size()";
    }

    void emit() {
        doc(w_, m_.doc);
        w_.open("struct " + m_.name + " {");
        for (const auto& f : m_.fields) {
            if (!f->has_member()) continue;
            std::string comment = f->doc;
            if (!f->unit.empty()) comment += (comment.empty() ? "" : " ") + std::string("[") + f->unit + "]";
            if (f->derived_from)
                comment += (comment.empty() ? "" : "; ") + std::string("set automatically from ") + f->derived_from->name + " on encode";
            w_.line(member_type(m_, *f) + " " + ident(f->name) + "{};" + (comment.empty() ? "" : "  ///< " + comment));
        }
        w_.line();
        emit_constants();
        emit_scaled();
        emit_computed();
        emit_read();
        emit_write();
        emit_wrappers();
        w_.line("bool operator==(const " + m_.name + "&) const = default;");
        w_.close("};");
        w_.line();
    }

private:
    void emit_constants() {
        w_.line("static constexpr std::size_t min_wire_size = " + sz(m_.min_size) + ";");
        if (m_.max_size) w_.line("static constexpr std::size_t max_wire_size = " + sz(*m_.max_size) + ";");
        if (m_.static_size) w_.line("static constexpr std::size_t wire_size = " + sz(*m_.static_size) + ";");
        for (const auto& f : m_.fields)
            if (f->const_value)
                w_.line("static constexpr " + element_type(m_, *f) + " " + ident(f->name) + "_value = " +
                        typed_literal(*f, *f->const_value) + ";");
        w_.line();
    }

    void emit_scaled() {
        bool any = false;
        for (const auto& f : m_.fields) {
            if (!f->has_member()) continue;
            const auto n = ident(f->name);
            const auto t = prim_type(f->prim);
            for (const auto& cv : conversions_of(*f)) {
                any = true;
                const auto scale = double_literal(cv.scale);
                const auto off = double_literal(cv.offset);
                const auto acc = f->name + "_" + cv.suffix;
                if (!cv.unit.empty() && cv.suffix != "scaled") w_.line("/// " + f->name + " in " + cv.unit);
                if (f->when) {
                    w_.line("[[nodiscard]] std::optional<double> " + acc + "() const noexcept { if (!" + n +
                            ") return std::nullopt; return static_cast<double>(*" + n + ") * " + scale + " + " + off + "; }");
                    continue;
                }
                w_.line("[[nodiscard]] constexpr double " + acc + "() const noexcept { return static_cast<double>(" + n +
                        ") * " + scale + " + " + off + "; }");
                const auto conv = prim_is_float(f->prim) ? "static_cast<" + t + ">((v - " + off + ") / " + scale + ")"
                                                         : "static_cast<" + t + ">(std::llround((v - " + off + ") / " + scale + "))";
                w_.line("void set_" + acc + "(double v) noexcept { " + n + " = " + conv + "; }");
            }
        }
        if (any) w_.line();
    }

    void emit_computed() {
        for (const auto& c : m_.computed) {
            const auto t = prim_type(c.prim);
            const auto n = ident(c.name);
            computed_doc(w_, c);
            w_.line("[[nodiscard]] constexpr " + t + " " + n + "() const noexcept { return static_cast<" + t + ">(" +
                    expr(*c.value, base_ref_) + "); }");
            if (c.setters.empty()) continue;
            w_.line(std::string("/// Sets the fields ") + (c.setter_derived ? "packed into " : "listed in `set` for ") + c.name +
                    " so that " + n + "() == v" + (c.setter_derived ? " (for values that fit in those fields)." : "."));
            w_.open("constexpr void set_" + c.name + "(" + t + " v) noexcept {");
            w_.line("[[maybe_unused]] const std::int64_t value = static_cast<std::int64_t>(v);");
            for (const auto& [f, e] : c.setters)
                w_.line("this->" + ident(f->name) + " = static_cast<" + element_type(m_, *f) + ">(" + expr(*e, base_ref_) + ");");
            w_.close();
        }
        if (!m_.computed.empty()) w_.line();
    }

    void emit_read() {
        w_.open("gmp::VoidResult read([[maybe_unused]] gmp::ByteReader& r) noexcept {");
        for (const auto& f : m_.fields) {
            const auto dst = f->has_member() ? ident(f->name) : std::string();
            if (f->when) {
                w_.open("if (" + expr(*f->when, ref_) + ") {");
                w_.line(dst + ".emplace();");
                read_value(*f, "(*" + dst + ")", 0);
                w_.dedent();
                w_.open("} else {");
                w_.line(dst + ".reset();");
                w_.close();
            } else {
                read_value(*f, dst, 0);
            }
        }
        w_.line("return {};");
        w_.close();
    }

    void read_value(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: {
                const auto t = element_type(m_, f);
                w_.open("{");
                if (f.crc) {
                    // value bytes followed by their checksum
                    const auto n = sz(value_bytes(f));
                    w_.line("auto b" + d + " = r.read_bytes(" + sz(value_bytes(f) + f.crc->size) + ");");
                    w_.line("if (!b" + d + ") return gmp::fail(b" + d + ".error());");
                    w_.line("if (gmp::load_uint(b" + d + "->data() + " + n + ", " + sz(f.crc->size) + ", " +
                            endian_expr(f.crc->endian) + ") != " + crc_call(*f.crc, "b" + d + "->first(" + n + ")") + ") " +
                            fail("BadChecksum"));
                    w_.line("const auto v" + d + " = gmp::load<" + t + ">(b" + d + "->data(), " + endian_expr(f.endian) + ");");
                } else {
                    w_.line("auto v" + d + " = r.read<" + t + ">(" + endian_expr(f.endian) + ");");
                    w_.line("if (!v" + d + ") return gmp::fail(v" + d + ".error());");
                }
                const auto val = (f.crc ? "v" : "*v") + d;
                if (strict_enum(f)) w_.line("if (!is_known(" + val + ")) " + fail("UnknownEnum"));
                if (f.const_value) w_.line("if (" + val + " != " + typed_literal(f, *f.const_value) + ") " + fail("BadConst"));
                else if (!dst.empty()) w_.line(dst + " = " + val + ";");
                w_.close();
                break;
            }
            case FieldKind::Bits: {
                const auto t = bits_type_name(m_, f);
                w_.open("{");
                w_.line("auto b" + d + " = r.read_bytes(" + sz(f.bits_bytes) + ");");
                w_.line("if (!b" + d + ") return gmp::fail(b" + d + ".error());");
                w_.line("if (auto c" + d + " = " + t + "::check(b" + d + "->data()); !c" + d + ") return c" + d + ";");
                w_.line(dst + " = " + t + "::unpack(b" + d + "->data());");
                w_.close();
                break;
            }
            case FieldKind::Pad:
                w_.line("if (auto e" + d + " = r.skip(" + sz(f.len.fixed) + "); !e" + d + ") return e" + d + ";");
                break;
            case FieldKind::Bytes:
            case FieldKind::String: read_blob(f, dst, depth); break;
            case FieldKind::Array: read_array(f, dst, depth); break;
            case FieldKind::Message:
                w_.line("if (auto e" + d + " = " + dst + ".read(r); !e" + d + ") return e" + d + ";");
                break;
            case FieldKind::Custom: {
                const auto fn = lang_value(f.custom->lang, "decode");
                w_.open("{");
                if (f.custom->wire_size) {
                    w_.line("auto b" + d + " = r.read_bytes(" + sz(*f.custom->wire_size) + ");");
                    w_.line("if (!b" + d + ") return gmp::fail(b" + d + ".error());");
                    w_.line("gmp::ByteReader sub" + d + "(*b" + d + ");");
                    w_.line("auto v" + d + " = " + fn + "(sub" + d + ");");
                } else {
                    w_.line("auto v" + d + " = " + fn + "(r);");
                }
                w_.line("if (!v" + d + ") return gmp::fail(v" + d + ".error());");
                w_.line(dst + " = std::move(*v" + d + ");");
                w_.close();
                break;
            }
        }
    }

    void read_blob(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        const bool str = f.kind == FieldKind::String;
        const auto b = "b" + d;
        w_.open("{");
        switch (f.len.kind) {
            case LenKind::Fixed: w_.line("auto " + b + " = r.read_bytes(" + sz(f.len.fixed) + ");"); break;
            case LenKind::Expr:
                w_.line("const std::int64_t n" + d + " = " + expr(*f.len.expr, ref_) + ";");
                w_.line("if (n" + d + " < 0 || n" + d + " > " + sz(f.max.value_or(0)) + ") " + fail("OutOfRange"));
                w_.line("auto " + b + " = r.read_bytes(static_cast<std::size_t>(n" + d + "));");
                break;
            case LenKind::Remaining:
                w_.line("if (r.remaining() > " + sz(f.max.value_or(0)) + ") " + fail("OutOfRange"));
                w_.line("auto " + b + " = r.read_bytes(r.remaining());");
                break;
            case LenKind::Terminator:
                w_.line("auto " + b + " = r.read_until(" + sz(f.len.terminator) + ");");
                break;
            case LenKind::None: break;
        }
        w_.line("if (!" + b + ") return gmp::fail(" + b + ".error());");
        if (f.len.kind == LenKind::Terminator)
            w_.line("if (" + b + "->size() > " + sz(f.max.value_or(0)) + ") " + fail("OutOfRange"));
        if (str) {
            if (f.len.kind == LenKind::Fixed)
                w_.line("(void)" + dst + ".assign(gmp::strip_padding(gmp::as_string_view(*" + b + "), " +
                        int_literal(static_cast<unsigned char>(f.pad_char)) + "));");
            else
                w_.line("(void)" + dst + ".assign(gmp::as_string_view(*" + b + "));");
        } else {
            if (f.len.kind == LenKind::Fixed)
                w_.line("std::copy(" + b + "->begin(), " + b + "->end(), " + dst + ".begin());");
            else
                w_.line("(void)" + dst + ".assign(*" + b + ");");
        }
        w_.close();
    }

    void read_array(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        const auto i = "i" + d;
        const auto& e = *f.elem;
        switch (f.len.kind) {
            case LenKind::Fixed:
                w_.open("for (std::size_t " + i + " = 0; " + i + " < " + sz(f.len.fixed) + "; ++" + i + ") {");
                read_value(e, dst + "[" + i + "]", depth + 1);
                w_.close();
                break;
            case LenKind::Expr:
                w_.open("{");
                w_.line("const std::int64_t n" + d + " = " + expr(*f.len.expr, ref_) + ";");
                w_.line("if (n" + d + " < 0 || n" + d + " > " + sz(f.max.value_or(0)) + ") " + fail("OutOfRange"));
                w_.line("(void)" + dst + ".resize(static_cast<std::size_t>(n" + d + "));");
                w_.open("for (std::size_t " + i + " = 0; " + i + " < " + dst + ".size(); ++" + i + ") {");
                read_value(e, dst + "[" + i + "]", depth + 1);
                w_.close();
                w_.close();
                break;
            case LenKind::Remaining:
                w_.line(dst + ".clear();");
                w_.open("while (!r.empty()) {");
                w_.line("if (" + dst + ".full()) " + fail("OutOfRange"));
                w_.line("(void)" + dst + ".resize(" + dst + ".size() + 1);");
                read_value(e, dst + ".back()", depth + 1);
                w_.close();
                break;
            default: break;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // struct: write
    // ---------------------------------------------------------------------------------------------
    void emit_write() {
        ref_ = write_ref_;  // expressions on the encode side see derived values
        w_.open("gmp::VoidResult write([[maybe_unused]] gmp::ByteWriter& w) const noexcept {");
        for (const auto& f : m_.fields) {
            const auto src = f->has_member() ? ident(f->name) : std::string();
            if (f->when) {
                w_.open("if (" + expr(*f->when, ref_) + ") {");
                w_.line("if (!" + src + ") " + fail("MissingField"));
                write_value(*f, "(*" + src + ")", 0);
                w_.close();
            } else {
                write_value(*f, src, 0);
            }
        }
        w_.line("return {};");
        w_.close();
        ref_ = base_ref_;
    }

    void write_value(const Field& f, const std::string& src, int depth) {
        const auto d = dv(depth);
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: {
                const auto t = element_type(m_, f);
                std::string val = src;
                if (f.const_value) {
                    val = typed_literal(f, *f.const_value);
                } else if (f.derived_from) {
                    const auto count = derived_count(f);
                    w_.line("if (" + count + " > static_cast<std::size_t>(std::numeric_limits<" + t + ">::max())) " +
                            fail("OutOfRange"));
                    val = "static_cast<" + t + ">(" + count + ")";
                } else if (strict_enum(f)) {
                    w_.line("if (!is_known(" + val + ")) " + fail("UnknownEnum"));
                }
                w_.line("if (auto e" + d + " = w.write<" + t + ">(" + val + ", " + endian_expr(f.endian) + "); !e" + d +
                        ") return e" + d + ";");
                if (f.crc)
                    w_.line("if (auto e" + d + " = w.write_uint(" +
                            crc_call(*f.crc, "w.written().last(" + sz(value_bytes(f)) + ")") + ", " + sz(f.crc->size) + ", " +
                            endian_expr(f.crc->endian) + "); !e" + d + ") return e" + d + ";");
                break;
            }
            case FieldKind::Bits:
                for (const auto& b : f.bits)
                    if (!b.pad && !b.const_value && b.enum_def && b.enum_def->strict)
                        w_.line("if (!is_known(" + src + "." + ident(b.name) + ")) " + fail("UnknownEnum"));
                w_.open("{");
                w_.line("auto at" + d + " = w.reserve(" + sz(f.bits_bytes) + ");");
                w_.line("if (!at" + d + ") return gmp::fail(at" + d + ".error());");
                w_.line(src + ".pack(w.buffer().data() + *at" + d + ");");
                w_.close();
                break;
            case FieldKind::Pad:
                w_.line("if (auto e" + d + " = w.fill(0, " + sz(f.len.fixed) + "); !e" + d + ") return e" + d + ";");
                break;
            case FieldKind::Bytes:
            case FieldKind::String: write_blob(f, src, depth); break;
            case FieldKind::Array: write_array(f, src, depth); break;
            case FieldKind::Message:
                w_.line("if (auto e" + d + " = " + src + ".write(w); !e" + d + ") return e" + d + ";");
                break;
            case FieldKind::Custom: {
                const auto fn = lang_value(f.custom->lang, "encode");
                w_.open("{");
                w_.line("[[maybe_unused]] const auto p" + d + " = w.position();");
                w_.line("if (auto e" + d + " = " + fn + "(w, " + src + "); !e" + d + ") return e" + d + ";");
                if (f.custom->wire_size)
                    w_.line("if (w.position() - p" + d + " != " + sz(*f.custom->wire_size) + ") " + fail("InvalidValue"));
                w_.close();
                break;
            }
        }
    }

    void length_check(const Field& f, const std::string& size_expr) {
        if (f.len.kind == LenKind::Expr && !is_derived_length(f))
            w_.line("if (static_cast<std::int64_t>(" + size_expr + ") != " + expr(*f.len.expr, ref_) + ") " +
                    fail("BadLength"));
    }

    void write_blob(const Field& f, const std::string& src, int depth) {
        const auto d = dv(depth);
        const bool str = f.kind == FieldKind::String;
        w_.open("{");
        if (str) {
            w_.line("const std::string_view s" + d + " = " + src + ".view();");
            w_.line("const gmp::bytes_view b" + d + " = gmp::as_bytes(s" + d + ".data(), s" + d + ".size());");
        } else {
            w_.line("const gmp::bytes_view b" + d + "(" + src + ".data(), " + src + ".size());");
        }
        length_check(f, "b" + d + ".size()");
        if (f.len.kind == LenKind::Fixed && str) {
            w_.line("if (b" + d + ".size() > " + sz(f.len.fixed) + ") " + fail("OutOfRange"));
        }
        w_.line("if (auto e" + d + " = w.write_bytes(b" + d + "); !e" + d + ") return e" + d + ";");
        if (f.len.kind == LenKind::Fixed && str)
            w_.line("if (auto e" + d + " = w.fill(" + int_literal(static_cast<unsigned char>(f.pad_char)) + ", " +
                    sz(f.len.fixed) + " - b" + d + ".size()); !e" + d + ") return e" + d + ";");
        if (f.len.kind == LenKind::Terminator)
            w_.line("if (auto e" + d + " = w.write<std::uint8_t>(" + sz(f.len.terminator) + "); !e" + d + ") return e" + d + ";");
        w_.close();
    }

    void write_array(const Field& f, const std::string& src, int depth) {
        const auto d = dv(depth);
        length_check(f, src + ".size()");
        w_.open("for (const auto& x" + d + " : " + src + ") {");
        write_value(*f.elem, "x" + d, depth + 1);
        w_.close();
    }

    void emit_wrappers() {
        w_.open("static gmp::Result<" + m_.name + "> decode(gmp::bytes_view data) noexcept {");
        w_.line(m_.name + " m{};");
        w_.line("gmp::ByteReader r(data);");
        w_.line("if (auto e = m.read(r); !e) return gmp::fail(e.error());");
        w_.line("return m;");
        w_.close();
        w_.open("gmp::Result<std::size_t> encode(gmp::mut_bytes out) const noexcept {");
        w_.line("gmp::ByteWriter w(out);");
        w_.line("if (auto e = write(w); !e) return gmp::fail(e.error());");
        w_.line("return w.position();");
        w_.close();
    }

    Ctx& c_;
    CodeWriter& w_;
    const Message& m_;
    RefFn base_ref_, ref_, write_ref_;
};

// -------------------------------------------------------------------------------------------------
// view
// -------------------------------------------------------------------------------------------------
class ViewEmitter {
public:
    ViewEmitter(Ctx& c, const Message& m) : c_(c), w_(c.w), m_(m), name_(view_name(m)) {
        dynamic_ = !m.fixed_layout;
        // accessor references from inside from(): call the accessor on the view under construction.
        walk_ref_ = [](const Field& f, const BitField* b) {
            if (f.const_value) return typed_literal(f, *f.const_value);
            std::string s = "v." + ident(f.name) + "()";
            if (b) s += "." + ident(b->name);
            return s;
        };
        acc_ref_ = [](const Field& f, const BitField* b) {
            if (f.const_value) return typed_literal(f, *f.const_value);
            std::string s = ident(f.name) + "()";
            if (b) s += "." + ident(b->name);
            return s;
        };
        std::size_t k = 0;
        for (const auto& f : m.fields)
            if (f->when) cond_bit_[f.get()] = k++;
    }

    void emit() {
        doc(w_, "Zero-copy view over an encoded " + m_.name + ". Holds a reference to the input buffer.");
        w_.open("class " + name_ + " {");
        w_.label("public:");
        w_.line("static constexpr std::size_t min_wire_size = " + sz(m_.min_size) + ";");
        if (m_.static_size) w_.line("static constexpr std::size_t wire_size_v = " + sz(*m_.static_size) + ";");
        w_.line();
        emit_from();
        emit_unchecked();
        w_.line("[[nodiscard]] gmp::bytes_view data() const noexcept { return gmp_data_; }");
        w_.line("[[nodiscard]] std::size_t wire_size() const noexcept { return gmp_data_.size(); }");
        w_.line();
        for (const auto& f : m_.fields) emit_accessor(*f);
        emit_computed_accessors();
        if (m_.want_struct) {
            w_.line();
            w_.line("/// Decode into the eager struct.");
            w_.line("[[nodiscard]] gmp::Result<" + m_.name + "> to_struct() const noexcept { return " + m_.name +
                    "::decode(gmp_data_); }");
        }
        w_.line();
        w_.label("private:");
        w_.line("gmp::bytes_view gmp_data_{};");
        if (dynamic_) w_.line("std::array<std::uint32_t, " + sz(m_.fields.size() + 1) + "> gmp_off_{};");
        if (m_.conditional_count) w_.line("std::uint64_t gmp_present_ = 0;");
        w_.close("};");
        w_.line();
    }

private:
    std::string off(const Field& f) const {
        if (f.static_offset) return sz(*f.static_offset);
        return "gmp_off_[" + sz(f.index) + "]";
    }
    std::string size(const Field& f) const {
        if (f.static_size) return sz(*f.static_size);
        return "(gmp_off_[" + sz(f.index + 1) + "] - gmp_off_[" + sz(f.index) + "])";
    }

    /// Validation for a field with static size located at byte offset expression `o` of `data`.
    void validate_static(const Field& f, const std::string& o) {
        const auto p = "data.data() + " + o;
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum:
                if (f.crc) {
                    const auto n = sz(value_bytes(f));
                    w_.line("if (gmp::load_uint(" + p + " + " + n + ", " + sz(f.crc->size) + ", " + endian_expr(f.crc->endian) +
                            ") != " + crc_call(*f.crc, "gmp::bytes_view(" + p + ", " + n + ")") + ") " + fail("BadChecksum"));
                }
                if (f.const_value)
                    w_.line("if (gmp::load<" + element_type(m_, f) + ">(" + p + ", " + endian_expr(f.endian) + ") != " +
                            typed_literal(f, *f.const_value) + ") " + fail("BadConst"));
                else if (strict_enum(f))
                    w_.line("if (!is_known(gmp::load<" + element_type(m_, f) + ">(" + p + ", " + endian_expr(f.endian) +
                            "))) " + fail("UnknownEnum"));
                break;
            case FieldKind::Bits:
                if (bits_need_check(f))
                    w_.line("if (auto c = " + bits_type_name(m_, f) + "::check(" + p + "); !c) return gmp::fail(c.error());");
                break;
            case FieldKind::Message:
                if (f.message->needs_validation)
                    w_.line("if (auto c = " + view_name(*f.message) + "::from(data.subspan(" + o + ", " +
                            sz(*f.static_size) + ")); !c) return gmp::fail(c.error());");
                break;
            case FieldKind::Array:
                if (f.elem->kind == FieldKind::Message && f.elem->message->needs_validation) {
                    const auto es = sz(*f.elem->static_size);
                    w_.open("for (std::size_t k = 0; k < " + sz(f.len.fixed) + "; ++k) {");
                    w_.line("if (auto c = " + view_name(*f.elem->message) + "::from(data.subspan(" + o + " + k * " + es +
                            ", " + es + ")); !c) return gmp::fail(c.error());");
                    w_.close();
                } else if (f.elem->const_value) {
                    // arrays of constants are unusual; validate each element
                    const auto es = sz(*f.elem->static_size);
                    w_.open("for (std::size_t k = 0; k < " + sz(f.len.fixed) + "; ++k) {");
                    w_.line("if (gmp::load<" + element_type(m_, *f.elem) + ">(" + p + " + k * " + es + ", " +
                            endian_expr(f.elem->endian) + ") != " + typed_literal(*f.elem, *f.elem->const_value) + ") " +
                            fail("BadConst"));
                    w_.close();
                } else if (strict_enum(*f.elem)) {
                    emit_enum_array_check(*f.elem, p, sz(f.len.fixed));
                }
                break;
            default: break;
        }
    }

    /// Reject undeclared values in `count` strict-enum elements starting at pointer expression `p`.
    void emit_enum_array_check(const Field& e, const std::string& p, const std::string& count) {
        const auto es = sz(prim_size(e.prim));
        w_.open("for (std::size_t k = 0; k < " + count + "; ++k) {");
        w_.line("if (!is_known(gmp::load<" + element_type(m_, e) + ">(" + p + " + k * " + es + ", " + endian_expr(e.endian) +
                "))) " + fail("UnknownEnum"));
        w_.close();
    }

    void emit_from() {
        w_.line("/// Validate `data` and build a view. Trailing bytes beyond the message are ignored.");
        w_.open("[[nodiscard]] static gmp::Result<" + name_ + "> from(gmp::bytes_view data) noexcept {");
        if (!dynamic_) {
            w_.line("if (data.size() < " + sz(*m_.static_size) + ") " + fail("Truncated"));
            for (const auto& f : m_.fields) validate_static(*f, sz(*f->static_offset));
            w_.line(name_ + " v;");
            w_.line("v.gmp_data_ = data.first(" + sz(*m_.static_size) + ");");
            w_.line("return v;");
            w_.close();
            return;
        }
        w_.line(name_ + " v;");
        w_.line("v.gmp_data_ = data;");
        w_.line("std::size_t pos = 0;");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            w_.line("v.gmp_off_[" + sz(f.index) + "] = static_cast<std::uint32_t>(pos);");
            if (f.when) {
                w_.open("if (" + expr(*f.when, walk_ref_) + ") {");
                w_.line("v.gmp_present_ |= std::uint64_t{1} << " + sz(cond_bit_.at(&f)) + ";");
            }
            walk_field(f);
            if (f.when) w_.close();
        }
        w_.line("v.gmp_off_[" + sz(m_.fields.size()) + "] = static_cast<std::uint32_t>(pos);");
        w_.line("v.gmp_data_ = data.first(pos);");
        w_.line("return v;");
        w_.close();
    }

    /// Emit size computation for one field in the dynamic walk (advances `pos`).
    void walk_field(const Field& f) {
        // static-size content (also covers conditional fields whose payload has a fixed size)
        std::optional<std::size_t> fixed = f.static_size;
        if (!fixed && f.when) {
            Field tmp = f;
            tmp.when = nullptr;
            // size if present
            switch (f.kind) {
                case FieldKind::Scalar: case FieldKind::Enum: fixed = prim_size(f.prim) + (f.crc ? f.crc->size : 0); break;
                case FieldKind::Bits: fixed = f.bits_bytes; break;
                case FieldKind::Pad: fixed = f.len.fixed; break;
                case FieldKind::Message: fixed = f.message->static_size; break;
                case FieldKind::Custom: fixed = f.custom->wire_size; break;
                case FieldKind::Bytes: case FieldKind::String:
                    if (f.len.kind == LenKind::Fixed) fixed = f.len.fixed;
                    break;
                case FieldKind::Array:
                    if (f.len.kind == LenKind::Fixed && f.elem->static_size) fixed = f.len.fixed * *f.elem->static_size;
                    break;
            }
        }
        if (fixed) {
            w_.line("if (data.size() - pos < " + sz(*fixed) + ") " + fail("Truncated"));
            if (f.kind == FieldKind::Message && f.message->needs_validation) {
                w_.line("if (auto c = " + view_name(*f.message) + "::from(data.subspan(pos, " + sz(*fixed) +
                        ")); !c) return gmp::fail(c.error());");
            } else if (f.kind == FieldKind::Array && f.elem->kind == FieldKind::Message && f.elem->message->needs_validation) {
                const auto es = sz(*f.elem->static_size);
                w_.open("for (std::size_t k = 0; k < " + sz(f.len.fixed) + "; ++k) {");
                w_.line("if (auto c = " + view_name(*f.elem->message) + "::from(data.subspan(pos + k * " + es + ", " + es +
                        ")); !c) return gmp::fail(c.error());");
                w_.close();
            } else {
                validate_static(f, "pos");
            }
            w_.line("pos += " + sz(*fixed) + ";");
            return;
        }
        switch (f.kind) {
            case FieldKind::Bytes:
            case FieldKind::String:
                switch (f.len.kind) {
                    case LenKind::Expr:
                        w_.open("{");
                        w_.line("const std::int64_t n = " + expr(*f.len.expr, walk_ref_) + ";");
                        w_.line("if (n < 0 || static_cast<std::uint64_t>(n) > data.size() - pos) " + fail("Truncated"));
                        w_.line("pos += static_cast<std::size_t>(n);");
                        w_.close();
                        break;
                    case LenKind::Remaining: w_.line("pos = data.size();"); break;
                    case LenKind::Terminator:
                        w_.open("{");
                        w_.line("std::size_t k = pos;");
                        w_.line("while (k < data.size() && data[k] != " + sz(f.len.terminator) + ") ++k;");
                        w_.line("if (k == data.size()) " + fail("Truncated"));
                        w_.line("pos = k + 1;");
                        w_.close();
                        break;
                    default: break;
                }
                break;
            case FieldKind::Array: walk_array(f); break;
            case FieldKind::Message:
                w_.open("{");
                w_.line("auto c = " + view_name(*f.message) + "::from(data.subspan(pos));");
                w_.line("if (!c) return gmp::fail(c.error());");
                w_.line("pos += c->wire_size();");
                w_.close();
                break;
            case FieldKind::Custom:
                w_.open("{");
                w_.line("gmp::ByteReader cr(data.subspan(pos));");
                w_.line("auto c = " + lang_value(f.custom->lang, "decode") + "(cr);");
                w_.line("if (!c) return gmp::fail(c.error());");
                w_.line("pos += cr.position();");
                w_.close();
                break;
            default: break;
        }
    }

    void walk_array(const Field& f) {
        const auto& e = *f.elem;
        const bool elem_msg = e.kind == FieldKind::Message;
        const auto es = e.static_size;
        const bool validate = elem_msg && e.message->needs_validation;
        w_.open("{");
        if (f.len.kind == LenKind::Remaining) {
            if (es && !validate) {
                w_.line("if ((data.size() - pos) % " + sz(*es) + " != 0) " + fail("BadLength"));
                if (strict_enum(e)) emit_enum_array_check(e, "data.data() + pos", "(data.size() - pos) / " + sz(*es));
                w_.line("pos = data.size();");
            } else {
                w_.open("while (pos < data.size()) {");
                w_.line("auto c = " + view_name(*e.message) + "::from(data.subspan(pos));");
                w_.line("if (!c) return gmp::fail(c.error());");
                w_.line("pos += c->wire_size();");
                w_.close();
            }
        } else {
            if (f.len.kind == LenKind::Fixed) w_.line("const std::int64_t n = " + sz(f.len.fixed) + ";");
            else w_.line("const std::int64_t n = " + expr(*f.len.expr, walk_ref_) + ";");
            w_.line("if (n < 0) " + fail("BadLength"));
            if (es && !validate) {
                w_.line("if (static_cast<std::uint64_t>(n) * " + sz(*es) + " > data.size() - pos) " + fail("Truncated"));
                if (strict_enum(e)) emit_enum_array_check(e, "data.data() + pos", "static_cast<std::size_t>(n)");
                w_.line("pos += static_cast<std::size_t>(n) * " + sz(*es) + ";");
            } else {
                w_.open("for (std::int64_t k = 0; k < n; ++k) {");
                w_.line("auto c = " + view_name(*e.message) + "::from(data.subspan(pos));");
                w_.line("if (!c) return gmp::fail(c.error());");
                w_.line("pos += c->wire_size();");
                w_.close();
            }
        }
        w_.close();
    }

    void emit_unchecked() {
        w_.line("/// Build a view over data already validated by from() (no checks).");
        w_.open("[[nodiscard]] static " + name_ + " unchecked(gmp::bytes_view data) noexcept {");
        if (!dynamic_) {
            w_.line(name_ + " v;");
            w_.line("v.gmp_data_ = data.first(" + sz(*m_.static_size) + ");");
            w_.line("return v;");
        } else {
            w_.line("return *from(data);");
        }
        w_.close();
        w_.line();
    }

    std::string accessor_type(const Field& f) const {
        switch (f.kind) {
            case FieldKind::Scalar: case FieldKind::Enum: return element_type(m_, f);
            case FieldKind::Bits: return bits_type_name(m_, f);
            case FieldKind::Bytes: return "gmp::bytes_view";
            case FieldKind::String: return "std::string_view";
            case FieldKind::Message: return view_name(*f.message);
            case FieldKind::Custom: return "gmp::Result<" + lang_value(f.custom->lang, "type") + ">";
            case FieldKind::Array: {
                const auto& e = *f.elem;
                if (e.kind == FieldKind::Message) {
                    if (e.static_size) return "gmp::FixedArrayView<" + view_name(*e.message) + ", " + sz(*e.static_size) + ">";
                    return "gmp::SeqArrayView<" + view_name(*e.message) + ">";
                }
                return "gmp::ScalarArrayView<" + element_type(m_, e) + ">";
            }
            default: return "void";
        }
    }

    std::string accessor_body(const Field& f) const {
        const auto o = off(f);
        const auto z = size(f);
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum:
                return "gmp::load<" + element_type(m_, f) + ">(gmp_data_.data() + " + o + ", " + endian_expr(f.endian) + ")";
            case FieldKind::Bits: return bits_type_name(m_, f) + "::unpack(gmp_data_.data() + " + o + ")";
            case FieldKind::Bytes: return "gmp_data_.subspan(" + o + ", " + z + ")";
            case FieldKind::String: {
                if (f.len.kind == LenKind::Fixed)
                    return "gmp::strip_padding(gmp::as_string_view(gmp_data_.subspan(" + o + ", " + z + ")), " +
                           int_literal(static_cast<unsigned char>(f.pad_char)) + ")";
                if (f.len.kind == LenKind::Terminator)
                    return "gmp::as_string_view(gmp_data_.subspan(" + o + ", " + z + " - 1))";
                return "gmp::as_string_view(gmp_data_.subspan(" + o + ", " + z + "))";
            }
            case FieldKind::Message: return view_name(*f.message) + "::unchecked(gmp_data_.subspan(" + o + ", " + z + "))";
            case FieldKind::Custom:
                return "[&] { gmp::ByteReader r(gmp_data_.subspan(" + o + ", " + z + ")); return " +
                       lang_value(f.custom->lang, "decode") + "(r); }()";
            case FieldKind::Array: {
                const auto& e = *f.elem;
                const auto sub = "gmp_data_.subspan(" + o + ", " + z + ")";
                if (e.kind == FieldKind::Message) {
                    if (e.static_size) return accessor_type(f) + "(" + sub + ", " + z + " / " + sz(*e.static_size) + ")";
                    return accessor_type(f) + "(" + sub + ")";
                }
                return accessor_type(f) + "(" + sub + ", " + z + " / " + sz(prim_size(e.prim)) + ", " +
                       endian_expr(e.endian) + ")";
            }
            default: return "";
        }
    }

    void emit_computed_accessors() {
        for (const auto& c : m_.computed) {
            const auto t = prim_type(c.prim);
            computed_doc(w_, c);
            w_.line("[[nodiscard]] " + t + " " + ident(c.name) + "() const noexcept { return static_cast<" + t + ">(" +
                    expr(*c.value, acc_ref_) + "); }");
        }
    }

    void emit_accessor(const Field& f) {
        if (!f.has_member()) return;
        const auto n = ident(f.name);
        const auto t = accessor_type(f);
        std::string comment = f.doc;
        if (!f.unit.empty()) comment += (comment.empty() ? "" : " ") + std::string("[") + f.unit + "]";
        if (!comment.empty()) w_.line("/// " + comment);
        const bool constexpr_ok = f.kind == FieldKind::Scalar || f.kind == FieldKind::Enum || f.kind == FieldKind::Bits;
        (void)constexpr_ok;
        if (f.when) {
            const auto bit = "(gmp_present_ >> " + sz(cond_bit_.at(&f)) + ") & 1u";
            w_.line("[[nodiscard]] std::optional<" + t + "> " + n + "() const noexcept { if (!(" + bit +
                    ")) return std::nullopt; return " + accessor_body(f) + "; }");
        } else {
            w_.line("[[nodiscard]] " + t + " " + n + "() const noexcept { return " + accessor_body(f) + "; }");
        }
        for (const auto& cv : conversions_of(f)) {
            const auto scale = double_literal(cv.scale);
            const auto offv = double_literal(cv.offset);
            const auto acc = f.name + "_" + cv.suffix;
            if (!cv.unit.empty() && cv.suffix != "scaled") w_.line("/// " + f.name + " in " + cv.unit);
            if (f.when)
                w_.line("[[nodiscard]] std::optional<double> " + acc + "() const noexcept { auto v = " + n +
                        "(); if (!v) return std::nullopt; return static_cast<double>(*v) * " + scale + " + " + offv + "; }");
            else
                w_.line("[[nodiscard]] double " + acc + "() const noexcept { return static_cast<double>(" + n + "()) * " +
                        scale + " + " + offv + "; }");
        }
    }

    Ctx& c_;
    CodeWriter& w_;
    const Message& m_;
    std::string name_;
    bool dynamic_ = false;
    RefFn walk_ref_, acc_ref_;
    std::map<const Field*, std::size_t> cond_bit_;
};

} // namespace

void emit_binary_message(Ctx& c, const Message& m) {
    for (const auto& f : m.fields)
        if (f->kind == FieldKind::Bits) emit_bits_type(c, m, *f);
    if (m.want_struct) StructEmitter(c, m).emit();
    if (m.want_view) ViewEmitter(c, m).emit();
}

} // namespace gmpc::cpp
