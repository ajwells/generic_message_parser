// JavaScript emission for binary messages: bits group classes, struct classes and lazy view classes.
//
// Semantics follow backend/cpp/emit_binary_message.cpp check for check (same order, same error codes). JavaScript
// containers have no capacity, so encode also checks what the C++ types guarantee: variable bytes / strings / arrays
// hold at most `max` elements and fixed ones exactly their size (OutOfRange otherwise), and values fit their type.

#include <map>
#include <string>

#include "backend/js/js_common.hpp"

namespace gmpc::js {

namespace {

using namespace ir;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string dv(int depth) { return std::to_string(depth); }
std::string fail(const char* err) { return std::string("gmsg.fail(Err.") + err + ");"; }
std::string q(const std::string& s) { return "'" + s + "'"; }

bool strict_enum(const Field& f) { return f.kind == FieldKind::Enum && f.enum_def && f.enum_def->strict; }

bool bits_need_check(const Field& f) {
    for (const auto& b : f.bits)
        if (b.const_value || (b.enum_def && b.enum_def->strict)) return true;
    return false;
}

std::string bit_prim(const BitField& b) { return prim_id(b.enum_def ? b.enum_def->underlying : b.prim); }

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

/// The field value as a Number for conversions (static_cast<double>).
std::string to_double(const Field& f, const std::string& v) {
    switch (f.prim) {
        case Prim::F32: return "Math.fround(" + v + ")";
        case Prim::F64: return v;
        case Prim::Bool: return "(" + v + " ? 1 : 0)";
        case Prim::Char: return "Number(gmsg.toInt(" + v + "))";
        default: return "Number(" + v + ")";
    }
}

/// Statement printing a computed value of type p read through `v`.
std::string print_prim(Prim p, const std::string& v) {
    switch (p) {
        case Prim::F32: return "p.float(Math.fround(" + v + "), true);";
        case Prim::F64: return "p.float(" + v + ");";
        case Prim::Bool: return "p.bool(" + v + ");";
        case Prim::Char: return "p.char(" + v + ");";
        default: return "p.int(" + v + ");";
    }
}

/// Default value of one field element in a new struct.
std::string default_value(const Message& m, const Field& f) {
    switch (f.kind) {
        case FieldKind::Scalar:
        case FieldKind::Enum: return typed_literal(f, 0);
        case FieldKind::Bits: return "new " + bits_type_name(m, f) + "()";
        case FieldKind::Bytes:
            return f.len.kind == LenKind::Fixed ? "new Uint8Array(" + sz(f.len.fixed) + ")" : "new Uint8Array(0)";
        case FieldKind::String: return "''";
        case FieldKind::Message: return "new " + f.message->name + "()";
        case FieldKind::Array:
            if (f.len.kind == LenKind::Fixed)
                return "Array.from({ length: " + sz(f.len.fixed) + " }, () => " + default_value(m, *f.elem) + ")";
            return "[]";
        default: return "undefined";
    }
}

void print_computed(Ctx& c, const Message& m) {
    auto& w = c.w;
    for (const auto& cp : m.computed) {
        w.line("p.key(" + string_literal(cp.name) + ");");
        w.line(print_prim(cp.prim, "this." + ident(cp.name)));
        if (!cp.unit.empty()) w.line("p.unit(" + string_literal(cp.unit) + ");");
        w.line("p.note('computed');");
    }
}

// -------------------------------------------------------------------------------------------------
// bits groups
// -------------------------------------------------------------------------------------------------
void emit_bits_type(Ctx& c, const Message& m, const Field& f) {
    auto& w = c.w;
    const auto name = bits_type_name(m, f);
    const auto order = bit_order(f.bit_order);
    doc(w, f.doc.empty() ? "Bit fields of " + m.name + "." + f.name : f.doc);
    w.open("class " + name + " {");
    w.open("constructor(init) {");
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value) continue;
        const auto p = b.enum_def ? b.enum_def->underlying : b.prim;
        w.line("this." + ident(b.name) + " = " + int_literal(0, p) + ";");
    }
    w.line("if (init) Object.assign(this, init);");
    w.close();
    w.line("static wireSize = " + sz(f.bits_bytes) + ";");
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value || !(b.scale || b.offset_value)) continue;
        w.line("get " + b.name + "_scaled() { return Number(this." + ident(b.name) + ") * " + double_literal(b.scale.value_or(1.0)) +
               " + " + double_literal(b.offset_value.value_or(0.0)) + "; }");
    }
    w.open("static unpack(p, at = 0) {");
    w.line("const g = new " + name + "();");
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value) continue;
        w.line("g." + ident(b.name) + " = gmsg.getBits(p, at * 8 + " + sz(b.offset) + ", " + sz(b.bits) + ", " + order + ", " +
               q(bit_prim(b)) + ");");
    }
    w.line("return g;");
    w.close();
    w.open("static pack(g, p, at = 0) {");
    for (const auto& b : f.bits) {
        const auto pos = "at * 8 + " + sz(b.offset) + ", " + sz(b.bits) + ", " + order;
        if (b.pad) w.line("gmsg.setBits(p, " + pos + ", 0);");
        else if (b.const_value) w.line("gmsg.setBits(p, " + pos + ", " + big_literal(*b.const_value) + ");");
        else if (bit_prim(b) == "bool") w.line("gmsg.setBits(p, " + pos + ", !!g." + ident(b.name) + ");");
        else w.line("gmsg.setBits(p, " + pos + ", gmsg.checkInt(g." + ident(b.name) + ", " + q(bit_prim(b)) + ", " +
                    string_literal(b.name) + "));");
    }
    w.close();
    w.open("static check(p, at = 0) {");
    for (const auto& b : f.bits) {
        const auto pos = "at * 8 + " + sz(b.offset) + ", " + sz(b.bits) + ", " + order;
        if (b.const_value) {
            const auto u = static_cast<std::uint64_t>(*b.const_value);
            const auto lit = b.bits > 32 ? std::to_string(u) + "n" : std::to_string(u);
            w.line("if (gmsg.extractBits(p, " + pos + ") !== " + lit + ") " + fail("BadConst"));
        } else if (b.enum_def && b.enum_def->strict) {
            w.line("if (!" + b.enum_def->name + ".isKnown(gmsg.getBits(p, " + pos + ", " + q(bit_prim(b)) + "))) " +
                   fail("UnknownEnum"));
        }
    }
    w.close();
    w.open("printTo(p) {");
    w.line("p.begin('');");
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value) continue;
        const auto v = "this." + ident(b.name);
        w.line("p.key(" + string_literal(b.name) + ");");
        if (b.enum_def) w.line("p.enumValue(" + b.enum_def->name + ".nameOf(" + v + "), " + v + ", " +
                               sz(prim_size(b.enum_def->underlying)) + ");");
        else if (b.prim == Prim::Bool) w.line("p.bool(" + v + ");");
        else w.line("p.int(" + v + ");");
        if (b.scale || b.offset_value)
            w.line("p.conversion(Number(" + v + ") * " + double_literal(b.scale.value_or(1.0)) + " + " +
                   double_literal(b.offset_value.value_or(0.0)) + ");");
    }
    w.line("p.end();");
    w.close();
    w.line("toString(opts) { return gmsg.print(this, opts); }");
    w.close("}");
    w.line();
    c.exports.push_back(name);
}

// -------------------------------------------------------------------------------------------------
// struct
// -------------------------------------------------------------------------------------------------
class StructEmitter {
public:
    StructEmitter(Ctx& c, const Message& m) : c_(c), w_(c.w), m_(m) {
        read_ref_ = [](const Field& f, const BitField* b) {
            if (f.const_value) return typed_literal(f, *f.const_value);
            std::string s = "m." + ident(f.name);
            if (b) s += "." + ident(b->name);
            return s;
        };
        this_ref_ = [](const Field& f, const BitField* b) {
            if (f.const_value) return typed_literal(f, *f.const_value);
            std::string s = "this." + ident(f.name);
            if (b) s += "." + ident(b->name);
            return s;
        };
        // On encode a derived length / count field takes its value from the container it describes.
        write_ref_ = [this](const Field& f, const BitField* b) {
            if (f.derived_from && !b) return derived_count(f);
            return read_ref_(f, b);
        };
        ref_ = read_ref_;
    }

    static std::string derived_count(const Field& f) { return "gmsg.count(m." + ident(f.derived_from->name) + ")"; }

    void emit() {
        doc(w_, m_.doc);
        w_.open("class " + m_.name + " {");
        w_.open("constructor(init) {");
        for (const auto& f : m_.fields) {
            if (!f->has_member()) continue;
            std::string comment;
            if (!f->unit.empty()) comment = "[" + f->unit + "]";
            if (f->derived_from) comment += (comment.empty() ? "" : " ") + std::string("set from ") + f->derived_from->name + " on encode";
            w_.line("this." + ident(f->name) + " = " + (f->when ? std::string("undefined") : default_value(m_, *f)) + ";" +
                    (comment.empty() ? "" : "  // " + comment));
        }
        w_.line("if (init) Object.assign(this, init);");
        w_.close();
        w_.line("static messageName = " + string_literal(m_.name) + ";");
        w_.line("static minWireSize = " + sz(m_.min_size) + ";");
        if (m_.max_size) w_.line("static maxWireSize = " + sz(*m_.max_size) + ";");
        if (m_.static_size) w_.line("static wireSize = " + sz(*m_.static_size) + ";");
        for (const auto& f : m_.fields)
            if (f->const_value) w_.line("static " + f->name + "_value = " + typed_literal(*f, *f->const_value) + ";");
        w_.line();
        emit_conversions();
        emit_computed();
        emit_read();
        emit_write();
        w_.line("/** Decode from bytes (trailing bytes are ignored). Throws gmsg.GmpError. */");
        w_.line("static decode(bytes) { return " + m_.name + ".read(new gmsg.Reader(bytes)); }");
        w_.line("/** Encode `m` (this class or a plain object with the same fields) to a new Uint8Array. */");
        w_.line("static encode(m) { const w = new gmsg.Writer(); " + m_.name + ".write(w, m); return w.toBytes(); }");
        w_.line("encode() { return " + m_.name + ".encode(this); }");
        if (m_.want_view) w_.line("/** A validated zero-copy view over encoded bytes. */");
        if (m_.want_view) w_.line("static view(bytes) { return " + view_name(m_) + ".from(bytes); }");
        emit_print();
        w_.line("toString(opts) { return gmsg.print(this, opts); }");
        w_.close("}");
        w_.line();
        c_.exports.push_back(m_.name);
    }

private:
    void emit_conversions() {
        bool any = false;
        for (const auto& f : m_.fields) {
            if (!f->has_member()) continue;
            const auto n = "this." + ident(f->name);
            for (const auto& cv : conversions_of(*f)) {
                any = true;
                const auto scale = double_literal(cv.scale), off = double_literal(cv.offset);
                const auto acc = f->name + "_" + cv.suffix;
                if (!cv.unit.empty() && cv.suffix != "scaled") w_.line("// " + f->name + " in " + cv.unit);
                if (f->when) {
                    w_.line("get " + acc + "() { return " + n + " === undefined ? undefined : " + to_double(*f, n) + " * " +
                            scale + " + " + off + "; }");
                    continue;
                }
                w_.line("get " + acc + "() { return " + to_double(*f, n) + " * " + scale + " + " + off + "; }");
                std::string conv;
                if (f->prim == Prim::F32) conv = "Math.fround((v - " + off + ") / " + scale + ")";
                else if (f->prim == Prim::F64) conv = "(v - " + off + ") / " + scale;
                else if (f->prim != Prim::Bool && f->prim != Prim::Char)
                    conv = "gmsg.wrapInt(gmsg.llround((v - " + off + ") / " + scale + "), " + q(prim_id(f->prim)) + ")";
                if (!conv.empty()) w_.line("set " + acc + "(v) { " + n + " = " + conv + "; }");
            }
        }
        if (any) w_.line();
    }

    void emit_computed() {
        for (const auto& cp : m_.computed) {
            const auto n = ident(cp.name);
            if (!cp.doc.empty()) doc(w_, cp.doc);
            w_.line("get " + n + "() { return " + from_int(expr(*cp.value, this_ref_), cp.prim) + "; }");
            if (cp.setters.empty()) continue;
            w_.open("set " + n + "(v) {");
            w_.line("const value = gmsg.toInt(v);");
            for (const auto& [f, e] : cp.setters) w_.line("this." + ident(f->name) + " = " + from_int(expr(*e, this_ref_), *f) + ";");
            w_.close();
        }
        if (!m_.computed.empty()) w_.line();
    }

    // ---- read --------------------------------------------------------------------------------------
    void emit_read() {
        w_.open("static read(r) {");
        w_.line("const m = new " + m_.name + "();");
        for (const auto& f : m_.fields) {
            const auto dst = f->has_member() ? "m." + ident(f->name) : std::string();
            if (f->when) {
                w_.open("if (" + expr(*f->when, ref_) + " !== 0n) {");
                read_value(*f, dst, 0);
                w_.close();
            } else {
                read_value(*f, dst, 0);
            }
        }
        w_.line("return m;");
        w_.close();
    }

    void read_value(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: {
                const auto t = q(prim_id(storage_prim(f)));
                const auto n = prim_size(storage_prim(f));
                w_.open("{");
                if (f.crc) {
                    w_.line("const b" + d + " = r.view(" + sz(n + f.crc->size) + ");");
                    w_.line("if (gmsg.loadUintBig(b" + d + ", " + sz(n) + ", " + sz(f.crc->size) + ", " + endian(f.crc->endian) +
                            ") !== " + checksum_value(c_, *f.crc, "b" + d, "0", sz(n)) + ") " + fail("BadChecksum"));
                    w_.line("const v" + d + " = gmsg.load(b" + d + ", 0, " + t + ", " + endian(f.endian) + ");");
                } else {
                    w_.line("const v" + d + " = r.read(" + t + ", " + endian(f.endian) + ");");
                }
                const auto val = "v" + d;
                if (strict_enum(f)) w_.line("if (!" + f.enum_def->name + ".isKnown(" + val + ")) " + fail("UnknownEnum"));
                if (f.const_value) w_.line("if (" + val + " !== " + typed_literal(f, *f.const_value) + ") " + fail("BadConst"));
                else if (!dst.empty()) w_.line(dst + " = " + val + ";");
                w_.close();
                break;
            }
            case FieldKind::Bits: {
                const auto t = bits_type_name(m_, f);
                w_.open("{");
                w_.line("const b" + d + " = r.view(" + sz(f.bits_bytes) + ");");
                w_.line(t + ".check(b" + d + ");");
                w_.line(dst + " = " + t + ".unpack(b" + d + ");");
                w_.close();
                break;
            }
            case FieldKind::Pad: w_.line("r.skip(" + sz(f.len.fixed) + ");"); break;
            case FieldKind::Bytes:
            case FieldKind::String: read_blob(f, dst, depth); break;
            case FieldKind::Array: read_array(f, dst, depth); break;
            case FieldKind::Message: w_.line(dst + " = " + f.message->name + ".read(r);"); break;
            case FieldKind::Custom: {
                const auto fn = ext_fn(c_, f.custom->lang, "decode");
                if (f.custom->wire_size) w_.line(dst + " = " + fn + "(new gmsg.Reader(r.view(" + sz(*f.custom->wire_size) + ")));");
                else w_.line(dst + " = " + fn + "(r);");
                break;
            }
        }
    }

    void read_blob(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        const auto b = "b" + d;
        const auto max = sz(f.max.value_or(0));
        w_.open("{");
        switch (f.len.kind) {
            case LenKind::Fixed: w_.line("const " + b + " = r.take(" + sz(f.len.fixed) + ");"); break;
            case LenKind::Expr:
                w_.line("const n" + d + " = " + expr(*f.len.expr, ref_) + ";");
                w_.line("if (n" + d + " < 0n || n" + d + " > " + max + "n) " + fail("OutOfRange"));
                w_.line("const " + b + " = r.take(Number(n" + d + "));");
                break;
            case LenKind::Remaining:
                w_.line("if (r.remaining > " + max + ") " + fail("OutOfRange"));
                w_.line("const " + b + " = r.take(r.remaining);");
                break;
            case LenKind::Terminator:
                w_.line("const " + b + " = r.readUntil(" + sz(f.len.terminator) + ");");
                w_.line("if (" + b + ".length > " + max + ") " + fail("OutOfRange"));
                break;
            case LenKind::None: w_.line("const " + b + " = new Uint8Array(0);"); break;
        }
        if (f.kind == FieldKind::String) {
            if (f.len.kind == LenKind::Fixed)
                w_.line(dst + " = gmsg.stripPadding(gmsg.latin1String(" + b + "), " +
                        sz(static_cast<unsigned char>(f.pad_char)) + ");");
            else
                w_.line(dst + " = gmsg.latin1String(" + b + ");");
        } else {
            w_.line(dst + " = " + b + ";");
        }
        w_.close();
    }

    void read_array(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        const auto i = "i" + d;
        const auto& e = *f.elem;
        switch (f.len.kind) {
            case LenKind::Fixed:
                w_.line(dst + " = new Array(" + sz(f.len.fixed) + ");");
                w_.open("for (let " + i + " = 0; " + i + " < " + sz(f.len.fixed) + "; " + i + "++) {");
                read_value(e, dst + "[" + i + "]", depth + 1);
                w_.close();
                break;
            case LenKind::Expr:
                w_.open("{");
                w_.line("const n" + d + " = " + expr(*f.len.expr, ref_) + ";");
                w_.line("if (n" + d + " < 0n || n" + d + " > " + sz(f.max.value_or(0)) + "n) " + fail("OutOfRange"));
                w_.line(dst + " = new Array(Number(n" + d + "));");
                w_.open("for (let " + i + " = 0; " + i + " < " + dst + ".length; " + i + "++) {");
                read_value(e, dst + "[" + i + "]", depth + 1);
                w_.close();
                w_.close();
                break;
            case LenKind::Remaining:
                w_.line(dst + " = [];");
                w_.open("while (r.remaining > 0) {");
                w_.line("if (" + dst + ".length >= " + sz(f.max.value_or(0)) + ") " + fail("OutOfRange"));
                w_.line(dst + ".push(undefined);");
                read_value(e, dst + "[" + dst + ".length - 1]", depth + 1);
                w_.close();
                break;
            default: break;
        }
    }

    // ---- write -------------------------------------------------------------------------------------
    void emit_write() {
        ref_ = write_ref_;
        w_.open("static write(w, m) {");
        for (const auto& f : m_.fields) {
            const auto src = f->has_member() ? "m." + ident(f->name) : std::string();
            if (f->when) {
                w_.open("if (" + expr(*f->when, ref_) + " !== 0n) {");
                w_.line("if (" + src + " === undefined || " + src + " === null) " + fail("MissingField"));
                write_value(*f, src, 0);
                w_.close();
            } else {
                write_value(*f, src, 0);
            }
        }
        w_.close();
        ref_ = read_ref_;
    }

    void write_value(const Field& f, const std::string& src, int depth) {
        const auto d = dv(depth);
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: {
                const auto p = storage_prim(f);
                const auto t = q(prim_id(p));
                std::string val = src;
                if (f.const_value) {
                    val = typed_literal(f, *f.const_value);
                } else if (f.derived_from) {
                    const auto max = prim_is_signed(p) ? (std::uint64_t{1} << (8 * prim_size(p) - 1)) - 1
                                                       : (prim_size(p) >= 8 ? ~std::uint64_t{0} : (std::uint64_t{1} << (8 * prim_size(p))) - 1);
                    w_.line("const c" + d + " = " + derived_count(f) + ";");
                    w_.line("if (c" + d + " > " + std::to_string(max) + ") " + fail("OutOfRange"));
                    val = is_big(p) ? "BigInt(c" + d + ")" : "c" + d;
                } else if (strict_enum(f)) {
                    w_.line("if (!" + f.enum_def->name + ".isKnown(" + val + ")) " + fail("UnknownEnum"));
                }
                w_.line("w.write(" + t + ", " + val + ", " + endian(f.endian) + ");");
                if (f.crc) {
                    const auto n = sz(prim_size(p));
                    w_.line("w.uint(" + checksum_value(c_, *f.crc, "w.bytes", "w.pos - " + n, "w.pos") + ", " + sz(f.crc->size) +
                            ", " + endian(f.crc->endian) + ");");
                }
                break;
            }
            case FieldKind::Bits:
                for (const auto& b : f.bits)
                    if (!b.pad && !b.const_value && b.enum_def && b.enum_def->strict)
                        w_.line("if (!" + b.enum_def->name + ".isKnown(" + src + "." + ident(b.name) + ")) " + fail("UnknownEnum"));
                w_.open("{");
                w_.line("const at" + d + " = w.pos;");
                w_.line("w.zeros(" + sz(f.bits_bytes) + ");");
                w_.line(bits_type_name(m_, f) + ".pack(" + src + ", w.buf, at" + d + ");");
                w_.close();
                break;
            case FieldKind::Pad: w_.line("w.zeros(" + sz(f.len.fixed) + ");"); break;
            case FieldKind::Bytes:
            case FieldKind::String: write_blob(f, src, depth); break;
            case FieldKind::Array: write_array(f, src, depth); break;
            case FieldKind::Message: w_.line(f.message->name + ".write(w, " + src + ");"); break;
            case FieldKind::Custom: {
                const auto fn = ext_fn(c_, f.custom->lang, "encode");
                w_.open("{");
                w_.line("const p" + d + " = w.pos;");
                w_.line(fn + "(w, " + src + ");");
                if (f.custom->wire_size)
                    w_.line("if (w.pos - p" + d + " !== " + sz(*f.custom->wire_size) + ") " + fail("InvalidValue"));
                w_.close();
                break;
            }
        }
    }

    void length_check(const Field& f, const std::string& size_expr) {
        if (f.len.kind == LenKind::Expr && !is_derived_length(f))
            w_.line("if (BigInt(" + size_expr + ") !== " + expr(*f.len.expr, ref_) + ") " + fail("BadLength"));
    }

    /// What the C++ container type guarantees: fixed size, or at most `max` elements.
    void capacity_check(const Field& f, const std::string& size_expr) {
        if (f.len.kind == LenKind::Fixed) {
            if (f.kind != FieldKind::String) w_.line("if (" + size_expr + " !== " + sz(f.len.fixed) + ") " + fail("OutOfRange"));
        } else {
            w_.line("if (" + size_expr + " > " + sz(f.max.value_or(0)) + ") " + fail("OutOfRange"));
        }
    }

    void write_blob(const Field& f, const std::string& src, int depth) {
        const auto d = dv(depth);
        const bool str = f.kind == FieldKind::String;
        const auto b = "b" + d;
        w_.open("{");
        w_.line("const " + b + " = " + (str ? "gmsg.latin1Bytes(" + src + ")" : "gmsg.toBytes(" + src + ")") + ";");
        capacity_check(f, b + ".length");
        length_check(f, b + ".length");
        if (f.len.kind == LenKind::Fixed && str) w_.line("if (" + b + ".length > " + sz(f.len.fixed) + ") " + fail("OutOfRange"));
        w_.line("w.put(" + b + ");");
        if (f.len.kind == LenKind::Fixed && str)
            w_.line("w.fill(" + sz(static_cast<unsigned char>(f.pad_char)) + ", " + sz(f.len.fixed) + " - " + b + ".length);");
        if (f.len.kind == LenKind::Terminator) w_.line("w.u8(" + sz(f.len.terminator) + ");");
        w_.close();
    }

    void write_array(const Field& f, const std::string& src, int depth) {
        const auto d = dv(depth);
        capacity_check(f, src + ".length");
        length_check(f, src + ".length");
        w_.open("for (const x" + d + " of " + src + ") {");
        write_value(*f.elem, "x" + d, depth + 1);
        w_.close();
    }

    void emit_print() {
        w_.open("printTo(p) {");
        w_.line("p.begin(" + string_literal(m_.name) + ");");
        for (const auto& f : m_.fields) {
            if (!f->has_member()) continue;
            const auto v = "this." + ident(f->name);
            if (f->when) w_.open("if (" + v + " !== undefined) {");
            w_.line("p.key(" + string_literal(f->name) + ");");
            w_.line(print_value(c_, m_, *f, v));
            emit_print_extras(w_, *f, v);
            if (f->when) {
                w_.dedent();
                w_.open("} else {");
                w_.line("p.absent(" + string_literal(f->name) + ");");
                w_.close();
            }
        }
        print_computed(c_, m_);
        w_.line("p.end();");
        w_.close();
    }

    Ctx& c_;
    CodeWriter& w_;
    const Message& m_;
    RefFn read_ref_, this_ref_, write_ref_, ref_;
};

// -------------------------------------------------------------------------------------------------
// view
// -------------------------------------------------------------------------------------------------
class ViewEmitter {
public:
    ViewEmitter(Ctx& c, const Message& m) : c_(c), w_(c.w), m_(m), name_(view_name(m)) {
        dynamic_ = !m.fixed_layout;
        walk_ref_ = [](const Field& f, const BitField* b) {
            if (f.const_value) return typed_literal(f, *f.const_value);
            std::string s = "v." + ident(f.name);
            if (b) s += "." + ident(b->name);
            return s;
        };
        acc_ref_ = [](const Field& f, const BitField* b) {
            if (f.const_value) return typed_literal(f, *f.const_value);
            std::string s = "this." + ident(f.name);
            if (b) s += "." + ident(b->name);
            return s;
        };
        std::size_t k = 0;
        for (const auto& f : m.fields)
            if (f->when) cond_bit_[f.get()] = k++;
    }

    void emit() {
        doc(w_, "Zero-copy view over an encoded " + m_.name + "; fields are read on access.");
        w_.open("class " + name_ + " {");
        w_.line("#b;");
        w_.line("#o;");
        w_.line("#p;");
        w_.line("constructor(b, o, p) { this.#b = b; this.#o = o; this.#p = p; }");
        w_.line("static messageName = " + string_literal(m_.name) + ";");
        w_.line("static minWireSize = " + sz(m_.min_size) + ";");
        if (m_.static_size) w_.line("static wireSize = " + sz(*m_.static_size) + ";");
        w_.line();
        emit_from();
        emit_unchecked();
        w_.line("/** The message bytes. */");
        w_.line("get data() { return this.#b; }");
        w_.line("get wireSize() { return this.#b.length; }");
        for (const auto& f : m_.fields) emit_accessor(*f);
        emit_computed_accessors();
        emit_ranges();
        w_.open("printTo(p) {");
        w_.line("p.begin(" + string_literal(m_.name) + ");");
        for (const auto& f : m_.fields) emit_print_field(*f);
        print_computed(c_, m_);
        w_.line("p.end();");
        w_.close();
        w_.line("toString(opts) { return gmsg.print(this, opts); }");
        if (m_.want_struct) w_.line("/** Decode into the struct class. */");
        if (m_.want_struct) w_.line("toStruct() { return " + m_.name + ".decode(this.#b); }");
        w_.close("}");
        w_.line();
        c_.exports.push_back(name_);
    }

private:
    std::string off(const Field& f) const {
        if (f.static_offset) return sz(*f.static_offset);
        return "this.#o[" + sz(f.index) + "]";
    }
    std::string size(const Field& f) const {
        if (f.static_size) return sz(*f.static_size);
        return "(this.#o[" + sz(f.index + 1) + "] - this.#o[" + sz(f.index) + "])";
    }

    /// Validation of a field with static size at byte offset expression `o` of `data`.
    void validate_static(const Field& f, const std::string& o) {
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: {
                const auto t = q(prim_id(storage_prim(f)));
                if (f.crc) {
                    const auto n = sz(prim_size(storage_prim(f)));
                    w_.line("if (gmsg.loadUintBig(data, " + o + " + " + n + ", " + sz(f.crc->size) + ", " + endian(f.crc->endian) +
                            ") !== " + checksum_value(c_, *f.crc, "data", o, o + " + " + n) + ") " + fail("BadChecksum"));
                }
                if (f.const_value)
                    w_.line("if (gmsg.load(data, " + o + ", " + t + ", " + endian(f.endian) + ") !== " +
                            typed_literal(f, *f.const_value) + ") " + fail("BadConst"));
                else if (strict_enum(f))
                    w_.line("if (!" + f.enum_def->name + ".isKnown(gmsg.load(data, " + o + ", " + t + ", " + endian(f.endian) +
                            "))) " + fail("UnknownEnum"));
                break;
            }
            case FieldKind::Bits:
                if (bits_need_check(f)) w_.line(bits_type_name(m_, f) + ".check(data, " + o + ");");
                break;
            case FieldKind::Message:
                if (f.message->needs_validation)
                    w_.line(view_name(*f.message) + ".from(data.subarray(" + o + ", " + o + " + " + sz(*f.static_size) + "));");
                break;
            case FieldKind::Array: {
                const auto& e = *f.elem;
                if (e.kind == FieldKind::Message && e.message->needs_validation) {
                    const auto es = sz(*e.static_size);
                    w_.open("for (let k = 0; k < " + sz(f.len.fixed) + "; k++) {");
                    w_.line(view_name(*e.message) + ".from(data.subarray(" + o + " + k * " + es + ", " + o + " + (k + 1) * " + es + "));");
                    w_.close();
                } else if (e.const_value) {
                    const auto es = sz(*e.static_size);
                    w_.open("for (let k = 0; k < " + sz(f.len.fixed) + "; k++) {");
                    w_.line("if (gmsg.load(data, " + o + " + k * " + es + ", " + q(prim_id(storage_prim(e))) + ", " +
                            endian(e.endian) + ") !== " + typed_literal(e, *e.const_value) + ") " + fail("BadConst"));
                    w_.close();
                } else if (strict_enum(e)) {
                    emit_enum_array_check(e, o, sz(f.len.fixed));
                }
                break;
            }
            default: break;
        }
    }

    void emit_enum_array_check(const Field& e, const std::string& o, const std::string& count) {
        const auto es = sz(prim_size(storage_prim(e)));
        w_.open("for (let k = 0; k < " + count + "; k++) {");
        w_.line("if (!" + e.enum_def->name + ".isKnown(gmsg.load(data, " + o + " + k * " + es + ", " +
                q(prim_id(storage_prim(e))) + ", " + endian(e.endian) + "))) " + fail("UnknownEnum"));
        w_.close();
    }

    void emit_from() {
        w_.line("/** Validate `data` and build a view. Trailing bytes beyond the message are ignored. Throws GmpError. */");
        w_.open("static from(data) {");
        w_.line("data = gmsg.toBytes(data);");
        if (!dynamic_) {
            w_.line("if (data.length < " + sz(*m_.static_size) + ") " + fail("Truncated"));
            for (const auto& f : m_.fields) validate_static(*f, sz(*f->static_offset));
            w_.line("return new " + name_ + "(data.subarray(0, " + sz(*m_.static_size) + "), null, null);");
            w_.close();
            return;
        }
        w_.line("const v = new " + name_ + "(data, new Array(" + sz(m_.fields.size() + 1) + ").fill(0), [" +
                std::string(m_.conditional_count ? "" : "") + "]);");
        w_.line("let pos = 0;");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            w_.line("v.#o[" + sz(f.index) + "] = pos;");
            if (f.when) {
                w_.open("if (" + expr(*f.when, walk_ref_) + " !== 0n) {");
                w_.line("v.#p[" + sz(cond_bit_.at(&f)) + "] = true;");
            }
            walk_field(f);
            if (f.when) w_.close();
        }
        w_.line("v.#o[" + sz(m_.fields.size()) + "] = pos;");
        w_.line("v.#b = data.subarray(0, pos);");
        w_.line("return v;");
        w_.close();
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
            w_.line("if (data.length - pos < " + sz(*fixed) + ") " + fail("Truncated"));
            if (f.kind == FieldKind::Message && f.message->needs_validation) {
                w_.line(view_name(*f.message) + ".from(data.subarray(pos, pos + " + sz(*fixed) + "));");
            } else if (f.kind == FieldKind::Array && f.elem->kind == FieldKind::Message && f.elem->message->needs_validation) {
                const auto es = sz(*f.elem->static_size);
                w_.open("for (let k = 0; k < " + sz(f.len.fixed) + "; k++) {");
                w_.line(view_name(*f.elem->message) + ".from(data.subarray(pos + k * " + es + ", pos + (k + 1) * " + es + "));");
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
                        w_.line("const n = " + expr(*f.len.expr, walk_ref_) + ";");
                        w_.line("if (n < 0n || n > BigInt(data.length - pos)) " + fail("Truncated"));
                        w_.line("pos += Number(n);");
                        w_.close();
                        break;
                    case LenKind::Remaining: w_.line("pos = data.length;"); break;
                    case LenKind::Terminator:
                        w_.open("{");
                        w_.line("let k = pos;");
                        w_.line("while (k < data.length && data[k] !== " + sz(f.len.terminator) + ") k++;");
                        w_.line("if (k === data.length) " + fail("Truncated"));
                        w_.line("pos = k + 1;");
                        w_.close();
                        break;
                    default: break;
                }
                break;
            case FieldKind::Array: walk_array(f); break;
            case FieldKind::Message:
                w_.line("pos += " + view_name(*f.message) + ".from(data.subarray(pos)).wireSize;");
                break;
            case FieldKind::Custom:
                w_.open("{");
                w_.line("const cr = new gmsg.Reader(data.subarray(pos));");
                w_.line(ext_fn(c_, f.custom->lang, "decode") + "(cr);");
                w_.line("pos += cr.pos;");
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
                w_.line("if ((data.length - pos) % " + sz(*es) + " !== 0) " + fail("BadLength"));
                if (strict_enum(e)) emit_enum_array_check(e, "pos", "(data.length - pos) / " + sz(*es));
                w_.line("pos = data.length;");
            } else {
                w_.line("while (pos < data.length) pos += " + view_name(*e.message) + ".from(data.subarray(pos)).wireSize;");
            }
        } else {
            if (f.len.kind == LenKind::Fixed) w_.line("const n = " + sz(f.len.fixed) + "n;");
            else w_.line("const n = " + expr(*f.len.expr, walk_ref_) + ";");
            w_.line("if (n < 0n) " + fail("BadLength"));
            if (es && !validate) {
                w_.line("if (n * " + sz(*es) + "n > BigInt(data.length - pos)) " + fail("Truncated"));
                if (strict_enum(e)) emit_enum_array_check(e, "pos", "Number(n)");
                w_.line("pos += Number(n) * " + sz(*es) + ";");
            } else {
                w_.line("for (let k = 0n; k < n; k++) pos += " + view_name(*e.message) + ".from(data.subarray(pos)).wireSize;");
            }
        }
        w_.close();
    }

    void emit_unchecked() {
        w_.line("/** A view over data already validated by from() (no checks). */");
        if (!dynamic_)
            w_.line("static unchecked(data) { return new " + name_ + "(data.subarray(0, " + sz(*m_.static_size) + "), null, null); }");
        else
            w_.line("static unchecked(data) { return " + name_ + ".from(data); }");
        w_.line();
    }

    std::string accessor_body(const Field& f) const {
        const auto o = off(f), z = size(f);
        const auto sub = "this.#b.subarray(" + o + ", " + o + " + " + z + ")";
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum:
                return "gmsg.load(this.#b, " + o + ", " + q(prim_id(storage_prim(f))) + ", " + endian(f.endian) + ")";
            case FieldKind::Bits: return bits_type_name(m_, f) + ".unpack(this.#b, " + o + ")";
            case FieldKind::Bytes: return sub;
            case FieldKind::String:
                if (f.len.kind == LenKind::Fixed)
                    return "gmsg.stripPadding(gmsg.latin1String(this.#b, " + o + ", " + o + " + " + z + "), " +
                           sz(static_cast<unsigned char>(f.pad_char)) + ")";
                if (f.len.kind == LenKind::Terminator) return "gmsg.latin1String(this.#b, " + o + ", " + o + " + " + z + " - 1)";
                return "gmsg.latin1String(this.#b, " + o + ", " + o + " + " + z + ")";
            case FieldKind::Message: return view_name(*f.message) + ".unchecked(" + sub + ")";
            case FieldKind::Custom: return ext_fn(c_, f.custom->lang, "decode") + "(new gmsg.Reader(" + sub + "))";
            case FieldKind::Array: {
                const auto& e = *f.elem;
                if (e.kind == FieldKind::Message) {
                    if (e.static_size) {
                        const auto es = sz(*e.static_size);
                        return "gmsg.fixedArray(" + sub + ", " + es + ", (b) => " + view_name(*e.message) + ".unchecked(b))";
                    }
                    return "gmsg.seqArray(" + sub + ", (b) => " + view_name(*e.message) + ".unchecked(b))";
                }
                const auto es = sz(prim_size(storage_prim(e)));
                return "gmsg.fixedArray(" + sub + ", " + es + ", (b) => gmsg.load(b, 0, " + q(prim_id(storage_prim(e))) + ", " +
                       endian(e.endian) + "))";
            }
            default: return "undefined";
        }
    }

    void emit_accessor(const Field& f) {
        if (!f.has_member()) return;
        const auto n = ident(f.name);
        if (!f.unit.empty()) w_.line("// [" + f.unit + "]");
        if (f.when)
            w_.line("get " + n + "() { if (!this.#p[" + sz(cond_bit_.at(&f)) + "]) return undefined; return " + accessor_body(f) + "; }");
        else
            w_.line("get " + n + "() { return " + accessor_body(f) + "; }");
        for (const auto& cv : conversions_of(f)) {
            const auto acc = f.name + "_" + cv.suffix;
            const auto conv = to_double(f, "x") + " * " + double_literal(cv.scale) + " + " + double_literal(cv.offset);
            if (f.when) w_.line("get " + acc + "() { const x = this." + n + "; return x === undefined ? undefined : " + conv + "; }");
            else w_.line("get " + acc + "() { const x = this." + n + "; return " + conv + "; }");
        }
    }

    void emit_computed_accessors() {
        for (const auto& cp : m_.computed) {
            if (!cp.doc.empty()) doc(w_, cp.doc);
            w_.line("get " + ident(cp.name) + "() { return " + from_int(expr(*cp.value, acc_ref_), cp.prim) + "; }");
        }
    }

    /// ranges(): [{ name, start, end }] of every present field (padding included), relative to data.
    void emit_ranges() {
        w_.line("/** Byte ranges of the present fields (relative to data), in wire order: { name, key, start, end }; key is the");
        w_.line(" *  property (null for padding and constants). */");
        w_.open("ranges() {");
        w_.line("const out = [];");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            const auto o = off(f), z = size(f);
            const auto item = "out.push({ name: " + string_literal(f.name) + ", key: " +
                              (f.has_member() ? string_literal(ident(f.name)) : std::string("null")) + ", start: " + o +
                              ", end: " + o + " + " + z + " });";
            if (f.when) w_.line("if (this.#p[" + sz(cond_bit_.at(&f)) + "]) " + item);
            else w_.line(item);
        }
        w_.line("return out;");
        w_.close();
    }

    void emit_print_field(const Field& f) {
        if (!f.has_member()) return;
        const auto acc = "this." + ident(f.name);
        const bool may_fail = f.kind == FieldKind::Custom;
        w_.open("{");
        if (may_fail) {
            w_.line("const [e, x] = gmsg.attempt(() => " + acc + ");");
            if (f.when) w_.line("if (!e && x === undefined) p.absent(" + string_literal(f.name) + ");");
            w_.open(f.when ? "else {" : "{");
            w_.line("p.key(" + string_literal(f.name) + ");");
            w_.line("if (e) p.error(e); else " + print_value(c_, m_, f, "x"));
            w_.close();
        } else {
            w_.line("const x = " + acc + ";");
            if (f.when) w_.line("if (x === undefined) p.absent(" + string_literal(f.name) + ");");
            w_.open(f.when ? "else {" : "{");
            w_.line("p.key(" + string_literal(f.name) + ");");
            w_.line(print_value(c_, m_, f, "x"));
            emit_print_extras(w_, f, "x");
            w_.close();
        }
        w_.close();
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

} // namespace gmpc::js
