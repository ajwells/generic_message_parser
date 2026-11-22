// C emission for binary messages: bits group types, eager structs and zero-copy views.
//
// Semantics follow backend/cpp/emit_binary_message.cpp exactly (same validation order and error codes), so the
// C and C++ outputs can be cross-checked. Small helpers (accessors, conversions, computed values, bits pack/unpack)
// are `static inline` in the header; read/write/decode/encode and view validation live in the .c file.

#include <map>
#include <optional>
#include <string>

#include "backend/c/c_common.hpp"
#include "schema/expr.hpp"

namespace gmpc::c {

namespace {

using namespace ir;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string dv(int depth) { return std::to_string(depth); }

/// `if ((e = call) != GMSG_OK) return e;`
std::string try_(const std::string& call) { return "if ((e = " + call + ") != GMSG_OK) return e;"; }
std::string fail(const char* err) { return std::string("return GMSG_ERR_") + err + ";"; }

std::size_t value_bytes(const Field& f) { return prim_size(f.prim); }
bool strict_enum(const Field& f) { return f.kind == FieldKind::Enum && f.enum_def && f.enum_def->strict; }

bool bits_need_check(const Field& f) {
    for (const auto& b : f.bits)
        if (b.const_value || (b.enum_def && b.enum_def->strict)) return true;
    return false;
}

struct ConvAccessor {
    std::string suffix;
    double scale, offset;
    std::string unit;
};
std::vector<ConvAccessor> conversions_of(const Field& f) {
    std::vector<ConvAccessor> out;
    if (f.kind != FieldKind::Scalar) return out;
    if (f.scale || f.offset_value) out.push_back({"scaled", f.scale.value_or(1.0), f.offset_value.value_or(0.0), f.unit});
    for (const auto& cv : f.conversions) out.push_back({cv.name, cv.scale.value_or(1.0), cv.offset_value.value_or(0.0), cv.unit});
    return out;
}

/// Load expression for a scalar/enum value of field `f` at pointer `p`.
std::string load_expr(const Ctx& c, const Message& m, const Field& f, const std::string& p) {
    const auto call = f.prim == Prim::Bool ? "gmsg_load_bool(" + p + ")"
                                           : "gmsg_load_" + prim_suffix(f.prim) + "(" + p + ", " + endian_expr(f.endian) + ")";
    if (f.prim == Prim::Char) return "(char)" + call;
    if (f.kind == FieldKind::Enum) return "(" + element_type(c, m, f) + ")" + call;
    return call;
}

// -------------------------------------------------------------------------------------------------
// bits groups
// -------------------------------------------------------------------------------------------------
std::string bitfield_type(const Ctx& c, const BitField& b) { return b.enum_def ? enum_name(c, *b.enum_def) : prim_type(b.prim); }

std::string get_bits_expr(const Ctx& c, const BitField& b, const std::string& p, const std::string& order) {
    const auto args = "(" + p + ", " + sz(b.offset) + ", " + sz(b.bits) + ", " + order + ")";
    if (b.prim == Prim::Bool && !b.enum_def) return "gmsg_get_bits" + args + " != 0";
    if (!b.enum_def && prim_is_signed(b.prim)) return "(" + bitfield_type(c, b) + ")gmsg_get_bits_signed" + args;
    return "(" + bitfield_type(c, b) + ")gmsg_get_bits" + args;
}

void emit_bits_type(Ctx& c, const Message& m, const Field& f) {
    auto& h = c.h;
    const auto name = bits_name(c, m, f);
    const auto order = bit_order_expr(f.bit_order);
    doc(h, f.doc.empty() ? "Bit fields of " + m.name + "." + f.name : f.doc);
    h.open("typedef struct " + name + " {");
    bool any = false;
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value) continue;
        any = true;
        h.line(bitfield_type(c, b) + " " + ident(b.name) + ";" + (b.doc.empty() ? "" : "  /* " + b.doc + " */"));
    }
    if (!any) h.line("uint8_t gmsg_unused_; /* every bit is constant or padding */");
    h.close("} " + name + ";");
    h.line();
    h.line("#define " + upper(name) + "_WIRE_SIZE " + sz(f.bits_bytes));
    h.line();
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value || !(b.scale || b.offset_value)) continue;
        h.line("static inline double " + name + "_" + b.name + "_scaled(const " + name + "* g) { return (double)g->" +
               ident(b.name) + " * " + double_literal(b.scale.value_or(1.0)) + " + " +
               double_literal(b.offset_value.value_or(0.0)) + "; }");
    }
    h.open("static inline void " + name + "_unpack(" + name + "* g, const uint8_t* p) {");
    if (!any) h.line("(void)p;");
    if (!any) h.line("g->gmsg_unused_ = 0;");
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value) continue;
        h.line("g->" + ident(b.name) + " = " + get_bits_expr(c, b, "p", order) + ";");
    }
    h.close();
    h.open("static inline void " + name + "_pack(const " + name + "* g, uint8_t* p) {");
    if (!any) h.line("(void)g;");
    for (const auto& b : f.bits) {
        const auto pos = "p, " + sz(b.offset) + ", " + sz(b.bits) + ", ";
        if (b.pad) {
            h.line("gmsg_set_bits(" + pos + "0, " + order + ");");
        } else if (b.const_value) {
            h.line("gmsg_set_bits(" + pos + "(uint64_t)" + int_literal(*b.const_value) + ", " + order + ");");
        } else if (b.prim == Prim::Bool && !b.enum_def) {
            h.line("gmsg_set_bits(" + pos + "g->" + ident(b.name) + " ? 1u : 0u, " + order + ");");
        } else if (!b.enum_def && prim_is_signed(b.prim)) {
            h.line("gmsg_set_bits_signed(" + pos + "(int64_t)g->" + ident(b.name) + ", " + order + ");");
        } else {
            h.line("gmsg_set_bits(" + pos + "(uint64_t)g->" + ident(b.name) + ", " + order + ");");
        }
    }
    h.close();
    h.line("/* Validate constant and strict-enum bit fields of the raw group bytes. */");
    h.open("static inline gmsg_error " + name + "_check(const uint8_t* p) {");
    h.line("(void)p;");
    for (const auto& b : f.bits) {
        if (b.const_value) {
            h.line("if (gmsg_get_bits(p, " + sz(b.offset) + ", " + sz(b.bits) + ", " + order + ") != (uint64_t)" +
                   int_literal(*b.const_value) + ") " + fail("BAD_CONST"));
        } else if (b.enum_def && b.enum_def->strict) {
            h.line("if (!" + enum_name(c, *b.enum_def) + "_is_known(" + get_bits_expr(c, b, "p", order) + ")) " +
                   fail("UNKNOWN_ENUM"));
        }
    }
    h.line("return GMSG_OK;");
    h.close();
    h.line("/* Human readable dump (gmp/c/print.h). */");
    h.line("void " + name + "_print(const " + name + "* g, gmsg_printer* p);");
    h.line();
    auto& s = c.c;
    s.open("void " + name + "_print(const " + name + "* g, gmsg_printer* p) {");
    if (!any) s.line("(void)g;");
    s.line("gmsg_print_begin(p, NULL);");
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value) continue;
        const auto v = "g->" + ident(b.name);
        s.line("gmsg_print_key(p, " + string_literal(b.name) + ");");
        s.line(b.enum_def ? print_enum(c, *b.enum_def, v) : print_prim(b.prim, v));
        if (b.scale || b.offset_value)
            s.line("gmsg_print_conversion(p, (double)" + v + " * " + double_literal(b.scale.value_or(1.0)) + " + " +
                   double_literal(b.offset_value.value_or(0.0)) + ", NULL);");
    }
    s.line("gmsg_print_end(p);");
    s.close();
    s.line();
}

// -------------------------------------------------------------------------------------------------
// struct
// -------------------------------------------------------------------------------------------------
class StructEmitter {
public:
    StructEmitter(Ctx& c, const Message& m)
        : c_(c), h_(c.h), s_(c.c), m_(m), t_(type_name(c, m)), names_(c.diag, m.loc, "message '" + m.name + "'") {
        base_ref_ = [this](const Field& f, const BitField* b) {
            if (f.const_value) return typed_literal(c_, f, *f.const_value);
            std::string s = "m->" + ident(f.name);
            if (b) s += "." + ident(b->name);
            return s;
        };
        // On encode, a derived length/count field takes its value from the container it describes.
        write_ref_ = [this](const Field& f, const BitField* b) {
            if (f.derived_from && !b) return derived_count(f);
            return base_ref_(f, b);
        };
        ref_ = base_ref_;
    }

    /// The member holding the number of bytes / elements of a variable container. When the length is a bare
    /// reference to an integer field (derived on encode, as in C++), that field is the size and there is no
    /// extra member; otherwise `<field>_len` (bytes, strings) or `<field>_count` (arrays).
    static std::string count_member(const Field& f) {
        if (is_derived_length(f)) return ident(f.len.expr->field->name);
        return ident(f.name) + (f.kind == FieldKind::Array ? "_count" : "_len");
    }

    /// C type of count_member(f).
    static std::string count_member_type(const Field& f, std::size_t capacity) {
        if (is_derived_length(f)) return prim_type(f.len.expr->field->prim);
        return count_type(capacity);
    }

    /// Number of elements / bytes of `f`'s container in the struct (0 when a conditional field is absent).
    static std::string container_size(const Field& f) {
        if (f.len.kind == LenKind::Fixed && f.kind != FieldKind::String) return sz(f.len.fixed);
        return "(size_t)m->" + count_member(f);
    }

    static std::uint64_t container_capacity(const Field& f) {
        return f.len.kind == LenKind::Fixed ? f.len.fixed : f.max.value_or(0);
    }

    static std::uint64_t prim_max_value(Prim p) {
        if (prim_is_float(p) || p == Prim::Bool || p == Prim::Char) return 0;
        const unsigned bits = static_cast<unsigned>(prim_size(p) * 8) - (prim_is_signed(p) ? 1u : 0u);
        return bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
    }

    static std::string derived_count(const Field& f) {
        const auto& d = *f.derived_from;
        const auto n = container_size(d);
        return d.when ? "(m->has_" + d.name + " ? " + n + " : 0u)" : n;
    }

    void emit() {
        emit_members();
        emit_constants();
        emit_prototypes();
        emit_scaled();
        emit_computed();
        emit_read();
        emit_write();
        emit_wrappers();
        emit_print();
    }

private:
    void emit_members() {
        doc(h_, m_.doc);
        h_.open("typedef struct " + t_ + " {");
        bool any = false;
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            if (!f.has_member()) continue;
            any = true;
            std::string comment = f.doc;
            if (!f.unit.empty()) comment += (comment.empty() ? "" : " ") + std::string("[") + f.unit + "]";
            if (f.derived_from)
                comment += (comment.empty() ? "" : "; ") + std::string("number of ") +
                           (f.derived_from->kind == FieldKind::Array ? "elements" : "bytes") + " in " + f.derived_from->name;
            const auto n = ident(f.name);
            const auto tail = comment.empty() ? std::string() : "  /* " + comment + " */";
            if (f.when) {
                h_.line("bool has_" + f.name + ";  /* " + f.name + " is present */");
                names_.add("has_" + f.name, "presence of " + f.name);
            }
            names_.add(n, "field " + f.name);
            switch (f.kind) {
                case FieldKind::Bytes:
                    if (f.len.kind == LenKind::Fixed) {
                        h_.line("uint8_t " + n + "[" + sz(f.len.fixed) + "];" + tail);
                    } else {
                        h_.line("uint8_t " + n + "[" + sz(f.max.value_or(0)) + "];" + tail);
                        if (!is_derived_length(f)) {
                            h_.line(count_type(f.max.value_or(0)) + " " + count_member(f) + ";  /* bytes used in " + f.name + " */");
                            names_.add(count_member(f), "length of " + f.name);
                        }
                    }
                    break;
                case FieldKind::String: {
                    const auto cap = f.len.kind == LenKind::Fixed ? f.len.fixed : f.max.value_or(0);
                    h_.line("char " + n + "[" + sz(cap + 1) + "];" + (tail.empty() ? "  /* NUL terminated after decode */" : tail));
                    if (!is_derived_length(f)) {
                        h_.line(count_type(cap) + " " + count_member(f) + ";  /* characters in " + f.name + " (encode uses this) */");
                        names_.add(count_member(f), "length of " + f.name);
                    }
                    break;
                }
                case FieldKind::Array: {
                    const auto et = element_type(c_, m_, *f.elem);
                    if (f.len.kind == LenKind::Fixed) {
                        h_.line(et + " " + n + "[" + sz(f.len.fixed) + "];" + tail);
                    } else {
                        h_.line(et + " " + n + "[" + sz(f.max.value_or(0)) + "];" + tail);
                        if (!is_derived_length(f)) {
                            h_.line(count_type(f.max.value_or(0)) + " " + count_member(f) + ";  /* elements used in " + f.name + " */");
                            names_.add(count_member(f), "count of " + f.name);
                        }
                    }
                    break;
                }
                default: h_.line(element_type(c_, m_, f) + " " + n + ";" + tail); break;
            }
        }
        if (!any) h_.line("uint8_t gmsg_unused_; /* no value fields */");
        h_.close("} " + t_ + ";");
        h_.line();
    }

    void emit_constants() {
        const auto M = macro_name(c_, m_);
        h_.line("#define " + M + "_MIN_WIRE_SIZE " + sz(m_.min_size));
        if (m_.max_size) h_.line("#define " + M + "_MAX_WIRE_SIZE " + sz(*m_.max_size));
        if (m_.static_size) h_.line("#define " + M + "_WIRE_SIZE " + sz(*m_.static_size));
        for (const auto& f : m_.fields)
            if (f->const_value) h_.line("#define " + M + "_" + upper(f->name) + "_VALUE " + typed_literal(c_, *f, *f->const_value));
        h_.line();
    }

    void emit_prototypes() {
        for (const char* fn : {"read", "write", "decode", "encode", "print"}) names_.add(fn, "function");
        h_.line("/* Read / write one " + m_.name + " at a cursor. */");
        h_.line("gmsg_error " + t_ + "_read(gmsg_reader* r, " + t_ + "* m);");
        h_.line("gmsg_error " + t_ + "_write(const " + t_ + "* m, gmsg_writer* w);");
        h_.line("/* Decode from a buffer (trailing bytes are ignored; *out is unspecified on error). */");
        h_.line("gmsg_error " + t_ + "_decode(const uint8_t* data, size_t size, " + t_ + "* out);");
        h_.line("/* Encode into out[0..capacity); *written receives the number of bytes (may be NULL). */");
        h_.line("gmsg_error " + t_ + "_encode(const " + t_ + "* m, uint8_t* out, size_t capacity, size_t* written);");
        h_.line("/* Human readable dump (gmp/c/print.h). */");
        h_.line("void " + t_ + "_print(const " + t_ + "* m, gmsg_printer* p);");
        h_.line();
    }

    void emit_scaled() {
        bool any = false;
        for (const auto& f : m_.fields) {
            if (!f->has_member()) continue;
            const auto n = ident(f->name);
            const auto t = prim_type(f->prim);
            for (const auto& cv : conversions_of(*f)) {
                any = true;
                const auto acc = f->name + "_" + cv.suffix;
                names_.add(acc, "conversion");
                names_.add("set_" + acc, "conversion setter");
                const auto scale = double_literal(cv.scale);
                const auto off = double_literal(cv.offset);
                if (!cv.unit.empty() && cv.suffix != "scaled") h_.line("/* " + f->name + " in " + cv.unit + " */");
                h_.line("static inline double " + t_ + "_" + acc + "(const " + t_ + "* m) { return (double)m->" + n +
                        " * " + scale + " + " + off + "; }");
                const auto conv = prim_is_float(f->prim) ? "(" + t + ")((v - " + off + ") / " + scale + ")"
                                                         : "(" + t + ")gmsg_round_i64((v - " + off + ") / " + scale + ")";
                h_.line("static inline void " + t_ + "_set_" + acc + "(" + t_ + "* m, double v) { m->" + n + " = " + conv + "; }");
            }
        }
        if (any) h_.line();
    }

    void emit_computed() {
        for (const auto& cp : m_.computed) {
            const auto t = prim_type(cp.prim);
            names_.add(cp.name, "computed value");
            if (!cp.doc.empty()) doc(h_, cp.doc);
            h_.line("/* " + cp.name + " = " + expr_to_string(*cp.value) + (cp.unit.empty() ? "" : "  [" + cp.unit + "]") + " */");
            h_.line("static inline " + t + " " + t_ + "_" + cp.name + "(const " + t_ + "* m) { return (" + t + ")(" +
                    expr(c_, *cp.value, base_ref_) + "); }");
            if (cp.setters.empty()) continue;
            names_.add("set_" + cp.name, "computed setter");
            h_.line(std::string("/* Sets the fields ") + (cp.setter_derived ? "packed into " : "listed in `set` for ") + cp.name +
                    " so that " + t_ + "_" + cp.name + "(m) == v" + (cp.setter_derived ? " (for values that fit in those fields)." : ".") + " */");
            h_.open("static inline void " + t_ + "_set_" + cp.name + "(" + t_ + "* m, " + t + " v) {");
            h_.line("const int64_t value = (int64_t)v;");
            h_.line("(void)value;");
            for (const auto& [f, e] : cp.setters)
                h_.line("m->" + ident(f->name) + " = (" + element_type(c_, m_, *f) + ")(" + expr(c_, *e, base_ref_) + ");");
            h_.close();
        }
        if (!m_.computed.empty()) h_.line();
    }

    // ---- read ---------------------------------------------------------------------------------------
    void emit_read() {
        s_.open("gmsg_error " + t_ + "_read(gmsg_reader* r, " + t_ + "* m) {");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line("(void)r;");
        s_.line("(void)m;");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            const auto dst = f.has_member() ? "m->" + ident(f.name) : std::string();
            if (f.when) {
                s_.open("if (" + expr(c_, *f.when, ref_) + ") {");
                s_.line("m->has_" + f.name + " = true;");
                read_value(f, dst, 0);
                s_.dedent();
                s_.open("} else {");
                s_.line("m->has_" + f.name + " = false;");
                s_.line("gmsg_memset(&" + dst + ", 0, sizeof " + dst + ");");
                s_.close();
            } else {
                read_value(f, dst, 0);
            }
        }
        s_.line("return e;");
        s_.close();
        s_.line();
    }

    void read_value(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: {
                const auto t = element_type(c_, m_, f);
                s_.open("{");
                const auto v = "v" + d;
                if (f.crc) {
                    const auto n = sz(value_bytes(f));
                    s_.line("const uint8_t* b" + d + " = NULL;");
                    s_.line(try_("gmsg_read_bytes(r, " + sz(value_bytes(f) + f.crc->size) + ", &b" + d + ")"));
                    s_.line("if (gmsg_load_uint(b" + d + " + " + n + ", " + sz(f.crc->size) + ", " + endian_expr(f.crc->endian) +
                            ") != " + checksum_call(*f.crc, "b" + d, n) + ") " + fail("BAD_CHECKSUM"));
                    s_.line("const " + t + " " + v + " = " + load_expr(c_, m_, f, "b" + d) + ";");
                } else {
                    // read through the raw prim type (char goes through uint8_t)
                    const auto raw = f.prim == Prim::Char ? std::string("uint8_t") : t;
                    s_.line(raw + " " + v + " = 0;");
                    if (f.prim == Prim::Bool) s_.line(try_("gmsg_read_bool(r, &" + v + ")"));
                    else s_.line(try_("gmsg_read_" + prim_suffix(f.prim) + "(r, " + endian_expr(f.endian) + ", &" + v + ")"));
                }
                if (strict_enum(f)) s_.line("if (!" + enum_name(c_, *f.enum_def) + "_is_known(" + v + ")) " + fail("UNKNOWN_ENUM"));
                if (f.const_value) s_.line("if (" + v + " != " + typed_literal(c_, f, *f.const_value) + ") " + fail("BAD_CONST"));
                else if (!dst.empty()) s_.line(dst + " = " + (f.prim == Prim::Char ? "(char)" : "") + v + ";");
                s_.close();
                break;
            }
            case FieldKind::Bits: {
                const auto t = bits_name(c_, m_, f);
                s_.open("{");
                s_.line("const uint8_t* b" + d + " = NULL;");
                s_.line(try_("gmsg_read_bytes(r, " + sz(f.bits_bytes) + ", &b" + d + ")"));
                s_.line(try_(t + "_check(b" + d + ")"));
                s_.line(t + "_unpack(&" + dst + ", b" + d + ");");
                s_.close();
                break;
            }
            case FieldKind::Pad: s_.line(try_("gmsg_read_skip(r, " + sz(f.len.fixed) + ")")); break;
            case FieldKind::Bytes:
            case FieldKind::String: read_blob(f, dst, depth); break;
            case FieldKind::Array: read_array(f, dst, depth); break;
            case FieldKind::Message: s_.line(try_(type_name(c_, *f.message) + "_read(r, &" + dst + ")")); break;
            case FieldKind::Custom: {
                const auto fn = lang_value(f.custom->lang, "decode");
                if (f.custom->wire_size) {
                    // the decoder sees exactly wire_size bytes
                    s_.open("{");
                    s_.line("const uint8_t* b" + d + " = NULL;");
                    s_.line(try_("gmsg_read_bytes(r, " + sz(*f.custom->wire_size) + ", &b" + d + ")"));
                    s_.line("gmsg_reader sub" + d + " = gmsg_reader_init(b" + d + ", " + sz(*f.custom->wire_size) + ");");
                    s_.line(try_(fn + "(&sub" + d + ", &" + dst + ")"));
                    s_.close();
                } else {
                    s_.line(try_(fn + "(r, &" + dst + ")"));
                }
                break;
            }
        }
    }

    void read_blob(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        const bool str = f.kind == FieldKind::String;
        const auto b = "b" + d, n = "n" + d;
        const auto max = sz(f.max.value_or(0));
        s_.open("{");
        s_.line("const uint8_t* " + b + " = NULL;");
        switch (f.len.kind) {
            case LenKind::Fixed:
                s_.line("const size_t " + n + " = " + sz(f.len.fixed) + ";");
                s_.line(try_("gmsg_read_bytes(r, " + n + ", &" + b + ")"));
                break;
            case LenKind::Expr:
                s_.line("const int64_t len" + d + " = " + expr(c_, *f.len.expr, ref_) + ";");
                s_.line("if (len" + d + " < 0 || len" + d + " > " + max + ") " + fail("OUT_OF_RANGE"));
                s_.line("const size_t " + n + " = (size_t)len" + d + ";");
                s_.line(try_("gmsg_read_bytes(r, " + n + ", &" + b + ")"));
                break;
            case LenKind::Remaining:
                s_.line("const size_t " + n + " = gmsg_reader_remaining(r);");
                s_.line("if (" + n + " > " + max + ") " + fail("OUT_OF_RANGE"));
                s_.line(try_("gmsg_read_bytes(r, " + n + ", &" + b + ")"));
                break;
            case LenKind::Terminator:
                s_.line("size_t " + n + " = 0;");
                s_.line(try_("gmsg_read_until(r, " + sz(f.len.terminator) + ", &" + b + ", &" + n + ")"));
                s_.line("if (" + n + " > " + max + ") " + fail("OUT_OF_RANGE"));
                break;
            case LenKind::None: break;
        }
        const auto len_member = "m->" + StructEmitter::count_member(f);
        if (str) {
            if (f.len.kind == LenKind::Fixed) {
                s_.line("const gmsg_str s" + d + " = gmsg_str_strip(gmsg_str_make(" + b + ", " + n + "), (char)" +
                        sz(static_cast<unsigned char>(f.pad_char)) + ");");
                s_.line("gmsg_memcpy(" + dst + ", s" + d + ".data, s" + d + ".size);");
                s_.line(dst + "[s" + d + ".size] = '\\0';");
                s_.line(len_member + " = (" + count_member_type(f, f.len.fixed) + ")s" + d + ".size;");
            } else {
                s_.line("gmsg_memcpy(" + dst + ", " + b + ", " + n + ");");
                s_.line(dst + "[" + n + "] = '\\0';");
                s_.line(len_member + " = (" + count_member_type(f, f.max.value_or(0)) + ")" + n + ";");
            }
        } else {
            s_.line("gmsg_memcpy(" + dst + ", " + b + ", " + n + ");");
            if (f.len.kind != LenKind::Fixed) s_.line(len_member + " = (" + count_member_type(f, f.max.value_or(0)) + ")" + n + ";");
        }
        s_.close();
    }

    void read_array(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        const auto i = "i" + d;
        const auto& e = *f.elem;
        const auto count = "m->" + StructEmitter::count_member(f);
        switch (f.len.kind) {
            case LenKind::Fixed:
                s_.open("for (size_t " + i + " = 0; " + i + " < " + sz(f.len.fixed) + "; ++" + i + ") {");
                read_value(e, dst + "[" + i + "]", depth + 1);
                s_.close();
                break;
            case LenKind::Expr:
                s_.open("{");
                s_.line("const int64_t n" + d + " = " + expr(c_, *f.len.expr, ref_) + ";");
                s_.line("if (n" + d + " < 0 || n" + d + " > " + sz(f.max.value_or(0)) + ") " + fail("OUT_OF_RANGE"));
                s_.line(count + " = (" + count_member_type(f, f.max.value_or(0)) + ")n" + d + ";");
                s_.open("for (size_t " + i + " = 0; " + i + " < (size_t)n" + d + "; ++" + i + ") {");
                read_value(e, dst + "[" + i + "]", depth + 1);
                s_.close();
                s_.close();
                break;
            case LenKind::Remaining:
                s_.line(count + " = 0;");
                s_.open("while (gmsg_reader_remaining(r) > 0) {");
                s_.line("if (" + count + " >= " + sz(f.max.value_or(0)) + ") " + fail("OUT_OF_RANGE"));
                read_value(e, dst + "[" + count + "]", depth + 1);
                s_.line("++" + count + ";");
                s_.close();
                break;
            default: break;
        }
    }

    // ---- write --------------------------------------------------------------------------------------
    void emit_write() {
        ref_ = write_ref_;
        s_.open("gmsg_error " + t_ + "_write(const " + t_ + "* m, gmsg_writer* w) {");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line("(void)w;");
        s_.line("(void)m;");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            const auto src = f.has_member() ? "m->" + ident(f.name) : std::string();
            if (f.when) {
                s_.open("if (" + expr(c_, *f.when, ref_) + ") {");
                s_.line("if (!m->has_" + f.name + ") " + fail("MISSING_FIELD"));
                write_value(f, src, 0);
                s_.close();
            } else {
                write_value(f, src, 0);
            }
        }
        s_.line("return e;");
        s_.close();
        s_.line();
        ref_ = base_ref_;
    }

    void write_value(const Field& f, const std::string& src, int depth) {
        const auto d = dv(depth);
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: {
                std::string val = src;
                if (f.const_value) {
                    val = typed_literal(c_, f, *f.const_value);
                } else if (f.derived_from) {
                    const auto count = derived_count(f);
                    // Only needed when the container can hold more than the length field can express.
                    if (container_capacity(*f.derived_from) > prim_max_value(f.prim))
                        s_.line("if ((uint64_t)" + count + " > (uint64_t)" + prim_max(f.prim) + ") " + fail("OUT_OF_RANGE"));
                    val = "(" + element_type(c_, m_, f) + ")" + count;
                } else if (strict_enum(f)) {
                    s_.line("if (!" + enum_name(c_, *f.enum_def) + "_is_known(" + val + ")) " + fail("UNKNOWN_ENUM"));
                }
                if (f.prim == Prim::Bool) s_.line(try_("gmsg_write_bool(w, " + val + ")"));
                else if (f.prim == Prim::Char) s_.line(try_("gmsg_write_u8(w, " + endian_expr(f.endian) + ", (uint8_t)" + val + ")"));
                else s_.line(try_("gmsg_write_" + prim_suffix(f.prim) + "(w, " + endian_expr(f.endian) + ", " + val + ")"));
                if (f.crc) {
                    const auto n = sz(value_bytes(f));
                    s_.line(try_("gmsg_write_uint(w, " + sz(f.crc->size) + ", " + endian_expr(f.crc->endian) + ", " +
                                 checksum_call(*f.crc, "w->data + w->pos - " + n, n) + ")"));
                }
                break;
            }
            case FieldKind::Bits:
                for (const auto& b : f.bits)
                    if (!b.pad && !b.const_value && b.enum_def && b.enum_def->strict)
                        s_.line("if (!" + enum_name(c_, *b.enum_def) + "_is_known(" + src + "." + ident(b.name) + ")) " +
                                fail("UNKNOWN_ENUM"));
                s_.open("{");
                s_.line("size_t at" + d + " = 0;");
                s_.line(try_("gmsg_write_reserve(w, " + sz(f.bits_bytes) + ", &at" + d + ")"));
                s_.line(bits_name(c_, m_, f) + "_pack(&" + src + ", w->data + at" + d + ");");
                s_.close();
                break;
            case FieldKind::Pad: s_.line(try_("gmsg_write_fill(w, 0, " + sz(f.len.fixed) + ")")); break;
            case FieldKind::Bytes:
            case FieldKind::String: write_blob(f, src); break;
            case FieldKind::Array: write_array(f, src, depth); break;
            case FieldKind::Message: s_.line(try_(type_name(c_, *f.message) + "_write(&" + src + ", w)")); break;
            case FieldKind::Custom: {
                const auto fn = lang_value(f.custom->lang, "encode");
                s_.open("{");
                s_.line("const size_t p" + d + " = w->pos;");
                s_.line("(void)p" + d + ";");
                s_.line(try_(fn + "(w, &" + src + ")"));
                if (f.custom->wire_size)
                    s_.line("if (w->pos - p" + d + " != " + sz(*f.custom->wire_size) + ") " + fail("INVALID_VALUE"));
                s_.close();
                break;
            }
        }
    }

    void length_check(const Field& f, const std::string& size_expr) {
        if (f.len.kind == LenKind::Expr && !is_derived_length(f))
            s_.line("if ((int64_t)" + size_expr + " != " + expr(c_, *f.len.expr, ref_) + ") " + fail("BAD_LENGTH"));
    }

    void write_blob(const Field& f, const std::string& src) {
        const bool str = f.kind == FieldKind::String;
        const auto data = str ? "(const uint8_t*)" + src : src;
        if (f.len.kind == LenKind::Fixed && !str) {
            s_.line(try_("gmsg_write_bytes(w, " + src + ", " + sz(f.len.fixed) + ")"));
            return;
        }
        const auto n = container_size(f);
        const auto cap = f.len.kind == LenKind::Fixed ? f.len.fixed : f.max.value_or(0);
        s_.line("if (" + n + " > " + sz(cap) + ") " + fail("OUT_OF_RANGE"));
        length_check(f, n);
        s_.line(try_("gmsg_write_bytes(w, " + data + ", " + n + ")"));
        if (f.len.kind == LenKind::Fixed)
            s_.line(try_("gmsg_write_fill(w, (uint8_t)" + sz(static_cast<unsigned char>(f.pad_char)) + ", " + sz(f.len.fixed) +
                         " - " + n + ")"));
        if (f.len.kind == LenKind::Terminator)
            s_.line(try_("gmsg_write_u8(w, GMSG_ENDIAN_BIG, " + sz(f.len.terminator) + ")"));
    }

    void write_array(const Field& f, const std::string& src, int depth) {
        const auto d = dv(depth);
        const auto n = container_size(f);
        if (f.len.kind != LenKind::Fixed) s_.line("if (" + n + " > " + sz(f.max.value_or(0)) + ") " + fail("OUT_OF_RANGE"));
        length_check(f, n);
        s_.open("for (size_t x" + d + " = 0; x" + d + " < " + n + "; ++x" + d + ") {");
        write_value(*f.elem, src + "[x" + d + "]", depth + 1);
        s_.close();
    }

    void emit_wrappers() {
        s_.open("gmsg_error " + t_ + "_decode(const uint8_t* data, size_t size, " + t_ + "* out) {");
        s_.line("gmsg_reader r = gmsg_reader_init(data, size);");
        s_.line("gmsg_memset(out, 0, sizeof *out);");
        s_.line("return " + t_ + "_read(&r, out);");
        s_.close();
        s_.line();
        s_.open("gmsg_error " + t_ + "_encode(const " + t_ + "* m, uint8_t* out, size_t capacity, size_t* written) {");
        s_.line("gmsg_writer w = gmsg_writer_init(out, capacity);");
        s_.line("const gmsg_error e = " + t_ + "_write(m, &w);");
        s_.line("if (written) *written = e == GMSG_OK ? w.pos : 0;");
        s_.line("return e;");
        s_.close();
        s_.line();
    }

    // ---- print ----------------------------------------------------------------------------------------
    /// Statements printing one value of element field `e` stored at lvalue `v`.
    void print_value(const Field& e, const std::string& v) {
        switch (e.kind) {
            case FieldKind::Scalar: s_.line(print_prim(e.prim, v)); break;
            case FieldKind::Enum: s_.line(print_enum(c_, *e.enum_def, v)); break;
            case FieldKind::Bits: s_.line(bits_name(c_, m_, e) + "_print(&" + v + ", p);"); break;
            case FieldKind::Message: s_.line(type_name(c_, *e.message) + "_print(&" + v + ", p);"); break;
            case FieldKind::Custom: print_custom(s_, *e.custom, "&" + v); break;
            default: break;
        }
    }

    void print_field(const Field& f) {
        const auto n = "m->" + ident(f.name);
        switch (f.kind) {
            case FieldKind::Bytes: s_.line("gmsg_print_hex(p, " + n + ", " + container_size(f) + ");"); break;
            case FieldKind::String: s_.line("gmsg_print_str(p, " + n + ", " + container_size(f) + ");"); break;
            case FieldKind::Array: {
                const bool blocks = f.elem->kind == FieldKind::Message;
                s_.open("{");
                s_.line("const gmsg_print_list l = gmsg_print_list_begin(p, " + container_size(f) + ", " +
                        (blocks ? "true" : "false") + ");");
                s_.open("for (size_t i = 0; i < l.shown; ++i) {");
                s_.line("gmsg_print_list_item(p, &l, i);");
                print_value(*f.elem, n + "[i]");
                s_.close();
                s_.line("gmsg_print_list_end(p, &l);");
                s_.close();
                break;
            }
            default:
                print_value(f, n);
                print_extras(s_, f, n);
                break;
        }
    }

    void emit_print() {
        s_.open("void " + t_ + "_print(const " + t_ + "* m, gmsg_printer* p) {");
        s_.line("(void)m;");
        s_.line("gmsg_print_begin(p, " + string_literal(m_.name) + ");");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            if (!f.has_member()) continue;
            if (f.when) s_.open("if (m->has_" + f.name + ") {");
            s_.line("gmsg_print_key(p, " + string_literal(f.name) + ");");
            print_field(f);
            if (f.when) {
                s_.dedent();
                s_.open("} else {");
                s_.line("gmsg_print_absent(p, " + string_literal(f.name) + ");");
                s_.close();
            }
        }
        for (const auto& cp : m_.computed) {
            s_.line("gmsg_print_key(p, " + string_literal(cp.name) + ");");
            s_.line(print_prim(cp.prim, t_ + "_" + cp.name + "(m)"));
            if (!cp.unit.empty()) s_.line("gmsg_print_unit(p, " + string_literal(cp.unit) + ");");
            s_.line("gmsg_print_note(p, \"computed\");");
        }
        s_.line("gmsg_print_end(p);");
        s_.close();
        s_.line();
    }

    Ctx& c_;
    CodeWriter& h_;
    CodeWriter& s_;
    const Message& m_;
    std::string t_;
    NameSet names_;
    RefFn base_ref_, ref_, write_ref_;
};

// -------------------------------------------------------------------------------------------------
// view
// -------------------------------------------------------------------------------------------------
class ViewEmitter {
public:
    ViewEmitter(Ctx& c, const Message& m)
        : c_(c), h_(c.h), s_(c.c), m_(m), t_(type_name(c, m)), v_(view_name(c, m)),
          names_(c.diag, m.loc, "view of message '" + m.name + "'") {
        dynamic_ = !m.fixed_layout;
        walk_ref_ = [this](const Field& f, const BitField* b) {
            if (f.const_value) return typed_literal(c_, f, *f.const_value);
            std::string s = v_ + "_" + f.name + "(&v)";
            if (b) s += "." + ident(b->name);
            return s;
        };
        acc_ref_ = [this](const Field& f, const BitField* b) {
            if (f.const_value) return typed_literal(c_, f, *f.const_value);
            std::string s = v_ + "_" + f.name + "(v)";
            if (b) s += "." + ident(b->name);
            return s;
        };
        std::size_t k = 0;
        for (const auto& f : m.fields)
            if (f->when) cond_bit_[f.get()] = k++;
    }

    void emit() {
        doc(h_, "Zero-copy view over an encoded " + m_.name + "; points into the caller's buffer.");
        h_.open("typedef struct " + v_ + " {");
        h_.line("const uint8_t* data;");
        h_.line("size_t size;  /* bytes of this message */");
        if (dynamic_) h_.line("uint32_t off[" + sz(m_.fields.size() + 1) + "];  /* field offsets */");
        if (m_.conditional_count) h_.line("uint64_t present;  /* one bit per conditional field */");
        h_.close("} " + v_ + ";");
        h_.line();
        for (const char* fn : {"from", "unchecked", "to_struct", "print"}) names_.add(fn, "function");
        h_.line("/* Validate data[0..size) and build a view. Trailing bytes beyond the message are ignored. */");
        h_.line("gmsg_error " + v_ + "_from(const uint8_t* data, size_t size, " + v_ + "* out);");
        h_.line("/* Build a view over data already validated by " + v_ + "_from() (no checks). */");
        h_.line(v_ + " " + v_ + "_unchecked(const uint8_t* data, size_t size);");
        if (m_.want_struct) {
            h_.line("/* Decode into the eager struct. */");
            h_.line("gmsg_error " + v_ + "_to_struct(const " + v_ + "* v, " + t_ + "* out);");
        }
        h_.line("/* Human readable dump (gmp/c/print.h); same text as the struct. */");
        h_.line("void " + v_ + "_print(const " + v_ + "* v, gmsg_printer* p);");
        h_.line();
        for (const auto& f : m_.fields) emit_accessor(*f);
        emit_computed_accessors();
        h_.line();
        emit_from();
        emit_unchecked();
        if (m_.want_struct) {
            s_.open("gmsg_error " + v_ + "_to_struct(const " + v_ + "* v, " + t_ + "* out) {");
            s_.line("return " + t_ + "_decode(v->data, v->size, out);");
            s_.close();
            s_.line();
        }
        emit_print();
    }

private:
    /// Statements printing one value of element field `e` given by the (view) expression `v`.
    void print_value(const Field& e, const std::string& v) {
        switch (e.kind) {
            case FieldKind::Scalar: s_.line(print_prim(e.prim, v)); break;
            case FieldKind::Enum: s_.line(print_enum(c_, *e.enum_def, v)); break;
            case FieldKind::Bits:
            case FieldKind::Message: {
                const auto t = e.kind == FieldKind::Bits ? bits_name(c_, m_, e) : view_name(c_, *e.message);
                s_.open("{");
                s_.line("const " + t + " x = " + v + ";");
                s_.line(t + "_print(&x, p);");
                s_.close();
                break;
            }
            default: break;
        }
    }

    void print_field(const Field& f) {
        const auto base = v_ + "_" + f.name;
        switch (f.kind) {
            case FieldKind::Bytes:
                s_.open("{");
                s_.line("const gmsg_bytes b = " + base + "(v);");
                s_.line("gmsg_print_hex(p, b.data, b.size);");
                s_.close();
                break;
            case FieldKind::String:
                s_.open("{");
                s_.line("const gmsg_str s = " + base + "(v);");
                s_.line("gmsg_print_str(p, s.data, s.size);");
                s_.close();
                break;
            case FieldKind::Array: {
                const bool blocks = f.elem->kind == FieldKind::Message;
                s_.open("{");
                s_.line("const gmsg_print_list l = gmsg_print_list_begin(p, " + base + "_count(v), " +
                        (blocks ? "true" : "false") + ");");
                s_.open("for (size_t i = 0; i < l.shown; ++i) {");
                s_.line("gmsg_print_list_item(p, &l, i);");
                print_value(*f.elem, base + "_at(v, i)");
                s_.close();
                s_.line("gmsg_print_list_end(p, &l);");
                s_.close();
                break;
            }
            case FieldKind::Custom:
                s_.open("{");
                s_.line(element_type(c_, m_, f) + " x;");
                s_.line("const gmsg_error e = " + base + "(v, &x);");
                s_.open("if (e == GMSG_OK) {");
                print_custom(s_, *f.custom, "&x");
                s_.dedent();
                s_.open("} else {");
                s_.line("gmsg_print_error(p, e);");
                s_.close();
                s_.close();
                break;
            default:
                if (has_print_extras(f)) {
                    s_.open("{");
                    s_.line("const " + accessor_type(f) + " x = " + base + "(v);");
                    print_value(f, "x");
                    print_extras(s_, f, "x");
                    s_.close();
                } else {
                    print_value(f, base + "(v)");
                }
                break;
        }
    }

    void emit_print() {
        s_.open("void " + v_ + "_print(const " + v_ + "* v, gmsg_printer* p) {");
        s_.line("(void)v;");
        s_.line("gmsg_print_begin(p, " + string_literal(m_.name) + ");");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            if (!f.has_member()) continue;
            if (f.when) s_.open("if (" + v_ + "_has_" + f.name + "(v)) {");
            s_.line("gmsg_print_key(p, " + string_literal(f.name) + ");");
            print_field(f);
            if (f.when) {
                s_.dedent();
                s_.open("} else {");
                s_.line("gmsg_print_absent(p, " + string_literal(f.name) + ");");
                s_.close();
            }
        }
        for (const auto& cp : m_.computed) {
            s_.line("gmsg_print_key(p, " + string_literal(cp.name) + ");");
            s_.line(print_prim(cp.prim, v_ + "_" + cp.name + "(v)"));
            if (!cp.unit.empty()) s_.line("gmsg_print_unit(p, " + string_literal(cp.unit) + ");");
            s_.line("gmsg_print_note(p, \"computed\");");
        }
        s_.line("gmsg_print_end(p);");
        s_.close();
        s_.line();
    }

    std::string off(const Field& f) const {
        if (f.static_offset) return sz(*f.static_offset);
        return "v->off[" + sz(f.index) + "]";
    }
    std::string size(const Field& f) const {
        if (f.static_size) return sz(*f.static_size);
        return "(size_t)(v->off[" + sz(f.index + 1) + "] - v->off[" + sz(f.index) + "])";
    }

    void nested_check(const Message& nested, const std::string& p, const std::string& n) {
        s_.open("{");
        s_.line(view_name(c_, nested) + " c;");
        s_.line(try_(view_name(c_, nested) + "_from(" + p + ", " + n + ", &c)"));
        s_.close();
    }

    void enum_array_check(const Field& e, const std::string& p, const std::string& count) {
        const auto es = sz(prim_size(e.prim));
        s_.open("for (size_t k = 0; k < " + count + "; ++k) {");
        s_.line("if (!" + enum_name(c_, *e.enum_def) + "_is_known(" + load_expr(c_, m_, e, p + " + k * " + es) + ")) " +
                fail("UNKNOWN_ENUM"));
        s_.close();
    }

    /// Validation for a field with static size at byte offset expression `o` of `data`.
    void validate_static(const Field& f, const std::string& o) {
        const auto p = "data + " + o;
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum:
                if (f.crc) {
                    const auto n = sz(value_bytes(f));
                    s_.line("if (gmsg_load_uint(" + p + " + " + n + ", " + sz(f.crc->size) + ", " + endian_expr(f.crc->endian) +
                            ") != " + checksum_call(*f.crc, p, n) + ") " + fail("BAD_CHECKSUM"));
                }
                if (f.const_value)
                    s_.line("if (" + load_expr(c_, m_, f, p) + " != " + typed_literal(c_, f, *f.const_value) + ") " + fail("BAD_CONST"));
                else if (strict_enum(f))
                    s_.line("if (!" + enum_name(c_, *f.enum_def) + "_is_known(" + load_expr(c_, m_, f, p) + ")) " + fail("UNKNOWN_ENUM"));
                break;
            case FieldKind::Bits:
                if (bits_need_check(f)) s_.line(try_(bits_name(c_, m_, f) + "_check(" + p + ")"));
                break;
            case FieldKind::Message:
                if (f.message->needs_validation) nested_check(*f.message, p, sz(*f.static_size));
                break;
            case FieldKind::Array:
                if (f.elem->kind == FieldKind::Message && f.elem->message->needs_validation) {
                    const auto es = sz(*f.elem->static_size);
                    s_.open("for (size_t k = 0; k < " + sz(f.len.fixed) + "; ++k) {");
                    nested_check(*f.elem->message, p + " + k * " + es, es);
                    s_.close();
                } else if (f.elem->const_value) {
                    const auto es = sz(*f.elem->static_size);
                    s_.open("for (size_t k = 0; k < " + sz(f.len.fixed) + "; ++k) {");
                    s_.line("if (" + load_expr(c_, m_, *f.elem, p + " + k * " + es) + " != " +
                            typed_literal(c_, *f.elem, *f.elem->const_value) + ") " + fail("BAD_CONST"));
                    s_.close();
                } else if (strict_enum(*f.elem)) {
                    enum_array_check(*f.elem, p, sz(f.len.fixed));
                }
                break;
            default: break;
        }
    }

    void emit_from() {
        s_.open("gmsg_error " + v_ + "_from(const uint8_t* data, size_t size, " + v_ + "* out) {");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line("(void)e;");
        if (!dynamic_) {
            s_.line("if (size < " + sz(*m_.static_size) + ") " + fail("TRUNCATED"));
            for (const auto& f : m_.fields) validate_static(*f, sz(*f->static_offset));
            s_.line("out->data = data;");
            s_.line("out->size = " + sz(*m_.static_size) + ";");
            s_.line("return GMSG_OK;");
            s_.close();
            s_.line();
            return;
        }
        s_.line(v_ + " v;");
        s_.line("gmsg_memset(&v, 0, sizeof v);");
        s_.line("v.data = data;");
        s_.line("v.size = size;");
        s_.line("size_t pos = 0;");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            s_.line("v.off[" + sz(f.index) + "] = (uint32_t)pos;");
            if (f.when) {
                s_.open("if (" + expr(c_, *f.when, walk_ref_) + ") {");
                s_.line("v.present |= (uint64_t)1 << " + sz(cond_bit_.at(&f)) + ";");
            }
            walk_field(f);
            if (f.when) s_.close();
        }
        s_.line("v.off[" + sz(m_.fields.size()) + "] = (uint32_t)pos;");
        s_.line("v.size = pos;");
        s_.line("*out = v;");
        s_.line("return GMSG_OK;");
        s_.close();
        s_.line();
    }

    void walk_field(const Field& f) {
        std::optional<std::size_t> fixed = f.static_size;
        if (!fixed && f.when) {
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
            s_.line("if (size - pos < " + sz(*fixed) + ") " + fail("TRUNCATED"));
            if (f.kind == FieldKind::Message && f.message->needs_validation) {
                nested_check(*f.message, "data + pos", sz(*fixed));
            } else if (f.kind == FieldKind::Array && f.elem->kind == FieldKind::Message && f.elem->message->needs_validation) {
                const auto es = sz(*f.elem->static_size);
                s_.open("for (size_t k = 0; k < " + sz(f.len.fixed) + "; ++k) {");
                nested_check(*f.elem->message, "data + pos + k * " + es, es);
                s_.close();
            } else {
                validate_static(f, "pos");
            }
            s_.line("pos += " + sz(*fixed) + ";");
            return;
        }
        switch (f.kind) {
            case FieldKind::Bytes:
            case FieldKind::String:
                switch (f.len.kind) {
                    case LenKind::Expr:
                        s_.open("{");
                        s_.line("const int64_t n = " + expr(c_, *f.len.expr, walk_ref_) + ";");
                        s_.line("if (n < 0 || (uint64_t)n > (uint64_t)(size - pos)) " + fail("TRUNCATED"));
                        s_.line("pos += (size_t)n;");
                        s_.close();
                        break;
                    case LenKind::Remaining: s_.line("pos = size;"); break;
                    case LenKind::Terminator:
                        s_.open("{");
                        s_.line("size_t k = pos;");
                        s_.line("while (k < size && data[k] != " + sz(f.len.terminator) + ") ++k;");
                        s_.line("if (k == size) " + fail("TRUNCATED"));
                        s_.line("pos = k + 1;");
                        s_.close();
                        break;
                    default: break;
                }
                break;
            case FieldKind::Array: walk_array(f); break;
            case FieldKind::Message:
                s_.open("{");
                s_.line(view_name(c_, *f.message) + " c;");
                s_.line(try_(view_name(c_, *f.message) + "_from(data + pos, size - pos, &c)"));
                s_.line("pos += c.size;");
                s_.close();
                break;
            case FieldKind::Custom:
                // run the decoder to find the field's size
                s_.open("{");
                s_.line(element_type(c_, m_, f) + " c;");
                s_.line("gmsg_reader cr = gmsg_reader_init(data + pos, size - pos);");
                s_.line(try_(lang_value(f.custom->lang, "decode") + "(&cr, &c)"));
                s_.line("pos += cr.pos;");
                s_.close();
                break;
            default: break;
        }
    }

    void walk_array(const Field& f) {
        const auto& e = *f.elem;
        const bool elem_msg = e.kind == FieldKind::Message;
        const auto es = e.static_size;
        const bool validate = elem_msg && e.message->needs_validation;
        s_.open("{");
        if (f.len.kind == LenKind::Remaining) {
            if (es && !validate) {
                s_.line("if ((size - pos) % " + sz(*es) + " != 0) " + fail("BAD_LENGTH"));
                if (strict_enum(e)) enum_array_check(e, "data + pos", "(size - pos) / " + sz(*es));
                s_.line("pos = size;");
            } else {
                s_.open("while (pos < size) {");
                s_.line(view_name(c_, *e.message) + " c;");
                s_.line(try_(view_name(c_, *e.message) + "_from(data + pos, size - pos, &c)"));
                s_.line("pos += c.size;");
                s_.close();
            }
        } else {
            if (f.len.kind == LenKind::Fixed) s_.line("const int64_t n = " + sz(f.len.fixed) + ";");
            else s_.line("const int64_t n = " + expr(c_, *f.len.expr, walk_ref_) + ";");
            s_.line("if (n < 0) " + fail("BAD_LENGTH"));
            if (es && !validate) {
                s_.line("if ((uint64_t)n * " + sz(*es) + " > (uint64_t)(size - pos)) " + fail("TRUNCATED"));
                if (strict_enum(e)) enum_array_check(e, "data + pos", "(size_t)n");
                s_.line("pos += (size_t)n * " + sz(*es) + ";");
            } else {
                s_.open("for (int64_t k = 0; k < n; ++k) {");
                s_.line(view_name(c_, *e.message) + " c;");
                s_.line(try_(view_name(c_, *e.message) + "_from(data + pos, size - pos, &c)"));
                s_.line("pos += c.size;");
                s_.close();
            }
        }
        s_.close();
    }

    void emit_unchecked() {
        s_.open(v_ + " " + v_ + "_unchecked(const uint8_t* data, size_t size) {");
        s_.line(v_ + " v;");
        s_.line("gmsg_memset(&v, 0, sizeof v);");
        if (!dynamic_) {
            s_.line("(void)size;");
            s_.line("v.data = data;");
            s_.line("v.size = " + sz(*m_.static_size) + ";");
        } else {
            s_.line("(void)" + v_ + "_from(data, size, &v);");
        }
        s_.line("return v;");
        s_.close();
        s_.line();
    }

    /// Return type and value of the accessor of field `f` (the value at the field's position).
    std::string accessor_type(const Field& f) const {
        switch (f.kind) {
            case FieldKind::Scalar: case FieldKind::Enum: return element_type(c_, m_, f);
            case FieldKind::Bits: return bits_name(c_, m_, f);
            case FieldKind::Bytes: return "gmsg_bytes";
            case FieldKind::String: return "gmsg_str";
            case FieldKind::Message: return view_name(c_, *f.message);
            default: return "void";
        }
    }

    /// Statements (inside an accessor whose view parameter is `v`) returning the value of `f`.
    void accessor_body(const Field& f) {
        const auto o = off(f);
        const auto z = size(f);
        const auto p = "v->data + " + o;
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: h_.line("return " + load_expr(c_, m_, f, p) + ";"); break;
            case FieldKind::Bits: {
                const auto t = bits_name(c_, m_, f);
                h_.line(t + " g;");
                h_.line(t + "_unpack(&g, " + p + ");");
                h_.line("return g;");
                break;
            }
            case FieldKind::Bytes: h_.line("return gmsg_bytes_make(" + p + ", " + z + ");"); break;
            case FieldKind::String:
                if (f.len.kind == LenKind::Fixed)
                    h_.line("return gmsg_str_strip(gmsg_str_make(" + p + ", " + z + "), (char)" +
                            sz(static_cast<unsigned char>(f.pad_char)) + ");");
                else if (f.len.kind == LenKind::Terminator)
                    h_.line("return gmsg_str_make(" + p + ", " + z + " - 1);");
                else
                    h_.line("return gmsg_str_make(" + p + ", " + z + ");");
                break;
            case FieldKind::Message: h_.line("return " + view_name(c_, *f.message) + "_unchecked(" + p + ", " + z + ");"); break;
            default: break;
        }
    }

    /// Statements returning a zero value of the accessor type (for absent conditional fields).
    void zero_return(const Field& f) {
        const auto t = accessor_type(f);
        if (f.kind == FieldKind::Scalar || f.kind == FieldKind::Enum) {
            h_.line("return (" + t + ")0;");
            return;
        }
        h_.line(t + " z;");
        h_.line("gmsg_memset(&z, 0, sizeof z);");
        h_.line("return z;");
    }

    void emit_accessor(const Field& f) {
        if (!f.has_member()) return;
        const auto base = v_ + "_" + f.name;
        std::string comment = f.doc;
        if (!f.unit.empty()) comment += (comment.empty() ? "" : " ") + std::string("[") + f.unit + "]";
        if (!comment.empty()) h_.line("/* " + comment + " */");
        std::string guard;
        if (f.when) {
            names_.add("has_" + f.name, "presence of " + f.name);
            guard = v_ + "_has_" + f.name + "(v)";
            h_.line("static inline bool " + v_ + "_has_" + f.name + "(const " + v_ + "* v) { return ((v->present >> " +
                    sz(cond_bit_.at(&f)) + ") & 1u) != 0; }");
        }
        if (f.kind == FieldKind::Array) {
            emit_array_accessors(f, guard);
        } else if (f.kind == FieldKind::Custom) {
            // a custom decoder can fail: error code + out parameter (C++: gmp::Result<T>)
            names_.add(f.name, "field " + f.name);
            h_.line("/* Decode " + f.name + " (custom type " + f.custom->name + "); GMSG_ERR_MISSING_FIELD when absent. */");
            h_.open("static inline gmsg_error " + base + "(const " + v_ + "* v, " + element_type(c_, m_, f) + "* out) {");
            if (f.when) h_.line("if (!" + guard + ") return GMSG_ERR_MISSING_FIELD;");
            h_.line("gmsg_reader r = gmsg_reader_init(v->data + " + off(f) + ", " + size(f) + ");");
            h_.line("return " + lang_value(f.custom->lang, "decode") + "(&r, out);");
            h_.close();
        } else {
            names_.add(f.name, "field " + f.name);
            h_.open("static inline " + accessor_type(f) + " " + base + "(const " + v_ + "* v) {");
            if (f.when) {
                h_.open("if (!" + guard + ") {");
                zero_return(f);
                h_.close();
            }
            accessor_body(f);
            h_.close();
        }
        for (const auto& cv : conversions_of(f)) {
            const auto acc = f.name + "_" + cv.suffix;
            names_.add(acc, "conversion");
            if (!cv.unit.empty() && cv.suffix != "scaled") h_.line("/* " + f.name + " in " + cv.unit + " */");
            h_.line("static inline double " + v_ + "_" + acc + "(const " + v_ + "* v) { return (double)" + base + "(v) * " +
                    double_literal(cv.scale) + " + " + double_literal(cv.offset) + "; }");
        }
    }

    void emit_array_accessors(const Field& f, const std::string& guard) {
        const auto base = v_ + "_" + f.name;
        const auto& e = *f.elem;
        const auto o = off(f);
        const auto z = size(f);
        names_.add(f.name + "_count", "count of " + f.name);
        names_.add(f.name + "_at", "element of " + f.name);
        const std::string absent = guard.empty() ? "" : "if (!" + guard + ") return 0; ";
        if (e.kind == FieldKind::Message && !e.static_size) {
            // variable-size elements: walk from the start
            const auto ev = view_name(c_, *e.message);
            h_.open("static inline size_t " + base + "_count(const " + v_ + "* v) {");
            if (!guard.empty()) h_.line("if (!" + guard + ") return 0;");
            h_.line("const uint8_t* p = v->data + " + o + ";");
            h_.line("const size_t total = " + z + ";");
            h_.line("size_t n = 0, pos = 0;");
            h_.open("while (pos < total) {");
            h_.line(ev + " c;");
            h_.line("if (" + ev + "_from(p + pos, total - pos, &c) != GMSG_OK) break;");
            h_.line("pos += c.size;");
            h_.line("++n;");
            h_.close();
            h_.line("return n;");
            h_.close();
            h_.line("/* Element i (walks the previous elements: O(i)). */");
            h_.open("static inline " + ev + " " + base + "_at(const " + v_ + "* v, size_t i) {");
            h_.line("const uint8_t* p = v->data + " + o + ";");
            h_.line("const size_t total = " + z + ";");
            h_.line("size_t pos = 0;");
            h_.line(ev + " c;");
            h_.line("gmsg_memset(&c, 0, sizeof c);");
            h_.open("for (size_t k = 0; k <= i && pos < total; ++k) {");
            h_.line("if (" + ev + "_from(p + pos, total - pos, &c) != GMSG_OK) break;");
            h_.line("if (k < i) pos += c.size;");
            h_.close();
            h_.line("return c;");
            h_.close();
            return;
        }
        const auto es = sz(e.kind == FieldKind::Message ? *e.static_size : prim_size(e.prim));
        h_.line("static inline size_t " + base + "_count(const " + v_ + "* v) { " + (f.static_size && guard.empty() ? "(void)v; " : "") +
                absent + "return " + z + " / " + es + "; }");
        if (e.kind == FieldKind::Message) {
            const auto ev = view_name(c_, *e.message);
            h_.line("static inline " + ev + " " + base + "_at(const " + v_ + "* v, size_t i) { return " + ev +
                    "_unchecked(v->data + " + o + " + i * " + es + ", " + es + "); }");
        } else {
            h_.line("static inline " + element_type(c_, m_, e) + " " + base + "_at(const " + v_ + "* v, size_t i) { return " +
                    load_expr(c_, m_, e, "v->data + " + o + " + i * " + es) + "; }");
        }
    }

    void emit_computed_accessors() {
        for (const auto& cp : m_.computed) {
            const auto t = prim_type(cp.prim);
            names_.add(cp.name, "computed value");
            h_.line("/* " + cp.name + " = " + expr_to_string(*cp.value) + (cp.unit.empty() ? "" : "  [" + cp.unit + "]") + " */");
            h_.line("static inline " + t + " " + v_ + "_" + cp.name + "(const " + v_ + "* v) { return (" + t + ")(" +
                    expr(c_, *cp.value, acc_ref_) + "); }");
        }
    }

    Ctx& c_;
    CodeWriter& h_;
    CodeWriter& s_;
    const Message& m_;
    std::string t_, v_;
    NameSet names_;
    bool dynamic_ = false;
    RefFn walk_ref_, acc_ref_;
    std::map<const Field*, std::size_t> cond_bit_;
};

} // namespace

void emit_binary_message(Ctx& c, const Message& m) {
    c.h.line("/* ---- " + m.name + " " + std::string(std::max<int>(0, 96 - static_cast<int>(m.name.size())), '-') + " */");
    c.h.line();
    c.c.line("/* ---- " + m.name + " " + std::string(std::max<int>(0, 96 - static_cast<int>(m.name.size())), '-') + " */");
    c.c.line();
    for (const auto& f : m.fields)
        if (f->kind == FieldKind::Bits) emit_bits_type(c, m, *f);
    if (m.want_struct) StructEmitter(c, m).emit();
    if (m.want_view) ViewEmitter(c, m).emit();
}

} // namespace gmpc::c
