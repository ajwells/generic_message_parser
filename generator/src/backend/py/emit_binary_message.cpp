// Python emission for binary messages: bits group dataclasses, message dataclasses and lazy view classes.
//
// Semantics follow backend/js/emit_binary_message.cpp (and so the C++ backend) check for check: same order, same
// error codes. Python containers have no capacity, so encode also checks what the C++ types guarantee: variable bytes
// / strings / lists hold at most `max` elements and fixed ones exactly their size (OutOfRange otherwise), and values
// fit their type.

#include <map>
#include <string>

#include "backend/py/py_common.hpp"

namespace gmpc::py {

namespace {

using namespace ir;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string dv(int depth) { return std::to_string(depth); }
std::string fail(const char* err) { return std::string("gmsg.fail(Err.") + err + ")"; }

bool strict_enum(const Field& f) { return f.kind == FieldKind::Enum && f.enum_def && f.enum_def->strict; }

bool bits_need_check(const Field& f) {
    for (const auto& b : f.bits)
        if (b.const_value || (b.enum_def && b.enum_def->strict) || !b.limit_checks.empty()) return true;
    return false;
}

/// `if <v fails the limits>: fail(ValueOutOfLimits)` (nothing when there are none).
void limit_stmt(CodeWriter& w, const Field& f, const std::string& v) {
    if (f.limit_checks.empty()) return;
    w.line("if " + limit_fail(f.limit_checks, v, f.prim, f.scale, f.offset_value) + ": " + fail("VALUE_OUT_OF_LIMITS"));
}
void limit_stmt(CodeWriter& w, const BitField& b, const std::string& v) {
    if (b.limit_checks.empty()) return;
    w.line("if " + limit_fail(b.limit_checks, v, b.prim, b.scale, b.offset_value) + ": " + fail("VALUE_OUT_OF_LIMITS"));
}

std::string bit_prim(const BitField& b) { return prim_id(b.enum_def ? b.enum_def->underlying : b.prim); }

std::string bit_value(const BitField& b, const std::string& v) {
    return b.enum_def ? b.enum_def->name + ".of(" + v + ")" : v;
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

/// Expression printing a computed value of type p read through `v`.
std::string print_prim(Prim p, const std::string& v) {
    switch (p) {
        case Prim::F32: return "p.float_value(gmsg.to_f32(" + v + "), True)";
        case Prim::F64: return "p.float_value(" + v + ")";
        case Prim::Bool: return "p.bool_value(" + v + ")";
        case Prim::Char: return "p.char_value(" + v + ")";
        default: return "p.int_value(" + v + ")";
    }
}

std::string prim_type(Prim p) {
    switch (p) {
        case Prim::F32: case Prim::F64: return "float";
        case Prim::Bool: return "bool";
        case Prim::Char: return "str";
        default: return "int";
    }
}

/// Type hint of one field element.
std::string elem_type(const Message& m, const Field& f) {
    switch (f.kind) {
        case FieldKind::Scalar: return prim_type(f.prim);
        case FieldKind::Enum: return f.enum_def->name + " | int";
        case FieldKind::Bits: return bits_type_name(m, f);
        case FieldKind::Bytes: return "bytes";
        case FieldKind::String: return "str";
        case FieldKind::Message: return f.message->name;
        case FieldKind::Array: return "list[" + elem_type(m, *f.elem) + "]";
        default: return "object";
    }
}

/// A new value of one field element (an expression evaluated on every use).
std::string new_value(const Message& m, const Field& f) {
    switch (f.kind) {
        case FieldKind::Scalar: return typed_literal(f, 0);
        case FieldKind::Enum: return f.enum_def->name + ".of(" + typed_literal(f, 0) + ")";
        case FieldKind::Bits: return bits_type_name(m, f) + "()";
        case FieldKind::Bytes: return f.len.kind == LenKind::Fixed ? "bytes(" + sz(f.len.fixed) + ")" : "b''";
        case FieldKind::String: return "''";
        case FieldKind::Message: return f.message->name + "()";
        case FieldKind::Array:
            if (f.len.kind == LenKind::Fixed) return "[" + new_value(m, *f.elem) + " for _ in range(" + sz(f.len.fixed) + ")]";
            return "[]";
        default: return "None";
    }
}

/// Default of a dataclass field: immutable values directly, others through a factory.
std::string field_default(const Message& m, const Field& f) {
    switch (f.kind) {
        case FieldKind::Bits:
        case FieldKind::Message:
            return "_field(default_factory=lambda: " + new_value(m, f) + ")";
        case FieldKind::Array:
            if (f.len.kind != LenKind::Fixed) return "_field(default_factory=list)";
            return "_field(default_factory=lambda: " + new_value(m, f) + ")";
        default: return new_value(m, f);
    }
}

void print_computed(CodeWriter& w, const Message& m) {
    for (const auto& cp : m.computed) {
        w.line("p.key(" + string_literal(cp.name) + ")");
        w.line(print_prim(cp.prim, "self." + ident(cp.name)));
        if (!cp.unit.empty()) w.line("p.unit(" + string_literal(cp.unit) + ")");
        w.line("p.note('computed')");
    }
}

void text_methods(CodeWriter& w) {
    w.line("def to_text(self, **options) -> str:");
    w.line("    return gmsg.to_text(self, **options)");
    w.line();
    w.line("def __str__(self) -> str:");
    w.line("    return gmsg.to_text(self)");
}

// -------------------------------------------------------------------------------------------------
// bits groups
// -------------------------------------------------------------------------------------------------
void emit_bits_type(Ctx& c, const Message& m, const Field& f) {
    auto& w = c.w;
    const auto name = bits_type_name(m, f);
    const auto order = bit_order(f.bit_order);
    w.line("@_dataclass(slots=True)");
    w.open("class " + name + ":");
    w.line("# " + (f.doc.empty() ? "Bit fields of " + m.name + "." + f.name : f.doc));
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value) continue;
        const auto p = b.enum_def ? b.enum_def->underlying : b.prim;
        const auto type = b.enum_def ? b.enum_def->name + " | int" : prim_type(p);
        w.line(ident(b.name) + ": " + type + " = " + bit_value(b, int_literal(0, p)));
    }
    w.line("wire_size: _ClassVar[int] = " + sz(f.bits_bytes));
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value || !(b.scale || b.offset_value)) continue;
        w.line();
        w.line("@property");
        w.open("def " + b.name + "_scaled(self) -> float:");
        w.line("return float(self." + ident(b.name) + ") * " + double_literal(b.scale.value_or(1.0)) + " + " +
               double_literal(b.offset_value.value_or(0.0)));
        w.dedent();
    }
    w.line();
    w.line("@staticmethod");
    w.open("def unpack(p, at: int = 0) -> " + name + ":");
    std::string args;
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value) continue;
        args += (args.empty() ? "" : ", ") + ident(b.name) + "=" +
                bit_value(b, "gmsg.get_bits(p, at * 8 + " + sz(b.offset) + ", " + sz(b.bits) + ", " + order + ", " + bit_prim(b) + ")");
    }
    w.line("return " + name + "(" + args + ")");
    w.dedent();
    w.line();
    w.line("@staticmethod");
    w.open("def pack(g: " + name + ", p: bytearray, at: int = 0) -> None:");
    if (f.bits.empty()) w.line("pass");
    for (const auto& b : f.bits) {
        const auto pos = "at * 8 + " + sz(b.offset) + ", " + sz(b.bits) + ", " + order;
        if (b.pad) w.line("gmsg.set_bits(p, " + pos + ", 0)");
        else if (b.const_value) w.line("gmsg.set_bits(p, " + pos + ", " + std::to_string(*b.const_value) + ")");
        else if (bit_prim(b) == "'bool'") w.line("gmsg.set_bits(p, " + pos + ", bool(g." + ident(b.name) + "))");
        else w.line("gmsg.set_bits(p, " + pos + ", gmsg.check_int(g." + ident(b.name) + ", " + bit_prim(b) + ", " +
                    string_literal(b.name) + "))");
    }
    w.dedent();
    w.line();
    w.line("@staticmethod");
    w.open("def check(p, at: int = 0) -> None:");
    bool any = false;
    for (const auto& b : f.bits) {
        const auto pos = "at * 8 + " + sz(b.offset) + ", " + sz(b.bits) + ", " + order;
        if (b.const_value) {
            const auto u = static_cast<std::uint64_t>(*b.const_value);
            w.line("if gmsg.extract_bits(p, " + pos + ") != " + std::to_string(u) + ": " + fail("BAD_CONST"));
            any = true;
        } else if (b.enum_def && b.enum_def->strict) {
            w.line("if not " + b.enum_def->name + ".is_known(gmsg.get_bits(p, " + pos + ", " + bit_prim(b) + ")): " +
                   fail("UNKNOWN_ENUM"));
            any = true;
        } else if (!b.pad && !b.limit_checks.empty()) {
            w.line("v = gmsg.get_bits(p, " + pos + ", " + bit_prim(b) + ")");
            limit_stmt(w, b, "v");
            any = true;
        }
    }
    if (!any) w.line("pass");
    w.dedent();
    w.line();
    w.open("def print_to(self, p: gmsg.Printer) -> None:");
    w.line("p.begin('')");
    for (const auto& b : f.bits) {
        if (b.pad || b.const_value) continue;
        const auto v = "self." + ident(b.name);
        w.line("p.key(" + string_literal(b.name) + ")");
        if (b.enum_def) w.line("p.enum_value(" + b.enum_def->name + ".name_of(" + v + "), " + v + ", " +
                               sz(prim_size(b.enum_def->underlying)) + ")");
        else if (b.prim == Prim::Bool) w.line("p.bool_value(" + v + ")");
        else w.line("p.int_value(" + v + ")");
        if (b.scale || b.offset_value)
            w.line("p.conversion(float(" + v + ") * " + double_literal(b.scale.value_or(1.0)) + " + " +
                   double_literal(b.offset_value.value_or(0.0)) + ")");
    }
    w.line("p.end()");
    w.dedent();
    w.line();
    text_methods(w);
    w.dedent();
    w.line();
    w.line();
    c.exports.push_back(name);
}

// -------------------------------------------------------------------------------------------------
// struct (dataclass)
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
            std::string s = "self." + ident(f.name);
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
        w_.line("@_dataclass(slots=True)");
        w_.open("class " + m_.name + ":");
        doc(w_, m_.doc);
        for (const auto& f : m_.fields) {
            if (!f->has_member()) continue;
            std::string comment;
            if (!f->unit.empty()) comment = "[" + f->unit + "]";
            if (f->derived_from) comment += (comment.empty() ? "" : " ") + std::string("set from ") + f->derived_from->name + " on encode";
            const auto type = elem_type(m_, *f) + (f->when ? " | None" : "");
            w_.line(ident(f->name) + ": " + type + " = " + (f->when ? std::string("None") : field_default(m_, *f)) +
                    (comment.empty() ? "" : "  # " + comment));
        }
        w_.line("message_name: _ClassVar[str] = " + string_literal(m_.name));
        w_.line("min_wire_size: _ClassVar[int] = " + sz(m_.min_size));
        if (m_.max_size) w_.line("max_wire_size: _ClassVar[int] = " + sz(*m_.max_size));
        if (m_.static_size) w_.line("wire_size: _ClassVar[int] = " + sz(*m_.static_size));
        for (const auto& f : m_.fields)
            if (f->const_value) w_.line(f->name + "_value: _ClassVar = " + typed_literal(*f, *f->const_value));
        emit_conversions();
        emit_computed();
        emit_read();
        emit_write();
        const auto& n = m_.name;
        w_.line();
        w_.line("@staticmethod");
        w_.open("def decode(data) -> " + n + ":");
        w_.line("\"\"\"Decode from bytes (trailing bytes are ignored). Raises gmsg.GmpError.\"\"\"");
        w_.line("return " + n + ".read(gmsg.Reader(data))");
        w_.dedent();
        w_.line();
        w_.line("@staticmethod");
        w_.open("def decode_with_extra(data) -> tuple[" + n + ", int]:");
        w_.line("\"\"\"Like decode(data): (message, extra), extra = bytes after the message (which decode ignores).\"\"\"");
        w_.line("r = gmsg.Reader(data)");
        w_.line("return " + n + ".read(r), r.remaining");
        w_.dedent();
        w_.line();
        w_.open("def encode(self) -> bytes:");
        w_.line("w = gmsg.Writer()");
        w_.line(n + ".write(w, self)");
        w_.line("return w.to_bytes()");
        w_.dedent();
        if (m_.want_view) {
            w_.line();
            w_.line("@staticmethod");
            w_.open("def view(data) -> " + view_name(m_) + ":");
            w_.line("\"\"\"A validated zero-copy view over encoded bytes.\"\"\"");
            w_.line("return " + view_name(m_) + ".from_bytes(data)");
            w_.dedent();
        }
        emit_print();
        w_.line();
        text_methods(w_);
        w_.dedent();
        w_.line();
        w_.line();
        c_.exports.push_back(m_.name);
    }

private:
    void emit_conversions() {
        for (const auto& f : m_.fields) {
            if (!f->has_member()) continue;
            const auto n = "self." + ident(f->name);
            for (const auto& cv : conversions_of(*f)) {
                const auto scale = double_literal(cv.scale), off = double_literal(cv.offset);
                const auto acc = f->name + "_" + cv.suffix;
                w_.line();
                if (!cv.unit.empty() && cv.suffix != "scaled") w_.line("# " + f->name + " in " + cv.unit);
                w_.line("@property");
                w_.open("def " + acc + "(self) -> float" + (f->when ? " | None" : "") + ":");
                if (f->when) w_.line("return None if " + n + " is None else " + to_double(*f, n) + " * " + scale + " + " + off);
                else w_.line("return " + to_double(*f, n) + " * " + scale + " + " + off);
                w_.dedent();
                if (f->when) continue;
                std::string conv;
                if (f->prim == Prim::F32) conv = "gmsg.to_f32((v - " + off + ") / " + scale + ")";
                else if (f->prim == Prim::F64) conv = "(v - " + off + ") / " + scale;
                else if (f->prim != Prim::Bool && f->prim != Prim::Char)
                    conv = "gmsg.wrap_int(gmsg.llround((v - " + off + ") / " + scale + "), " + prim_id(f->prim) + ")";
                if (conv.empty()) continue;
                w_.line();
                w_.line("@" + acc + ".setter");
                w_.open("def " + acc + "(self, v: float) -> None:");
                w_.line(n + " = " + conv);
                w_.dedent();
            }
        }
    }

    void emit_computed() {
        for (const auto& cp : m_.computed) {
            const auto n = ident(cp.name);
            w_.line();
            if (!cp.doc.empty()) doc(w_, cp.doc);
            w_.line("@property");
            w_.open("def " + n + "(self) -> " + prim_type(cp.prim) + ":");
            w_.line("return " + from_int(expr(*cp.value, this_ref_), cp.prim));
            w_.dedent();
            if (cp.setters.empty()) continue;
            w_.line();
            w_.line("@" + n + ".setter");
            w_.open("def " + n + "(self, v) -> None:");
            w_.line("value = gmsg.to_int(v)");
            for (const auto& [f, e] : cp.setters) w_.line("self." + ident(f->name) + " = " + from_int(expr(*e, this_ref_), *f));
            w_.dedent();
        }
    }

    // ---- runs of plain fields: one precompiled struct.Struct ----------------------------------------
    // Consecutive scalars / enums without checks (and padding) are read with one unpack and written with one pack.
    // Such fields can only fail with Truncated on decode (and OutOfRange on encode, checked per value as before), so
    // the errors and their order stay the same as field by field.

    /// Format character of a plain scalar ('' when the field cannot be in a run).
    static std::string struct_char(const Field& f) {
        switch (storage_prim(f)) {
            case Prim::U8: case Prim::Char: return "B";
            case Prim::I8: return "b";
            case Prim::U16: return "H";
            case Prim::I16: return "h";
            case Prim::U32: return "I";
            case Prim::I32: return "i";
            case Prim::U64: return "Q";
            case Prim::I64: return "q";
            case Prim::F32: return "f";
            case Prim::F64: return "d";
            case Prim::Bool: return "?";
        }
        return "";
    }

    static bool plain(const Field& f, bool reading) {
        if (f.when) return false;
        if (f.kind == FieldKind::Pad) return true;
        if (f.kind != FieldKind::Scalar && f.kind != FieldKind::Enum) return false;
        if (f.crc || f.const_value || strict_enum(f) || !f.limit_checks.empty() || !f.has_member()) return false;
        return reading || !f.derived_from;
    }

    /// End of the run of plain fields starting at i (i itself when there is no run of two or more values).
    std::size_t run_end(std::size_t i, bool reading) const {
        std::optional<Endian> e;
        std::size_t j = i, values = 0;
        for (; j < m_.fields.size(); ++j) {
            const auto& f = *m_.fields[j];
            if (!plain(f, reading)) break;
            if (f.kind != FieldKind::Pad) {
                if (prim_size(storage_prim(f)) > 1) {
                    if (e && *e != f.endian) break;
                    e = f.endian;
                }
                ++values;
            }
        }
        while (j > i && m_.fields[j - 1]->kind == FieldKind::Pad) --j;  // trailing padding: as before
        return values >= 2 ? j : i;
    }

    /// The module-level struct.Struct of fields [i, j).
    std::string run_struct(std::size_t i, std::size_t j) {
        std::string fmt;
        std::optional<Endian> e;
        for (auto k = i; k < j; ++k) {
            const auto& f = *m_.fields[k];
            if (f.kind == FieldKind::Pad) {
                fmt += sz(f.len.fixed) + "x";
                continue;
            }
            if (prim_size(storage_prim(f)) > 1) e = f.endian;
            fmt += struct_char(f);
        }
        return c_.consts.name("_struct.Struct('" + std::string(e == Endian::Little ? "<" : ">") + fmt + "')", "_st");
    }

    void read_run(std::size_t i, std::size_t j) {
        std::string targets;
        std::size_t n = 0;
        for (auto k = i; k < j; ++k)
            if (m_.fields[k]->kind != FieldKind::Pad) {
                targets += (n++ ? ", " : "") + std::string("m.") + ident(m_.fields[k]->name);
            }
        w_.line(targets + (n == 1 ? "," : "") + " = r.read_struct(" + run_struct(i, j) + ")");
        for (auto k = i; k < j; ++k) {
            const auto& f = *m_.fields[k];
            const auto dst = "m." + ident(f.name);
            if (f.kind == FieldKind::Enum) w_.line(dst + " = " + enum_of(f, dst));
            else if (f.kind == FieldKind::Scalar && f.prim == Prim::Char) w_.line(dst + " = chr(" + dst + ")");
        }
    }

    void write_run(std::size_t i, std::size_t j) {
        std::string args;
        for (auto k = i; k < j; ++k) {
            const auto& f = *m_.fields[k];
            if (f.kind == FieldKind::Pad) continue;
            const auto v = "m." + ident(f.name);
            const auto p = storage_prim(f);
            std::string a;
            if (p == Prim::F32) a = "gmsg.to_f32(" + v + ")";
            else if (p == Prim::F64 || p == Prim::Bool) a = v;
            else if (p == Prim::Char) a = "gmsg.char_code(" + v + ")";
            else a = "gmsg.check_int(" + v + ", " + prim_id(p) + ")";
            args += ", " + a;
        }
        w_.line("w.write_struct(" + run_struct(i, j) + args + ")");
    }

    // ---- read --------------------------------------------------------------------------------------
    void emit_read() {
        w_.line();
        w_.line("@staticmethod");
        w_.open("def read(r: gmsg.Reader) -> " + m_.name + ":");
        w_.line("m = " + m_.name + "()");
        for (std::size_t i = 0; i < m_.fields.size();) {
            if (const auto j = run_end(i, true); j > i) {
                read_run(i, j);
                i = j;
                continue;
            }
            const auto& f = m_.fields[i++];
            const auto dst = f->has_member() ? "m." + ident(f->name) : std::string();
            if (f->when) {
                w_.open("if " + condition(*f->when, ref_) + ":");
                read_value(*f, dst, 0);
                w_.dedent();
            } else {
                read_value(*f, dst, 0);
            }
        }
        w_.line("return m");
        w_.dedent();
    }

    void read_value(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: {
                const auto t = prim_id(storage_prim(f));
                const auto n = prim_size(storage_prim(f));
                const auto val = "v" + d;
                if (f.crc) {
                    w_.line("b" + d + " = r.view(" + sz(n + f.crc->size) + ")");
                    w_.line("if gmsg.load_uint(b" + d + ", " + sz(n) + ", " + sz(f.crc->size) + ", " + endian(f.crc->endian) +
                            ") != " + checksum_value(c_, *f.crc, "b" + d, "0", sz(n)) + ": " + fail("BAD_CHECKSUM"));
                    w_.line(val + " = gmsg.load(b" + d + ", 0, " + t + ", " + endian(f.endian) + ")");
                } else {
                    w_.line(val + " = r.read(" + t + ", " + endian(f.endian) + ")");
                }
                if (strict_enum(f)) w_.line("if not " + f.enum_def->name + ".is_known(" + val + "): " + fail("UNKNOWN_ENUM"));
                limit_stmt(w_, f, val);
                if (f.const_value) w_.line("if " + val + " != " + typed_literal(f, *f.const_value) + ": " + fail("BAD_CONST"));
                else if (!dst.empty()) w_.line(dst + " = " + enum_of(f, val));
                break;
            }
            case FieldKind::Bits: {
                const auto t = bits_type_name(m_, f);
                w_.line("b" + d + " = r.view(" + sz(f.bits_bytes) + ")");
                w_.line(t + ".check(b" + d + ")");
                w_.line(dst + " = " + t + ".unpack(b" + d + ")");
                break;
            }
            case FieldKind::Pad: w_.line("r.skip(" + sz(f.len.fixed) + ")"); break;
            case FieldKind::Bytes:
            case FieldKind::String: read_blob(f, dst, depth); break;
            case FieldKind::Array: read_array(f, dst, depth); break;
            case FieldKind::Message: w_.line(dst + " = " + f.message->name + ".read(r)"); break;
            case FieldKind::Custom: {
                const auto fn = ext_fn(c_, f.custom->lang, "decode");
                if (f.custom->wire_size) w_.line(dst + " = " + fn + "(gmsg.Reader(r.view(" + sz(*f.custom->wire_size) + ")))");
                else w_.line(dst + " = " + fn + "(r)");
                break;
            }
        }
    }

    void read_blob(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        const auto b = "b" + d;
        const auto max = sz(f.max.value_or(0));
        switch (f.len.kind) {
            case LenKind::Fixed: w_.line(b + " = r.take(" + sz(f.len.fixed) + ")"); break;
            case LenKind::Expr:
                w_.line("n" + d + " = " + expr(*f.len.expr, ref_));
                w_.line("if n" + d + " < 0 or n" + d + " > " + max + ": " + fail("OUT_OF_RANGE"));
                w_.line(b + " = r.take(n" + d + ")");
                break;
            case LenKind::Remaining:
                w_.line("if r.remaining > " + max + ": " + fail("OUT_OF_RANGE"));
                w_.line(b + " = r.take(r.remaining)");
                break;
            case LenKind::Terminator:
                w_.line(b + " = r.read_until(" + sz(f.len.terminator) + ")");
                w_.line("if len(" + b + ") > " + max + ": " + fail("OUT_OF_RANGE"));
                break;
            case LenKind::None: w_.line(b + " = b''"); break;
        }
        if (f.kind == FieldKind::String) {
            if (f.len.kind == LenKind::Fixed)
                w_.line(dst + " = gmsg.strip_padding(gmsg.latin1_string(" + b + "), " +
                        sz(static_cast<unsigned char>(f.pad_char)) + ")");
            else
                w_.line(dst + " = gmsg.latin1_string(" + b + ")");
        } else {
            w_.line(dst + " = " + b);
        }
    }

    void read_array(const Field& f, const std::string& dst, int depth) {
        const auto d = dv(depth);
        const auto i = "i" + d;
        const auto& e = *f.elem;
        switch (f.len.kind) {
            case LenKind::Fixed:
                w_.line(dst + " = [None] * " + sz(f.len.fixed));
                w_.open("for " + i + " in range(" + sz(f.len.fixed) + "):");
                read_value(e, dst + "[" + i + "]", depth + 1);
                w_.dedent();
                break;
            case LenKind::Expr:
                w_.line("n" + d + " = " + expr(*f.len.expr, ref_));
                w_.line("if n" + d + " < 0 or n" + d + " > " + sz(f.max.value_or(0)) + ": " + fail("OUT_OF_RANGE"));
                w_.line(dst + " = [None] * n" + d);
                w_.open("for " + i + " in range(n" + d + "):");
                read_value(e, dst + "[" + i + "]", depth + 1);
                w_.dedent();
                break;
            case LenKind::Remaining:
                w_.line(dst + " = []");
                w_.open("while r.remaining > 0:");
                w_.line("if len(" + dst + ") >= " + sz(f.max.value_or(0)) + ": " + fail("OUT_OF_RANGE"));
                w_.line(dst + ".append(None)");
                read_value(e, dst + "[-1]", depth + 1);
                w_.dedent();
                break;
            default: break;
        }
    }

    // ---- write -------------------------------------------------------------------------------------
    void emit_write() {
        ref_ = write_ref_;
        w_.line();
        w_.line("@staticmethod");
        w_.open("def write(w: gmsg.Writer, m: " + m_.name + ") -> None:");
        if (m_.fields.empty()) w_.line("pass");
        for (std::size_t i = 0; i < m_.fields.size();) {
            if (const auto j = run_end(i, false); j > i) {
                write_run(i, j);
                i = j;
                continue;
            }
            const auto& f = m_.fields[i++];
            const auto src = f->has_member() ? "m." + ident(f->name) : std::string();
            if (f->when) {
                w_.open("if " + condition(*f->when, ref_) + ":");
                w_.line("if " + src + " is None: " + fail("MISSING_FIELD"));
                write_value(*f, src, 0);
                w_.dedent();
            } else {
                write_value(*f, src, 0);
            }
        }
        w_.dedent();
        ref_ = read_ref_;
    }

    void write_value(const Field& f, const std::string& src, int depth) {
        const auto d = dv(depth);
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: {
                const auto p = storage_prim(f);
                const auto t = prim_id(p);
                std::string val = src;
                if (f.const_value) {
                    val = typed_literal(f, *f.const_value);
                } else if (f.derived_from) {
                    const auto max = prim_is_signed(p) ? (std::uint64_t{1} << (8 * prim_size(p) - 1)) - 1
                                                       : (prim_size(p) >= 8 ? ~std::uint64_t{0} : (std::uint64_t{1} << (8 * prim_size(p))) - 1);
                    w_.line("c" + d + " = " + derived_count(f));
                    w_.line("if c" + d + " > " + std::to_string(max) + ": " + fail("OUT_OF_RANGE"));
                    val = "c" + d;
                } else if (strict_enum(f)) {
                    w_.line("if not " + f.enum_def->name + ".is_known(" + val + "): " + fail("UNKNOWN_ENUM"));
                }
                limit_stmt(w_, f, val);
                w_.line("w.write(" + t + ", " + val + ", " + endian(f.endian) + ")");
                if (f.crc) {
                    const auto n = sz(prim_size(p));
                    w_.line("w.uint(" + checksum_value(c_, *f.crc, "w.buf", "w.pos - " + n, "w.pos") + ", " + sz(f.crc->size) +
                            ", " + endian(f.crc->endian) + ")");
                }
                break;
            }
            case FieldKind::Bits:
                for (const auto& b : f.bits)
                    if (!b.pad && !b.const_value && b.enum_def && b.enum_def->strict)
                        w_.line("if not " + b.enum_def->name + ".is_known(" + src + "." + ident(b.name) + "): " + fail("UNKNOWN_ENUM"));
                    else if (!b.pad && !b.const_value && !b.limit_checks.empty())
                        limit_stmt(w_, b, src + "." + ident(b.name));
                w_.line("at" + d + " = w.pos");
                w_.line("w.zeros(" + sz(f.bits_bytes) + ")");
                w_.line(bits_type_name(m_, f) + ".pack(" + src + ", w.buf, at" + d + ")");
                break;
            case FieldKind::Pad: w_.line("w.zeros(" + sz(f.len.fixed) + ")"); break;
            case FieldKind::Bytes:
            case FieldKind::String: write_blob(f, src, depth); break;
            case FieldKind::Array: write_array(f, src, depth); break;
            case FieldKind::Message: w_.line(f.message->name + ".write(w, " + src + ")"); break;
            case FieldKind::Custom: {
                const auto fn = ext_fn(c_, f.custom->lang, "encode");
                w_.line("p" + d + " = w.pos");
                w_.line(fn + "(w, " + src + ")");
                if (f.custom->wire_size)
                    w_.line("if w.pos - p" + d + " != " + sz(*f.custom->wire_size) + ": " + fail("INVALID_VALUE"));
                break;
            }
        }
    }

    void length_check(const Field& f, const std::string& size_expr) {
        if (f.len.kind == LenKind::Expr && !is_derived_length(f))
            w_.line("if " + size_expr + " != " + expr(*f.len.expr, ref_) + ": " + fail("BAD_LENGTH"));
    }

    /// What the C++ container type guarantees: fixed size, or at most `max` elements.
    void capacity_check(const Field& f, const std::string& size_expr) {
        if (f.len.kind == LenKind::Fixed) {
            if (f.kind != FieldKind::String) w_.line("if " + size_expr + " != " + sz(f.len.fixed) + ": " + fail("OUT_OF_RANGE"));
        } else {
            w_.line("if " + size_expr + " > " + sz(f.max.value_or(0)) + ": " + fail("OUT_OF_RANGE"));
        }
    }

    void write_blob(const Field& f, const std::string& src, int depth) {
        const auto d = dv(depth);
        const bool str = f.kind == FieldKind::String;
        const auto b = "b" + d;
        w_.line(b + " = " + (str ? "gmsg.latin1_bytes(" + src + ")" : "gmsg.to_bytes(" + src + ")"));
        capacity_check(f, "len(" + b + ")");
        length_check(f, "len(" + b + ")");
        if (f.len.kind == LenKind::Fixed && str) w_.line("if len(" + b + ") > " + sz(f.len.fixed) + ": " + fail("OUT_OF_RANGE"));
        w_.line("w.put(" + b + ")");
        if (f.len.kind == LenKind::Fixed && str)
            w_.line("w.fill(" + sz(static_cast<unsigned char>(f.pad_char)) + ", " + sz(f.len.fixed) + " - len(" + b + "))");
        if (f.len.kind == LenKind::Terminator) w_.line("w.write('u8', " + sz(f.len.terminator) + ")");
    }

    void write_array(const Field& f, const std::string& src, int depth) {
        const auto d = dv(depth);
        capacity_check(f, "len(" + src + ")");
        length_check(f, "len(" + src + ")");
        w_.open("for x" + d + " in " + src + ":");
        write_value(*f.elem, "x" + d, depth + 1);
        w_.dedent();
    }

    void emit_print() {
        w_.line();
        w_.open("def print_to(self, p: gmsg.Printer) -> None:");
        w_.line("p.begin(" + string_literal(m_.name) + ")");
        for (const auto& f : m_.fields) {
            if (!f->has_member()) continue;
            const auto v = "self." + ident(f->name);
            if (f->when) w_.open("if " + v + " is not None:");
            w_.line("p.key(" + string_literal(f->name) + ")");
            w_.line(print_value(c_, m_, *f, v));
            emit_print_extras(w_, *f, v);
            if (f->when) {
                w_.dedent();
                w_.open("else:");
                w_.line("p.absent(" + string_literal(f->name) + ")");
                w_.dedent();
            }
        }
        print_computed(w_, m_);
        w_.line("p.end()");
        w_.dedent();
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
            std::string s = "self." + ident(f.name);
            if (b) s += "." + ident(b->name);
            return s;
        };
        std::size_t k = 0;
        for (const auto& f : m.fields)
            if (f->when) cond_bit_[f.get()] = k++;
    }

    void emit() {
        w_.open("class " + name_ + ":");
        w_.line("# Zero-copy view over an encoded " + m_.name + "; fields are read on access.");
        w_.line("__slots__ = ('_b', '_o', '_p', '_x')");
        w_.line("message_name = " + string_literal(m_.name));
        w_.line("min_wire_size = " + sz(m_.min_size));
        w_.line();
        w_.open("def __init__(self, b: memoryview, o: list[int] | None = None, p: list[bool] | None = None, x: int = 0) -> None:");
        w_.line("self._b, self._o, self._p, self._x = b, o, p, x");
        w_.dedent();
        emit_from();
        emit_unchecked();
        w_.line();
        w_.line("@property");
        w_.open("def data(self) -> memoryview:");
        w_.line("\"\"\"The message bytes.\"\"\"");
        w_.line("return self._b");
        w_.dedent();
        w_.line();
        w_.line("@property");
        w_.open("def wire_size(self) -> int:");
        w_.line("return len(self._b)");
        w_.dedent();
        w_.line();
        w_.line("@property");
        w_.open("def extra_bytes(self) -> int:");
        w_.line("\"\"\"Bytes after the message in the data given to from_bytes() (ignored; a protocol with exact_size rejects them).\"\"\"");
        w_.line("return self._x");
        w_.dedent();
        for (const auto& f : m_.fields) emit_accessor(*f);
        emit_computed_accessors();
        emit_ranges();
        w_.line();
        w_.open("def print_to(self, p: gmsg.Printer) -> None:");
        w_.line("p.begin(" + string_literal(m_.name) + ")");
        for (const auto& f : m_.fields) emit_print_field(*f);
        print_computed(w_, m_);
        w_.line("p.end()");
        w_.dedent();
        w_.line();
        text_methods(w_);
        w_.line();
        w_.open("def __repr__(self) -> str:");
        w_.line("return '<" + name_ + " ' + gmsg.to_text(self, style='single_line') + '>'");
        w_.dedent();
        if (m_.want_struct) {
            w_.line();
            w_.open("def to_struct(self) -> " + m_.name + ":");
            w_.line("\"\"\"Decode into the message dataclass.\"\"\"");
            w_.line("return " + m_.name + ".decode(self._b)");
            w_.dedent();
        }
        w_.dedent();
        w_.line();
        w_.line();
        c_.exports.push_back(name_);
    }

private:
    std::string off(const Field& f) const {
        if (f.static_offset) return sz(*f.static_offset);
        return "self._o[" + sz(f.index) + "]";
    }
    std::string size(const Field& f) const {
        if (f.static_size) return sz(*f.static_size);
        return "(self._o[" + sz(f.index + 1) + "] - self._o[" + sz(f.index) + "])";
    }
    static std::string plus(const std::string& a, const std::string& b) { return a == "0" ? b : a + " + " + b; }

    /// Validation of a field with static size at byte offset expression `o` of `data`.
    void validate_static(const Field& f, const std::string& o) {
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: {
                const auto t = prim_id(storage_prim(f));
                const auto load = "gmsg.load(data, " + o + ", " + t + ", " + endian(f.endian) + ")";
                if (f.crc) {
                    const auto n = sz(prim_size(storage_prim(f)));
                    w_.line("if gmsg.load_uint(data, " + plus(o, n) + ", " + sz(f.crc->size) + ", " + endian(f.crc->endian) +
                            ") != " + checksum_value(c_, *f.crc, "data", o, plus(o, n)) + ": " + fail("BAD_CHECKSUM"));
                }
                if (f.const_value)
                    w_.line("if " + load + " != " + typed_literal(f, *f.const_value) + ": " + fail("BAD_CONST"));
                else if (strict_enum(f))
                    w_.line("if not " + f.enum_def->name + ".is_known(" + load + "): " + fail("UNKNOWN_ENUM"));
                if (!f.limit_checks.empty()) {
                    w_.line("lv = " + load);
                    limit_stmt(w_, f, "lv");
                }
                break;
            }
            case FieldKind::Bits:
                if (bits_need_check(f)) w_.line(bits_type_name(m_, f) + ".check(data, " + o + ")");
                break;
            case FieldKind::Message:
                if (f.message->needs_validation)
                    w_.line(view_name(*f.message) + ".from_bytes(data[" + o + ":" + plus(o, sz(*f.static_size)) + "])");
                break;
            case FieldKind::Array: {
                const auto& e = *f.elem;
                if (e.kind == FieldKind::Message && e.message->needs_validation) {
                    const auto es = sz(*e.static_size);
                    w_.open("for k in range(" + sz(f.len.fixed) + "):");
                    w_.line(view_name(*e.message) + ".from_bytes(data[" + plus(o, "k * " + es) + ":" + plus(o, "(k + 1) * " + es) + "])");
                    w_.dedent();
                } else if (e.const_value) {
                    const auto es = sz(*e.static_size);
                    w_.open("for k in range(" + sz(f.len.fixed) + "):");
                    w_.line("if gmsg.load(data, " + plus(o, "k * " + es) + ", " + prim_id(storage_prim(e)) + ", " +
                            endian(e.endian) + ") != " + typed_literal(e, *e.const_value) + ": " + fail("BAD_CONST"));
                    w_.dedent();
                } else if (strict_enum(e) || !e.limit_checks.empty()) {
                    emit_enum_array_check(e, o, sz(f.len.fixed));
                }
                break;
            }
            default: break;
        }
    }

    /// Reject undeclared values (strict enum) or values outside the limits in `count` elements from offset `o`.
    void emit_enum_array_check(const Field& e, const std::string& o, const std::string& count) {
        const auto es = sz(prim_size(storage_prim(e)));
        w_.open("for k in range(" + count + "):");
        const auto load = "gmsg.load(data, " + plus(o, "k * " + es) + ", " + prim_id(storage_prim(e)) + ", " + endian(e.endian) + ")";
        if (strict_enum(e)) {
            w_.line("if not " + e.enum_def->name + ".is_known(" + load + "): " + fail("UNKNOWN_ENUM"));
        } else {
            w_.line("lv = " + load);
            limit_stmt(w_, e, "lv");
        }
        w_.dedent();
    }

    void emit_from() {
        w_.line();
        w_.line("@staticmethod");
        w_.open("def from_bytes(data) -> " + name_ + ":");
        w_.line("\"\"\"Validate data and build a view. Trailing bytes beyond the message are ignored. Raises gmsg.GmpError.\"\"\"");
        w_.line("data = gmsg.as_view(data)");
        if (!dynamic_) {
            const auto n = sz(*m_.static_size);
            w_.line("if len(data) < " + n + ": " + fail("TRUNCATED"));
            for (const auto& f : m_.fields) validate_static(*f, sz(*f->static_offset));
            w_.line("return " + name_ + "(data[:" + n + "], None, None, len(data) - " + n + ")");
            w_.dedent();
            return;
        }
        w_.line("v = " + name_ + "(data, [0] * " + sz(m_.fields.size() + 1) + ", [False] * " + sz(cond_bit_.size()) + ")");
        w_.line("pos = 0");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            w_.line("v._o[" + sz(f.index) + "] = pos");
            if (f.when) {
                w_.open("if " + condition(*f.when, walk_ref_) + ":");
                w_.line("v._p[" + sz(cond_bit_.at(&f)) + "] = True");
            }
            walk_field(f);
            if (f.when) w_.dedent();
        }
        w_.line("v._o[" + sz(m_.fields.size()) + "] = pos");
        w_.line("v._b = data[:pos]");
        w_.line("v._x = len(data) - pos");
        w_.line("return v");
        w_.dedent();
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
            w_.line("if len(data) - pos < " + sz(*fixed) + ": " + fail("TRUNCATED"));
            if (f.kind == FieldKind::Message && f.message->needs_validation) {
                w_.line(view_name(*f.message) + ".from_bytes(data[pos:pos + " + sz(*fixed) + "])");
            } else if (f.kind == FieldKind::Array && f.elem->kind == FieldKind::Message && f.elem->message->needs_validation) {
                const auto es = sz(*f.elem->static_size);
                w_.open("for k in range(" + sz(f.len.fixed) + "):");
                w_.line(view_name(*f.elem->message) + ".from_bytes(data[pos + k * " + es + ":pos + (k + 1) * " + es + "])");
                w_.dedent();
            } else {
                validate_static(f, "pos");
            }
            w_.line("pos += " + sz(*fixed));
            return;
        }
        switch (f.kind) {
            case FieldKind::Bytes:
            case FieldKind::String:
                switch (f.len.kind) {
                    case LenKind::Expr:
                        w_.line("n = " + expr(*f.len.expr, walk_ref_));
                        w_.line("if n < 0 or n > len(data) - pos: " + fail("TRUNCATED"));
                        w_.line("pos += n");
                        break;
                    case LenKind::Remaining: w_.line("pos = len(data)"); break;
                    case LenKind::Terminator:
                        w_.line("k = bytes(data[pos:]).find(" + sz(f.len.terminator) + ")");
                        w_.line("if k < 0: " + fail("TRUNCATED"));
                        w_.line("pos += k + 1");
                        break;
                    default: break;
                }
                break;
            case FieldKind::Array: walk_array(f); break;
            case FieldKind::Message:
                w_.line("pos += " + view_name(*f.message) + ".from_bytes(data[pos:]).wire_size");
                break;
            case FieldKind::Custom:
                w_.line("cr = gmsg.Reader(data[pos:])");
                w_.line(ext_fn(c_, f.custom->lang, "decode") + "(cr)");
                w_.line("pos += cr.pos");
                break;
            default: break;
        }
    }

    void walk_array(const Field& f) {
        const auto& e = *f.elem;
        const bool elem_msg = e.kind == FieldKind::Message;
        const auto es = e.static_size;
        const bool validate = elem_msg && e.message->needs_validation;
        if (f.len.kind == LenKind::Remaining) {
            if (es && !validate) {
                w_.line("if (len(data) - pos) % " + sz(*es) + " != 0: " + fail("BAD_LENGTH"));
                if (strict_enum(e) || !e.limit_checks.empty()) emit_enum_array_check(e, "pos", "(len(data) - pos) // " + sz(*es));
                w_.line("pos = len(data)");
            } else {
                w_.open("while pos < len(data):");
                w_.line("pos += " + view_name(*e.message) + ".from_bytes(data[pos:]).wire_size");
                w_.dedent();
            }
            return;
        }
        if (f.len.kind == LenKind::Fixed) w_.line("n = " + sz(f.len.fixed));
        else w_.line("n = " + expr(*f.len.expr, walk_ref_));
        w_.line("if n < 0: " + fail("BAD_LENGTH"));
        if (es && !validate) {
            w_.line("if n * " + sz(*es) + " > len(data) - pos: " + fail("TRUNCATED"));
            if (strict_enum(e) || !e.limit_checks.empty()) emit_enum_array_check(e, "pos", "n");
            w_.line("pos += n * " + sz(*es));
        } else {
            w_.open("for _ in range(n):");
            w_.line("pos += " + view_name(*e.message) + ".from_bytes(data[pos:]).wire_size");
            w_.dedent();
        }
    }

    void emit_unchecked() {
        w_.line();
        w_.line("@staticmethod");
        w_.open("def unchecked(data) -> " + name_ + ":");
        w_.line("\"\"\"A view over data already validated by from_bytes() (no checks).\"\"\"");
        if (!dynamic_) {
            const auto n = sz(*m_.static_size);
            w_.line("data = gmsg.as_view(data)");
            w_.line("return " + name_ + "(data[:" + n + "], None, None, len(data) - " + n + ")");
        } else {
            w_.line("return " + name_ + ".from_bytes(data)");
        }
        w_.dedent();
    }

    std::string accessor_body(const Field& f) const {
        const auto o = off(f), z = size(f);
        const auto end = o == "0" ? z : o + " + " + z;
        const auto sub = "self._b[" + o + ":" + end + "]";
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum:
                return enum_of(f, "gmsg.load(self._b, " + o + ", " + prim_id(storage_prim(f)) + ", " + endian(f.endian) + ")");
            case FieldKind::Bits: return bits_type_name(m_, f) + ".unpack(self._b, " + o + ")";
            case FieldKind::Bytes: return sub;
            case FieldKind::String:
                if (f.len.kind == LenKind::Fixed)
                    return "gmsg.strip_padding(gmsg.latin1_string(self._b, " + o + ", " + end + "), " +
                           sz(static_cast<unsigned char>(f.pad_char)) + ")";
                if (f.len.kind == LenKind::Terminator) return "gmsg.latin1_string(self._b, " + o + ", " + end + " - 1)";
                return "gmsg.latin1_string(self._b, " + o + ", " + end + ")";
            case FieldKind::Message: return view_name(*f.message) + ".unchecked(" + sub + ")";
            case FieldKind::Custom: return ext_fn(c_, f.custom->lang, "decode") + "(gmsg.Reader(" + sub + "))";
            case FieldKind::Array: {
                const auto& e = *f.elem;
                if (e.kind == FieldKind::Message) {
                    if (e.static_size)
                        return "gmsg.fixed_array(" + sub + ", " + sz(*e.static_size) + ", " + view_name(*e.message) + ".unchecked)";
                    return "gmsg.seq_array(" + sub + ", " + view_name(*e.message) + ".unchecked)";
                }
                const auto es = sz(prim_size(storage_prim(e)));
                return "gmsg.fixed_array(" + sub + ", " + es + ", lambda b: " +
                       enum_of(e, "gmsg.load(b, 0, " + prim_id(storage_prim(e)) + ", " + endian(e.endian) + ")") + ")";
            }
            default: return "None";
        }
    }

    void emit_accessor(const Field& f) {
        if (!f.has_member()) return;
        const auto n = ident(f.name);
        const auto type = elem_type(m_, f);
        w_.line();
        w_.line("@property");
        w_.open("def " + n + "(self) -> " + (f.kind == FieldKind::Bytes ? std::string("memoryview") : view_type(f)) +
                (f.when ? " | None" : "") + ":");
        if (!f.unit.empty()) w_.line("\"\"\"[" + f.unit + "]\"\"\"");
        if (f.when) w_.line("if not self._p[" + sz(cond_bit_.at(&f)) + "]: return None");
        w_.line("return " + accessor_body(f));
        w_.dedent();
        (void)type;
        for (const auto& cv : conversions_of(f)) {
            const auto conv = to_double(f, "x") + " * " + double_literal(cv.scale) + " + " + double_literal(cv.offset);
            w_.line();
            w_.line("@property");
            w_.open("def " + f.name + "_" + cv.suffix + "(self) -> float" + (f.when ? " | None" : "") + ":");
            w_.line("x = self." + n);
            w_.line(f.when ? "return None if x is None else " + conv : "return " + conv);
            w_.dedent();
        }
    }

    /// Type hint of a view accessor.
    std::string view_type(const Field& f) const {
        switch (f.kind) {
            case FieldKind::Message: return view_name(*f.message);
            case FieldKind::Array:
                if (f.elem->kind == FieldKind::Message) return "list[" + view_name(*f.elem->message) + "]";
                return elem_type(m_, f);
            default: return elem_type(m_, f);
        }
    }

    void emit_computed_accessors() {
        for (const auto& cp : m_.computed) {
            w_.line();
            if (!cp.doc.empty()) doc(w_, cp.doc);
            w_.line("@property");
            w_.open("def " + ident(cp.name) + "(self) -> " + prim_type(cp.prim) + ":");
            w_.line("return " + from_int(expr(*cp.value, acc_ref_), cp.prim));
            w_.dedent();
        }
    }

    /// ranges(): gmsg.FieldRange(name, key, start, end) of every present field (padding included), relative to data.
    void emit_ranges() {
        w_.line();
        w_.open("def ranges(self) -> list[gmsg.FieldRange]:");
        w_.line("\"\"\"Byte ranges of the present fields (relative to data), in wire order; key is the attribute (None for");
        w_.line("padding and constants).\"\"\"");
        w_.line("out = []");
        for (const auto& fp : m_.fields) {
            const auto& f = *fp;
            const auto o = off(f), z = size(f);
            const auto item = "out.append(gmsg.FieldRange(" + string_literal(f.name) + ", " +
                              (f.has_member() ? string_literal(ident(f.name)) : std::string("None")) + ", " + o + ", " +
                              (o == "0" ? z : o + " + " + z) + "))";
            if (f.when) w_.line("if self._p[" + sz(cond_bit_.at(&f)) + "]: " + item);
            else w_.line(item);
        }
        w_.line("return out");
        w_.dedent();
    }

    void emit_print_field(const Field& f) {
        if (!f.has_member()) return;
        const auto acc = "self." + ident(f.name);
        if (f.kind == FieldKind::Custom) {
            w_.line("e, x = gmsg.attempt(lambda: " + acc + ")");
            if (f.when) {
                w_.line("if not e and x is None: p.absent(" + string_literal(f.name) + ")");
                w_.open("else:");
            }
            w_.line("p.key(" + string_literal(f.name) + ")");
            w_.line("if e: p.error_value(e)");
            w_.line("else: " + print_value(c_, m_, f, "x"));
            if (f.when) w_.dedent();
            return;
        }
        w_.line("x = " + acc);
        if (f.when) {
            w_.line("if x is None: p.absent(" + string_literal(f.name) + ")");
            w_.open("else:");
        }
        w_.line("p.key(" + string_literal(f.name) + ")");
        w_.line(print_value(c_, m_, f, "x"));
        emit_print_extras(w_, f, "x");
        if (f.when) w_.dedent();
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

} // namespace gmpc::py
