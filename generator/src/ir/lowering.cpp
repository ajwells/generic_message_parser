#include "ir/lowering.hpp"

#include <algorithm>
#include <functional>
#include <set>

#include "schema/expr.hpp"

namespace gmpc {

namespace {

using namespace ir;

std::optional<std::size_t> add_opt(std::optional<std::size_t> a, std::optional<std::size_t> b) {
    if (!a || !b) return std::nullopt;
    return *a + *b;
}
std::optional<std::size_t> mul_opt(std::optional<std::size_t> a, std::optional<std::size_t> b) {
    if (!a || !b) return std::nullopt;
    return *a * *b;
}

Prim smallest_unsigned(unsigned bits) {
    if (bits <= 8) return Prim::U8;
    if (bits <= 16) return Prim::U16;
    if (bits <= 32) return Prim::U32;
    return Prim::U64;
}

class Lowerer {
public:
    Lowerer(Schema& s, Diagnostics& d) : s_(s), d_(d) {}

    void run() {
        for (auto& m : s_.messages) resolve_message(*m);
        for (auto& m : s_.messages) layout_message(*m);
        if (d_.has_errors()) return;
        sort_messages();
        for (auto& p : s_.protocols) lower_protocol(*p);
    }

private:
    // ---------------------------------------------------------------------------------------------
    // type resolution
    // ---------------------------------------------------------------------------------------------
    void resolve_type(Field& f, const Message& owner, bool element) {
        const std::string& t = f.type_name;
        if (f.kind == FieldKind::Pad) return;  // set by loader for {pad: N}
        if (t == "bits") f.kind = FieldKind::Bits;
        else if (t == "bytes") f.kind = FieldKind::Bytes;
        else if (t == "string") f.kind = FieldKind::String;
        else if (t == "array") f.kind = FieldKind::Array;
        else if (t == "pad") f.kind = FieldKind::Pad;
        else if (auto p = parse_prim(t)) { f.kind = FieldKind::Scalar; f.prim = *p; }
        else if (auto e = s_.find_enum(t)) { f.kind = FieldKind::Enum; f.enum_def = e; f.prim = e->underlying; }
        else if (auto m = s_.find_message(t)) { f.kind = FieldKind::Message; f.message = m; }
        else if (auto c = s_.find_custom_type(t)) { f.kind = FieldKind::Custom; f.custom = c; }
        else {
            d_.error(f.loc, "unknown type '" + t + "'" + (f.name.empty() ? "" : " for field '" + f.name + "'"));
            return;
        }

        const bool text = owner.format == Format::Text;
        if (text) {
            switch (f.kind) {
                case FieldKind::Bits: case FieldKind::Bytes: case FieldKind::Array: case FieldKind::Message:
                    d_.error(f.loc, "field '" + f.name + "': type '" + t + "' is not supported in text messages");
                    break;
                default: break;
            }
            if (f.when) d_.error(f.loc, "field '" + f.name + "': 'when' is not supported in text messages (use optional: true)");
        } else if (f.optional) {
            d_.error(f.loc, "field '" + f.name + "': 'optional' applies to text messages only (use 'when' for binary)");
        }

        switch (f.kind) {
            case FieldKind::Bits: resolve_bits(f); break;
            case FieldKind::Array:
                if (element) { d_.error(f.loc, "nested arrays are not supported; wrap the inner array in a message"); break; }
                if (!f.elem) { d_.error(f.loc, "array field '" + f.name + "' needs 'of'"); break; }
                resolve_type(*f.elem, owner, true);
                if (f.elem->kind == FieldKind::Bits || f.elem->kind == FieldKind::Pad || f.elem->kind == FieldKind::Bytes ||
                    f.elem->kind == FieldKind::String)
                    d_.error(f.elem->loc, "array elements must be a scalar, enum, message or custom type");
                if (f.elem->kind == FieldKind::Custom && owner.want_view)
                    d_.error(f.loc, "arrays of custom types are only supported with access: struct");
                if (f.len.kind == LenKind::None || f.len.kind == LenKind::Terminator)
                    d_.error(f.loc, "array field '" + f.name + "' needs 'count' (number, expression or 'remaining')");
                break;
            case FieldKind::Bytes:
            case FieldKind::String:
                if (f.len.kind == LenKind::None && !(text && f.kind == FieldKind::String))
                    d_.error(f.loc, "field '" + f.name + "' needs 'length' (number, expression, 'remaining') or 'terminator'");
                if (text && f.len.kind != LenKind::None)
                    d_.error(f.loc, "text string fields take the whole token; remove 'length'");
                break;
            case FieldKind::Pad:
                if (f.len.kind != LenKind::Fixed) {
                    if (f.len.kind == LenKind::None) { f.len.kind = LenKind::Fixed; f.len.fixed = 1; }
                    else d_.error(f.loc, "padding size must be a fixed number");
                }
                break;
            case FieldKind::Scalar:
            case FieldKind::Enum:
                if (f.const_value && prim_is_float(f.prim)) d_.error(f.loc, "floating point fields cannot be 'const'");
                break;
            case FieldKind::Message:
                if (f.message->format != owner.format)
                    d_.error(f.loc, "nested message '" + f.message->name + "' must have the same format as '" + owner.name + "'");
                break;
            default: break;
        }
        if (f.const_value && f.kind != FieldKind::Scalar && f.kind != FieldKind::Enum)
            d_.error(f.loc, "'const' is only valid on scalar and enum fields");
        if ((f.scale || f.offset_value) && f.kind != FieldKind::Scalar)
            d_.error(f.loc, "'scale'/'offset' are only valid on numeric scalar fields");
        if (!f.conversions.empty() && (f.kind != FieldKind::Scalar || f.prim == Prim::Bool || f.prim == Prim::Char))
            d_.error(f.conversions.front().loc, "'conversions' are only valid on numeric scalar fields");
        if (f.crc) {
            if (text) d_.error(f.crc->loc, "field '" + f.name + "': 'crc' is not supported in text messages");
            else if (element) d_.error(f.crc->loc, "'crc' is not supported on array elements");
            else if (f.kind != FieldKind::Scalar && f.kind != FieldKind::Enum)
                d_.error(f.crc->loc, "field '" + f.name + "': 'crc' is only valid on scalar and enum fields");
            f.crc->endian = f.endian;  // a multi-byte CRC is stored in the field's byte order
            resolve_algo(*f.crc);
        }
    }

    void resolve_bits(Field& f) {
        unsigned off = 0;
        std::set<std::string> names;
        if (f.bits.empty()) d_.error(f.loc, "bits group '" + f.name + "' needs 'fields'");
        for (auto& b : f.bits) {
            b.offset = off;
            if (b.bits == 0 || b.bits > 64) d_.error(b.loc, "bit width must be 1..64");
            off += b.bits;
            if (b.pad) continue;
            if (!names.insert(b.name).second) d_.error(b.loc, "duplicate bit field '" + b.name + "'");
            if (b.type_name.empty()) {
                b.prim = b.bits == 1 ? Prim::Bool : smallest_unsigned(b.bits);
            } else if (auto p = parse_prim(b.type_name)) {
                if (prim_is_float(*p) || *p == Prim::Char) d_.error(b.loc, "bit fields must be integer, bool or enum");
                else if (*p != Prim::Bool && prim_size(*p) * 8 < b.bits)
                    d_.error(b.loc, "type '" + b.type_name + "' is too small for " + std::to_string(b.bits) + " bits");
                b.prim = *p;
            } else if (auto e = s_.find_enum(b.type_name)) {
                b.enum_def = e;
                b.prim = e->underlying;
            } else {
                d_.error(b.loc, "unknown bit field type '" + b.type_name + "'");
            }
        }
        if (off % 8 != 0)
            d_.error(f.loc, "bits group '" + f.name + "' spans " + std::to_string(off) +
                                " bits; it must be a whole number of bytes (add { pad: " + std::to_string(8 - off % 8) + " })");
        f.bits_bytes = off / 8;
    }

    // ---------------------------------------------------------------------------------------------
    // expressions
    // ---------------------------------------------------------------------------------------------
    void resolve_expr(Expr& e, const Message& m, std::size_t limit) {
        switch (e.kind) {
            case Expr::Kind::Int: return;
            case Expr::Kind::Unary: resolve_expr(*e.lhs, m, limit); return;
            case Expr::Kind::Binary:
                resolve_expr(*e.lhs, m, limit);
                resolve_expr(*e.rhs, m, limit);
                return;
            case Expr::Kind::EnumConst: return;
            case Expr::Kind::Input: return;
            case Expr::Kind::Ref:
                if (allow_input_ && e.path.size() == 1 && e.path[0] == "value") {
                    e.kind = Expr::Kind::Input;
                    return;
                }
                break;
        }
        const Field* target = nullptr;
        for (std::size_t i = 0; i < limit && i < m.fields.size(); ++i)
            if (m.fields[i]->name == e.path[0]) target = m.fields[i].get();
        if (!target) {
            if (e.path.size() == 2) {
                if (auto en = s_.find_enum(e.path[0])) {
                    if (!en->find(e.path[1])) {
                        d_.error(e.loc, "enum '" + en->name + "' has no value '" + e.path[1] + "'");
                        return;
                    }
                    e.kind = Expr::Kind::EnumConst;
                    e.enum_def = en;
                    return;
                }
            }
            d_.error(e.loc, "'" + expr_to_string(e) + "' does not name an earlier field of '" + m.name + "' or an enum constant");
            return;
        }
        if (target->when) {
            d_.error(e.loc, "expressions cannot reference conditional field '" + target->name + "'");
            return;
        }
        e.field = target;
        if (target->kind == FieldKind::Bits) {
            if (e.path.size() != 2) {
                d_.error(e.loc, "reference a member of bits group '" + target->name + "' (e.g. " + target->name + ".x)");
                return;
            }
            for (const auto& b : target->bits)
                if (!b.pad && b.name == e.path[1]) e.bitfield = &b;
            if (!e.bitfield) d_.error(e.loc, "bits group '" + target->name + "' has no member '" + e.path[1] + "'");
            return;
        }
        if (e.path.size() != 1) {
            d_.error(e.loc, "'" + expr_to_string(e) + "': only bits groups have members");
            return;
        }
        if (target->kind != FieldKind::Scalar && target->kind != FieldKind::Enum) {
            d_.error(e.loc, "field '" + target->name + "' cannot be used in an expression (must be scalar or enum)");
            return;
        }
        if (target->kind == FieldKind::Scalar && prim_is_float(target->prim))
            d_.error(e.loc, "floating point field '" + target->name + "' cannot be used in an expression");
    }

    void resolve_message(Message& m) {
        std::set<std::string> names;
        for (std::size_t i = 0; i < m.fields.size(); ++i) {
            auto& f = *m.fields[i];
            f.index = i;
            if (!f.name.empty() && !names.insert(f.name).second)
                d_.error(f.loc, "duplicate field '" + f.name + "' in '" + m.name + "'");
            resolve_type(f, m, false);
            if (f.when) resolve_expr(*f.when, m, i);
            if (f.len.kind == LenKind::Expr && f.len.expr) {
                resolve_expr(*f.len.expr, m, i);
                // A bare reference to an integer field makes that field derived on encode.
                auto& ex = *f.len.expr;
                if (ex.kind == Expr::Kind::Ref && ex.field && !ex.bitfield && ex.field->kind == FieldKind::Scalar &&
                    prim_is_integer(ex.field->prim) && !ex.field->const_value && !ex.field->derived_from) {
                    const_cast<Field*>(ex.field)->derived_from = &f;
                }
            }
        }
        resolve_computed(m);
    }

    // ---------------------------------------------------------------------------------------------
    // computed values
    // ---------------------------------------------------------------------------------------------
    /// Fields a computed setter may assign: plain scalar / enum members.
    static bool settable(const Field& f) {
        return (f.kind == FieldKind::Scalar || f.kind == FieldKind::Enum) && !f.const_value && !f.when && !f.derived_from &&
               f.prim != Prim::Bool && f.prim != Prim::Char;
    }

    void resolve_computed(Message& m) {
        for (auto& c : m.computed) {
            if (m.format == Format::Text) {
                d_.error(c.loc, "computed values are only supported in binary messages");
                continue;
            }
            if (auto p = parse_prim(c.type_name); p && *p != Prim::Bool && *p != Prim::Char) c.prim = *p;
            else if (!c.type_name.empty())
                d_.error(c.loc, "computed value '" + c.name + "': type must be numeric (u8..u64, i8..i64, f32, f64)");
            if (c.value) resolve_expr(*c.value, m, m.fields.size());
            c.setters.clear();
            if (!c.set.empty()) {
                std::set<const Field*> seen;
                for (auto& [name, e] : c.set) {
                    const Field* target = nullptr;
                    for (const auto& f : m.fields)
                        if (f->name == name) target = f.get();
                    if (!target) {
                        d_.error(c.loc, "computed value '" + c.name + "': set target '" + name + "' is not a field of '" + m.name + "'");
                        continue;
                    }
                    if (!settable(*target))
                        d_.error(c.loc, "computed value '" + c.name + "': set target '" + name +
                                            "' must be a scalar or enum field that is not const, conditional or a derived length");
                    if (!seen.insert(target).second)
                        d_.error(c.loc, "computed value '" + c.name + "': field '" + name + "' is set twice");
                    if (e) {
                        allow_input_ = true;
                        resolve_expr(*e, m, m.fields.size());
                        allow_input_ = false;
                    }
                    c.setters.emplace_back(target, e);
                }
            } else if (c.value) {
                derive_setter(c);
            }
        }
    }

    static ExprPtr make_int(std::int64_t v) {
        auto e = std::make_shared<Expr>();
        e->kind = Expr::Kind::Int;
        e->value = v;
        return e;
    }
    static ExprPtr make_bin(const char* op, ExprPtr l, ExprPtr r) {
        auto e = std::make_shared<Expr>();
        e->kind = Expr::Kind::Binary;
        e->op = op;
        e->lhs = std::move(l);
        e->rhs = std::move(r);
        return e;
    }

    /// `value` built by packing whole unsigned fields at fixed shifts, e.g. `(hi << 16) | lo`, can be split back:
    /// each field = (value >> shift) & mask. Terms may be joined with `|` or `+` and must not overlap.
    void derive_setter(Computed& c) {
        struct Term { const Field* f; std::int64_t shift; };
        std::vector<Term> terms;
        auto term = [&](const Expr& ref, std::int64_t shift) {
            if (ref.kind != Expr::Kind::Ref || !ref.field || ref.bitfield) return false;
            const Field& f = *ref.field;
            if (!settable(f) || f.kind != FieldKind::Scalar || !prim_is_integer(f.prim) || prim_is_signed(f.prim)) return false;
            if (shift < 0 || shift > 63) return false;
            terms.push_back({&f, shift});
            return true;
        };
        auto collect = [&](auto&& self, const Expr& e) -> bool {
            if (e.kind == Expr::Kind::Binary && (e.op == "|" || e.op == "+")) return self(self, *e.lhs) && self(self, *e.rhs);
            if (e.kind == Expr::Kind::Binary && e.op == "<<" && e.rhs->kind == Expr::Kind::Int) return term(*e.lhs, e.rhs->value);
            return term(e, 0);
        };
        if (!collect(collect, *c.value)) return;  // not a packing expression: no setter unless `set:` is given
        std::uint64_t used = 0;
        std::set<const Field*> fields;
        for (const auto& t : terms) {
            const auto bits = static_cast<std::int64_t>(prim_size(t.f->prim) * 8);
            const auto width = std::min<std::int64_t>(bits, 64 - t.shift);
            const std::uint64_t mask = width >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1) << t.shift;
            if ((used & mask) || !fields.insert(t.f).second) return;  // overlapping terms cannot be split
            used |= mask;
        }
        for (const auto& t : terms) {
            auto input = std::make_shared<Expr>();
            input->kind = Expr::Kind::Input;
            input->path = {"value"};
            ExprPtr e = t.shift ? make_bin(">>", input, make_int(t.shift)) : input;
            const auto bits = prim_size(t.f->prim) * 8;
            if (bits < 64) e = make_bin("&", e, make_int(static_cast<std::int64_t>((std::uint64_t{1} << bits) - 1)));
            c.setters.emplace_back(t.f, e);
        }
        c.setter_derived = true;
    }

    // ---------------------------------------------------------------------------------------------
    // layout
    // ---------------------------------------------------------------------------------------------
    void field_sizes(Field& f, const Message& m) {
        f.static_size.reset();
        f.max_size.reset();
        f.min_size = 0;
        switch (f.kind) {
            case FieldKind::Scalar:
            case FieldKind::Enum: f.static_size = prim_size(f.prim) + (f.crc ? f.crc->size : 0); break;
            case FieldKind::Bits: f.static_size = f.bits_bytes; break;
            case FieldKind::Pad: f.static_size = f.len.fixed; break;
            case FieldKind::Bytes:
            case FieldKind::String:
                switch (f.len.kind) {
                    case LenKind::Fixed: f.static_size = f.len.fixed; break;
                    case LenKind::Terminator:
                        f.min_size = 1;
                        if (f.max) f.max_size = *f.max + 1;
                        break;
                    default:
                        if (f.max) f.max_size = f.max;
                        break;
                }
                break;
            case FieldKind::Array: {
                if (!f.elem) break;
                field_sizes(*f.elem, m);
                const auto es = f.elem->static_size;
                const auto emax = f.elem->static_size ? f.elem->static_size : f.elem->max_size;
                if (f.len.kind == LenKind::Fixed) {
                    f.static_size = mul_opt(es, f.len.fixed);
                    f.max_size = mul_opt(emax, f.len.fixed);
                    f.min_size = f.len.fixed * f.elem->min_size;
                } else {
                    f.max_size = f.max ? mul_opt(emax, f.max) : std::nullopt;
                }
                break;
            }
            case FieldKind::Message: {
                auto* child = const_cast<Message*>(f.message);
                layout_message(*child);
                f.static_size = child->static_size;
                f.max_size = child->max_size;
                f.min_size = child->min_size;
                break;
            }
            case FieldKind::Custom:
                f.static_size = f.custom->wire_size;
                f.max_size = f.custom->max_wire_size;
                break;
        }
        if (f.static_size) {
            f.max_size = f.static_size;
            f.min_size = *f.static_size;
        }
        if (f.when) {
            f.max_size = f.static_size ? f.static_size : f.max_size;
            f.static_size.reset();
            f.min_size = 0;
        }
    }

    bool field_needs_validation(const Field& f) {
        if (f.crc) return true;
        if (f.const_value) return true;
        if (f.kind == FieldKind::Enum && f.enum_def && f.enum_def->strict) return true;
        if (f.kind == FieldKind::Bits)
            for (const auto& b : f.bits)
                if (b.const_value || (b.enum_def && b.enum_def->strict)) return true;
        if (f.kind == FieldKind::Message && f.message->needs_validation) return true;
        if (f.kind == FieldKind::Array && f.elem) {
            if (f.elem->kind == FieldKind::Message && f.elem->message->needs_validation) return true;
            if (f.elem->const_value) return true;
            if (f.elem->kind == FieldKind::Enum && f.elem->enum_def && f.elem->enum_def->strict) return true;
        }
        return !f.static_size.has_value();
    }

    void layout_message(Message& m) {
        if (m.layout_done) return;
        if (m.layout_in_progress) {
            d_.error(m.loc, "message '" + m.name + "' contains itself (recursive nesting is not supported)");
            return;
        }
        m.layout_in_progress = true;

        if (m.format == Format::Text) {
            std::size_t tok = 0;
            for (auto& fp : m.fields) {
                auto& f = *fp;
                f.token_index = tok;
                f.token_count = f.kind == FieldKind::Pad ? f.len.fixed : 1;
                tok += f.token_count;
                if (f.kind == FieldKind::String && m.want_struct && !f.max)
                    d_.error(f.loc, "text string field '" + f.name + "' needs 'max' (struct capacity)");
            }
            m.token_count = tok;
            m.fixed_layout = false;
            m.needs_validation = false;
        } else {
            std::size_t off = 0;
            bool static_off = true;
            std::optional<std::size_t> max = 0;
            std::size_t min = 0;
            m.fixed_layout = true;
            m.needs_validation = false;
            m.conditional_count = 0;
            for (auto& fp : m.fields) {
                auto& f = *fp;
                field_sizes(f, m);
                if (f.when) ++m.conditional_count;
                f.static_offset = static_off ? std::optional<std::size_t>(off) : std::nullopt;
                if (f.static_size && static_off) off += *f.static_size;
                else static_off = false;
                if (!f.static_size) m.fixed_layout = false;
                if (field_needs_validation(f)) m.needs_validation = true;
                min += f.min_size;
                max = add_opt(max, f.max_size);

                const bool variable = f.len.kind == LenKind::Expr || f.len.kind == LenKind::Remaining ||
                                      f.len.kind == LenKind::Terminator;
                if (m.want_struct && variable && (f.kind == FieldKind::Bytes || f.kind == FieldKind::String ||
                                                  f.kind == FieldKind::Array) && !f.max)
                    d_.error(f.loc, "variable length field '" + f.name +
                                        "' needs 'max' (fixed capacity for the generated struct); or use access: view");
                if (f.len.kind == LenKind::Remaining && &fp != &m.fields.back())
                    d_.error(f.loc, "'remaining' is only allowed on the last field of a message");
            }
            if (m.conditional_count > 64) d_.error(m.loc, "at most 64 conditional fields per message are supported");
            m.static_size = m.fixed_layout ? std::optional<std::size_t>(off) : std::nullopt;
            m.min_size = min;
            m.max_size = max;
        }
        m.layout_in_progress = false;
        m.layout_done = true;
    }

    /// Order messages so nested messages are defined before their users.
    void sort_messages() {
        std::vector<MessagePtr> out;
        std::set<const Message*> done;
        std::function<void(const MessagePtr&)> visit = [&](const MessagePtr& m) {
            if (done.contains(m.get())) return;
            done.insert(m.get());
            for (const auto& f : m->fields) {
                const Message* dep = f->kind == FieldKind::Message ? f->message
                                     : (f->kind == FieldKind::Array && f->elem && f->elem->kind == FieldKind::Message)
                                         ? f->elem->message
                                         : nullptr;
                if (!dep) continue;
                for (const auto& cand : s_.messages)
                    if (cand.get() == dep) visit(cand);
            }
            out.push_back(m);
        };
        for (const auto& m : s_.messages) visit(m);
        s_.messages = std::move(out);
    }

    // ---------------------------------------------------------------------------------------------
    // protocols
    // ---------------------------------------------------------------------------------------------
    void lower_protocol(Protocol& p) {
        auto& hdr = *p.header;
        resolve_message(hdr);
        layout_message(hdr);
        for (const auto& f : hdr.fields) {
            if (f->when) d_.error(f->loc, "protocol header fields cannot be conditional");
            if (f->kind == FieldKind::Bytes || f->kind == FieldKind::String || f->kind == FieldKind::Array ||
                f->kind == FieldKind::Custom || f->kind == FieldKind::Message)
                if (!f->static_size) d_.error(f->loc, "protocol header fields must have a fixed size");
        }
        if (p.format == Format::Text && !hdr.fields.empty())
            d_.error(p.loc, "text protocols do not support a binary 'header'");

        // messages
        for (const auto& name : p.message_names) {
            const Message* m = s_.find_message(name);
            if (!m) { d_.error(p.loc, "protocol '" + p.name + "' references unknown message '" + name + "'"); continue; }
            if (m->format != p.format)
                d_.error(m->loc, "message '" + m->name + "' format does not match protocol '" + p.name + "'");
            p.messages.push_back(m);
        }
        if (p.messages.empty()) d_.error(p.loc, "protocol '" + p.name + "' has no messages");

        lower_message_type(p);
        lower_checksum(p);
        lower_framing(p);
    }

    void lower_message_type(Protocol& p) {
        auto& d = p.message_type;
        p.type_ids.clear();
        switch (d.kind) {
            case MessageTypeSpec::Kind::None:
                if (p.messages.size() > 1)
                    d_.error(p.loc, "protocol '" + p.name + "' has several messages and needs 'message_type'");
                p.type_ids.assign(p.messages.size(), std::monostate{});
                return;
            case MessageTypeSpec::Kind::Token: {
                if (p.format != Format::Text) { d_.error(d.loc, "message_type by 'token' requires a text protocol"); return; }
                if (d.token != 0) d_.error(d.loc, "only token 0 (the first token) is supported for message_type");
                std::set<std::string> seen;
                for (const auto* m : p.messages) {
                    std::string id;
                    if (auto* s = std::get_if<std::string>(&m->type_id)) id = *s;
                    else if (auto* i = std::get_if<std::int64_t>(&m->type_id)) id = std::to_string(*i);
                    else d_.error(m->loc, "message '" + m->name + "' needs a 'type_id' to be identified in '" + p.name + "'");
                    if (!id.empty() && !seen.insert(id).second) d_.error(m->loc, "duplicate type_id '" + id + "'");
                    p.type_ids.emplace_back(id);
                }
                return;
            }
            case MessageTypeSpec::Kind::Field: {
                if (p.format != Format::Binary) { d_.error(d.loc, "text protocols identify messages by 'token'"); return; }
                for (const auto& f : p.header->fields)
                    if (f->name == d.field_name) d.field = f.get();
                if (!d.field) {
                    d_.error(d.loc, "message_type field '" + d.field_name + "' is not a header field of '" + p.name + "'");
                    return;
                }
                if (d.field->kind != FieldKind::Enum && !(d.field->kind == FieldKind::Scalar && prim_is_integer(d.field->prim))) {
                    d_.error(d.loc, "message_type field must be an integer or enum");
                    return;
                }
                std::set<std::int64_t> seen;
                for (const auto* m : p.messages) {
                    std::int64_t id = 0;
                    if (auto* i = std::get_if<std::int64_t>(&m->type_id)) {
                        id = *i;
                    } else if (auto* s = std::get_if<std::string>(&m->type_id)) {
                        const auto v = d.field->enum_def ? d.field->enum_def->find(*s) : std::nullopt;
                        if (!v) d_.error(m->loc, "type_id '" + *s + "' is not an integer" +
                                                     (d.field->enum_def ? " or a value of enum '" + d.field->enum_def->name + "'" : std::string()));
                        else id = *v;
                    } else {
                        d_.error(m->loc, "message '" + m->name + "' needs a 'type_id' to be identified in '" + p.name + "'");
                    }
                    if (!seen.insert(id).second) d_.error(m->loc, "duplicate type_id " + std::to_string(id) + " in '" + p.name + "'");
                    p.type_ids.emplace_back(id);
                }
                return;
            }
        }
    }

    void lower_checksum(Protocol& p) {
        auto& c = p.checksum;
        if (!c.present) return;
        resolve_algo(c);
        if (p.format == Format::Binary && p.header->fields.empty() && c.covers == ChecksumSpec::Covers::HeaderBody)
            c.covers = ChecksumSpec::Covers::Body;
    }

    /// Resolve a checksum algorithm (built-in, custom or inline CRC) and its size in bytes.
    void resolve_algo(ChecksumSpec& c) {
        if (c.algo == "crc" && c.crc) {
            if (c.crc->width == 0 || c.crc->width > 64) d_.error(c.loc, "crc width must be 1..64");
            c.size = (c.crc->width + 7) / 8;
            if (c.size == 3 || (c.size > 4 && c.size < 8)) {
                // supported: stored in next wider integer, written with `size` bytes
            }
        } else if (auto sz = builtin_checksum_size(c.algo)) {
            c.size = *sz;
        } else if (auto cc = s_.find_custom_checksum(c.algo)) {
            c.custom = cc;
            c.size = cc->size;
        } else {
            std::string names;
            for (const auto& n : builtin_checksum_names()) names += (names.empty() ? "" : ", ") + n;
            d_.error(c.loc, "unknown checksum algo '" + c.algo + "' (built-in: " + names + ", or a custom.checksums entry)");
        }
        if (c.size == 0 || c.size > 8) d_.error(c.loc, "checksum size must be 1..8 bytes");
    }

    void lower_framing(Protocol& p) {
        auto& fr = p.framing;
        using T = FramingSpec::Type;
        p.prefix_size = fr.sync.size();
        p.suffix_size = fr.end.size();
        p.header_size = p.header->static_size.value_or(0);
        p.trailer_size = (p.format == Format::Binary && p.checksum.present) ? p.checksum.size : 0;
        if (fr.sync.size() > 8 || fr.end.size() > 8) d_.error(fr.loc, "sync/start/end patterns are limited to 8 bytes");

        std::optional<std::size_t> body_max = 0;
        for (const auto* m : p.messages) {
            const auto mx = m->static_size ? m->static_size : m->max_size;
            if (!mx) { body_max.reset(); break; }
            body_max = std::max(*body_max, *mx);
        }
        const std::size_t fixed_parts = p.prefix_size + p.header_size + p.trailer_size + p.suffix_size;

        if (fr.escape.enabled && fr.type != T::Delimited)
            d_.error(fr.loc, "'escape' is only supported with delimited framing");
        if (p.format == Format::Text && fr.type != T::Delimited && fr.type != T::None && fr.type != T::Custom)
            d_.error(fr.loc, "text protocols need delimited (or none/custom) framing");

        switch (fr.type) {
            case T::None:
                if (!fr.sync.empty() || !fr.end.empty())
                    d_.error(fr.loc, "framing 'none' takes no sync/start/end; use 'delimited' or 'fixed'");
                break;
            case T::Fixed:
                if (!fr.size) {
                    std::optional<std::size_t> sz;
                    for (const auto* m : p.messages) {
                        if (!m->static_size) { sz.reset(); break; }
                        if (sz && *sz != *m->static_size) { sz.reset(); break; }
                        sz = m->static_size;
                    }
                    if (!sz) d_.error(fr.loc, "fixed framing needs 'size' unless all messages have the same fixed size");
                    else fr.size = fixed_parts + *sz;
                }
                if (fr.size && *fr.size < fixed_parts) d_.error(fr.loc, "fixed frame size is smaller than sync + header + checksum");
                if (!fr.max_frame && fr.size) fr.max_frame = fr.size;
                break;
            case T::LengthPrefixed: {
                if (fr.length_field.empty()) { d_.error(fr.loc, "length_prefixed framing needs length.field"); break; }
                for (const auto& f : p.header->fields)
                    if (f->name == fr.length_field) fr.length = f.get();
                if (!fr.length) { d_.error(fr.loc, "length field '" + fr.length_field + "' is not a header field"); break; }
                if (fr.length->kind != FieldKind::Scalar || !prim_is_integer(fr.length->prim) || prim_is_signed(fr.length->prim) ||
                    fr.length->const_value)
                    d_.error(fr.length->loc, "length field must be an unsigned integer");
                p.length_field_offset = p.prefix_size + fr.length->static_offset.value_or(0);
                break;
            }
            case T::Delimited:
                if (fr.end.empty()) d_.error(fr.loc, "delimited framing needs 'end'");
                if (fr.escape.enabled && fr.escape.map.empty() && fr.sync.empty() && fr.end.empty())
                    d_.error(fr.loc, "escape needs delimiters");
                break;
            case T::Custom:
                fr.custom = s_.find_custom_framer(fr.custom_name);
                if (!fr.custom) d_.error(fr.loc, "custom framing needs 'framer' naming an entry of custom.framers");
                break;
        }

        if (fr.max_frame) {
            p.max_frame = *fr.max_frame;
        } else if (fr.type != T::None) {
            if (body_max && p.format == Format::Binary) {
                std::size_t interior = p.header_size + *body_max + p.trailer_size;
                if (fr.escape.enabled) interior *= 2;
                p.max_frame = p.prefix_size + interior + p.suffix_size;
            } else {
                d_.error(fr.loc, "protocol '" + p.name + "' needs framing.max_frame (message sizes are not bounded)");
            }
        }
        if (p.max_frame && p.max_frame < fixed_parts) d_.error(fr.loc, "max_frame is smaller than the fixed frame overhead");
    }

    Schema& s_;
    Diagnostics& d_;
    bool allow_input_ = false;  // resolving a computed setter: `value` names the input
};

} // namespace

bool lower_schema(ir::Schema& schema, Diagnostics& diag) {
    validate_declarations(schema, diag);
    if (diag.has_errors()) return false;
    Lowerer l(schema, diag);
    l.run();
    if (diag.has_errors()) return false;
    validate_lowered(schema, diag);
    return !diag.has_errors();
}

} // namespace gmpc
