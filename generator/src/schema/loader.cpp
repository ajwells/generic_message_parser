#include "schema/loader.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <set>

#include "schema/expr.hpp"

namespace gmpc {

namespace {

using ir::Endian;
using ir::Format;

class Loader {
public:
    Loader(std::string file, Diagnostics& diag) : file_(std::move(file)), diag_(diag) {}

    std::unique_ptr<ir::Schema> load(const YAML::Node& root) {
        auto s = std::make_unique<ir::Schema>();
        s->file = file_;
        s->stem = std::filesystem::path(file_).stem().string();
        if (!root.IsMap()) {
            diag_.error(loc(root), "schema root must be a mapping");
            return nullptr;
        }
        check_keys(root, {"schema_version", "namespace", "defaults", "enums", "custom", "messages", "protocols"},
                   "schema");
        if (auto v = root["schema_version"]) {
            s->version = static_cast<int>(to_int(v));
            if (s->version != 1) diag_.error(loc(v), "unsupported schema_version " + std::to_string(s->version));
        }
        if (auto n = root["namespace"]) s->ns = to_str(n);
        if (auto d = root["defaults"]) load_defaults(d, *s);
        if (auto e = root["enums"]) load_enums(e, *s);
        if (auto c = root["custom"]) load_custom(c, *s);
        if (auto m = root["messages"]) load_messages(m, *s);
        if (auto p = root["protocols"]) load_protocols(p, *s);
        return s;
    }

private:
    // ---------------------------------------------------------------------------------------------
    // helpers
    // ---------------------------------------------------------------------------------------------
    SourceLoc loc(const YAML::Node& n) const {
        const auto m = n.Mark();
        if (m.is_null()) return {file_, 0, 0};
        return {file_, m.line + 1, m.column + 1};
    }

    void check_keys(const YAML::Node& n, std::initializer_list<const char*> allowed, const std::string& ctx) {
        if (!n.IsMap()) return;
        for (const auto& kv : n) {
            const auto key = kv.first.as<std::string>();
            if (std::none_of(allowed.begin(), allowed.end(), [&](const char* a) { return key == a; })) {
                std::string list;
                for (const char* a : allowed) list += std::string(list.empty() ? "" : ", ") + a;
                std::string msg = "unknown key '" + key + "' in " + ctx;
                // keys renamed in earlier versions: point straight at the replacement
                const auto renamed = [&](const char* old_key, const char* new_key) {
                    if (key == old_key && std::any_of(allowed.begin(), allowed.end(), [&](const char* a) { return std::string_view(a) == new_key; }))
                        msg += std::string(" ('") + old_key + "' was renamed to '" + new_key + "')";
                };
                renamed("dispatch", "message_type");
                renamed("id", "type_id");
                diag_.error(loc(kv.first), msg + " (expected one of: " + list + ")");
            }
        }
    }

    std::string to_str(const YAML::Node& n) {
        if (!n.IsScalar()) {
            diag_.error(loc(n), "expected a scalar value");
            return {};
        }
        return n.Scalar();
    }

    std::int64_t to_int(const YAML::Node& n) {
        const std::string s = to_str(n);
        if (s.empty()) return 0;
        try {
            std::size_t used = 0;
            std::int64_t v;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
                v = static_cast<std::int64_t>(std::stoull(s.substr(2), &used, 2));
                used += 2;
            } else if (s.size() > 3 && s[0] == '-' && s[1] == '0' && (s[2] == 'x' || s[2] == 'X')) {
                v = -static_cast<std::int64_t>(std::stoull(s.substr(1), &used, 16));
                used += 1;
            } else if (s[0] == '-') {
                v = std::stoll(s, &used, 0);
            } else {
                v = static_cast<std::int64_t>(std::stoull(s, &used, 0));
            }
            if (used != s.size()) throw std::invalid_argument(s);
            return v;
        } catch (...) {
            diag_.error(loc(n), "expected an integer, got '" + s + "'");
            return 0;
        }
    }

    std::size_t to_size(const YAML::Node& n) {
        const auto v = to_int(n);
        if (v < 0) {
            diag_.error(loc(n), "expected a non-negative integer");
            return 0;
        }
        return static_cast<std::size_t>(v);
    }

    double to_double(const YAML::Node& n) {
        try {
            return n.as<double>();
        } catch (...) {
            diag_.error(loc(n), "expected a number");
            return 0;
        }
    }

    bool to_bool(const YAML::Node& n) {
        try {
            return n.as<bool>();
        } catch (...) {
            diag_.error(loc(n), "expected true/false");
            return false;
        }
    }

    char to_char(const YAML::Node& n) {
        const auto s = to_str(n);
        if (s.size() != 1) {
            diag_.error(loc(n), "expected a single character");
            return ',';
        }
        return s[0];
    }

    Endian to_endian(const YAML::Node& n) {
        const auto s = to_str(n);
        if (s == "big" || s == "be") return Endian::Big;
        if (s == "little" || s == "le") return Endian::Little;
        diag_.error(loc(n), "endian must be 'big' or 'little'");
        return Endian::Big;
    }

    ir::BitOrder to_bit_order(const YAML::Node& n) {
        const auto s = to_str(n);
        if (s == "msb_first" || s == "msb") return ir::BitOrder::MsbFirst;
        if (s == "lsb_first" || s == "lsb") return ir::BitOrder::LsbFirst;
        diag_.error(loc(n), "bit_order must be 'msb_first' or 'lsb_first'");
        return ir::BitOrder::MsbFirst;
    }

    Format to_format(const YAML::Node& n) {
        const auto s = to_str(n);
        if (s == "binary") return Format::Binary;
        if (s == "text") return Format::Text;
        diag_.error(loc(n), "format must be 'binary' or 'text'");
        return Format::Binary;
    }

    void to_access(const YAML::Node& n, bool& view, bool& strct) {
        view = strct = false;
        auto apply = [&](const YAML::Node& x) {
            const auto s = to_str(x);
            if (s == "view") view = true;
            else if (s == "struct") strct = true;
            else if (s == "both") view = strct = true;
            else diag_.error(loc(x), "access must be 'view', 'struct' or 'both'");
        };
        if (n.IsSequence()) for (const auto& x : n) apply(x);
        else apply(n);
        if (!view && !strct) view = strct = true;
    }

    /// Byte pattern: "text", [0xAA, 0x55] or a single integer (big endian, minimal width).
    std::vector<std::uint8_t> to_bytes(const YAML::Node& n) {
        std::vector<std::uint8_t> out;
        if (n.IsSequence()) {
            for (const auto& x : n) {
                const auto v = to_int(x);
                if (v < 0 || v > 255) diag_.error(loc(x), "byte value out of range");
                out.push_back(static_cast<std::uint8_t>(v));
            }
            return out;
        }
        const auto s = to_str(n);
        // Plain (unquoted) numeric scalars are integers; quoted scalars ("$", "\r\n") are characters.
        const bool numeric = !s.empty() && std::isdigit(static_cast<unsigned char>(s[0])) && n.Tag() != "!";
        if (numeric) {
            auto v = static_cast<std::uint64_t>(to_int(n));
            do {
                out.insert(out.begin(), static_cast<std::uint8_t>(v & 0xFF));
                v >>= 8;
            } while (v);
            return out;
        }
        out.assign(s.begin(), s.end());
        return out;
    }

    ir::LangBlocks to_lang(const YAML::Node& n, const std::set<std::string>& non_lang_keys) {
        ir::LangBlocks out;
        for (const auto& kv : n) {
            const auto key = kv.first.as<std::string>();
            if (non_lang_keys.contains(key)) continue;
            if (!kv.second.IsMap()) {
                diag_.error(loc(kv.first), "'" + key + "' must be a language block mapping (e.g. cpp: {...})");
                continue;
            }
            for (const auto& e : kv.second) out[key][e.first.as<std::string>()] = to_str(e.second);
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------------
    // sections
    // ---------------------------------------------------------------------------------------------
    void load_defaults(const YAML::Node& d, ir::Schema& s) {
        check_keys(d, {"endian", "bit_order", "format", "separator", "access", "strict_enums"}, "defaults");
        if (auto v = d["endian"]) s.default_endian = to_endian(v);
        if (auto v = d["bit_order"]) s.default_bit_order = to_bit_order(v);
        if (auto v = d["format"]) s.default_format = to_format(v);
        if (auto v = d["separator"]) s.default_separator = to_char(v);
        if (auto v = d["access"]) to_access(v, s.default_view, s.default_struct);
        if (auto v = d["strict_enums"]) s.default_strict_enums = to_bool(v);
    }

    void load_enums(const YAML::Node& n, ir::Schema& s) {
        if (!n.IsMap()) { diag_.error(loc(n), "'enums' must be a mapping of name -> definition"); return; }
        for (const auto& kv : n) {
            auto e = std::make_shared<ir::EnumDef>();
            e->name = kv.first.as<std::string>();
            e->loc = loc(kv.first);
            const auto& body = kv.second;
            check_keys(body, {"type", "values", "strict", "doc"}, "enum '" + e->name + "'");
            e->strict = s.default_strict_enums;
            if (auto v = body["strict"]) e->strict = to_bool(v);
            if (auto t = body["type"]) {
                auto p = ir::parse_prim(to_str(t));
                if (!p || !ir::prim_is_integer(*p)) diag_.error(loc(t), "enum type must be an integer type");
                else e->underlying = *p;
            }
            if (auto d = body["doc"]) e->doc = to_str(d);
            auto values = body["values"];
            if (!values || !values.IsMap()) {
                diag_.error(e->loc, "enum '" + e->name + "' needs a 'values' mapping");
            } else {
                for (const auto& v : values) e->values.emplace_back(v.first.as<std::string>(), to_int(v.second));
            }
            s.enums.push_back(e);
        }
    }

    void load_custom(const YAML::Node& n, ir::Schema& s) {
        check_keys(n, {"types", "checksums", "framers"}, "custom");
        if (auto types = n["types"]) {
            for (const auto& kv : types) {
                auto c = std::make_shared<ir::CustomType>();
                c->name = kv.first.as<std::string>();
                c->loc = loc(kv.first);
                if (auto w = kv.second["wire_size"]) c->wire_size = to_size(w);
                if (auto w = kv.second["max_wire_size"]) c->max_wire_size = to_size(w);
                c->lang = to_lang(kv.second, {"wire_size", "max_wire_size", "doc"});
                s.custom_types.push_back(c);
            }
        }
        if (auto cs = n["checksums"]) {
            for (const auto& kv : cs) {
                auto c = std::make_shared<ir::CustomChecksum>();
                c->name = kv.first.as<std::string>();
                c->loc = loc(kv.first);
                if (auto w = kv.second["size"]) c->size = to_size(w);
                else diag_.error(c->loc, "custom checksum '" + c->name + "' needs 'size' (bytes)");
                c->lang = to_lang(kv.second, {"size", "doc"});
                s.custom_checksums.push_back(c);
            }
        }
        if (auto fs = n["framers"]) {
            for (const auto& kv : fs) {
                auto c = std::make_shared<ir::CustomFramer>();
                c->name = kv.first.as<std::string>();
                c->loc = loc(kv.first);
                c->lang = to_lang(kv.second, {"doc"});
                s.custom_framers.push_back(c);
            }
        }
    }

    ir::LengthSpec to_length(const YAML::Node& n) {
        ir::LengthSpec l;
        const auto s = to_str(n);
        if (s == "remaining") {
            l.kind = ir::LenKind::Remaining;
        } else if (!s.empty() && std::isdigit(static_cast<unsigned char>(s[0]))) {
            l.kind = ir::LenKind::Fixed;
            l.fixed = to_size(n);
        } else {
            l.kind = ir::LenKind::Expr;
            l.expr = parse_expr(s, loc(n), diag_);
        }
        return l;
    }

    ir::BitField load_bitfield(const YAML::Node& n) {
        ir::BitField b;
        b.loc = loc(n);
        if (!n.IsMap()) { diag_.error(b.loc, "bit field must be a mapping"); return b; }
        if (auto p = n["pad"]) {
            check_keys(n, {"pad"}, "bit padding");
            b.pad = true;
            b.bits = static_cast<unsigned>(to_size(p));
            return b;
        }
        check_keys(n, {"name", "bits", "type", "enum", "scale", "offset", "const", "doc"}, "bit field");
        if (auto v = n["name"]) b.name = to_str(v);
        else diag_.error(b.loc, "bit field needs a 'name'");
        if (auto v = n["bits"]) b.bits = static_cast<unsigned>(to_size(v));
        else diag_.error(b.loc, "bit field '" + b.name + "' needs 'bits'");
        if (auto v = n["type"]) b.type_name = to_str(v);
        if (auto v = n["enum"]) b.type_name = to_str(v);
        if (auto v = n["scale"]) b.scale = to_double(v);
        if (auto v = n["offset"]) b.offset_value = to_double(v);
        if (auto v = n["const"]) b.const_value = to_int(v);
        if (auto v = n["doc"]) b.doc = to_str(v);
        return b;
    }

    ir::FieldPtr load_field(const YAML::Node& n, bool element = false) {
        auto f = std::make_shared<ir::Field>();
        f->loc = loc(n);
        if (n.IsScalar()) {
            // shorthand element type: of: u16
            f->type_name = n.Scalar();
            if (!element) diag_.error(f->loc, "field must be a mapping");
            return f;
        }
        if (!n.IsMap()) { diag_.error(f->loc, "field must be a mapping"); return f; }

        if (auto p = n["pad"]; p && !n["type"]) {
            check_keys(n, {"pad", "doc"}, "padding");
            f->kind = ir::FieldKind::Pad;
            f->type_name = "pad";
            f->len.kind = ir::LenKind::Fixed;
            f->len.fixed = to_size(p);
            return f;
        }

        check_keys(n, {"name", "type", "endian", "bit_order", "fields", "count", "length", "size", "max", "of",
                       "when", "optional", "scale", "offset", "unit", "conversions", "crc", "doc", "const",
                       "terminator", "padding", "base", "width", "precision"},
                   element ? "array element" : "field");
        if (auto v = n["name"]) f->name = to_str(v);
        else if (!element) diag_.error(f->loc, "field needs a 'name'");
        if (auto v = n["type"]) f->type_name = to_str(v);
        else diag_.error(f->loc, "field '" + f->name + "' needs a 'type'");

        if (auto v = n["endian"]) { f->endian = to_endian(v); explicit_endian_.insert(f.get()); }
        if (auto v = n["bit_order"]) { f->bit_order = to_bit_order(v); explicit_bit_order_.insert(f.get()); }
        if (auto v = n["fields"]) {
            if (!v.IsSequence()) diag_.error(loc(v), "'fields' of a bits group must be a list");
            else for (const auto& b : v) f->bits.push_back(load_bitfield(b));
        }
        if (auto v = n["count"]) f->len = to_length(v);
        if (auto v = n["length"]) f->len = to_length(v);
        if (auto v = n["size"]) f->len = to_length(v);
        if (auto v = n["terminator"]) {
            if (f->len.kind != ir::LenKind::None) diag_.error(loc(v), "'terminator' cannot be combined with a length");
            f->len.kind = ir::LenKind::Terminator;
            f->len.terminator = static_cast<std::uint8_t>(to_int(v));
        }
        if (auto v = n["max"]) f->max = to_size(v);
        if (auto v = n["of"]) f->elem = load_field(v, true);
        if (auto v = n["when"]) f->when = parse_expr(to_str(v), loc(v), diag_);
        if (auto v = n["optional"]) f->optional = to_bool(v);
        if (auto v = n["scale"]) f->scale = to_double(v);
        if (auto v = n["offset"]) f->offset_value = to_double(v);
        if (auto v = n["unit"]) f->unit = to_str(v);
        if (auto v = n["conversions"]) {
            if (!v.IsMap()) {
                diag_.error(loc(v), "'conversions' must be a mapping of name -> { scale, offset, unit }");
            } else {
                for (const auto& kv : v) {
                    ir::Conversion cv;
                    cv.name = kv.first.as<std::string>();
                    cv.loc = loc(kv.first);
                    const auto& b = kv.second;
                    if (!b.IsMap()) { diag_.error(cv.loc, "conversion '" + cv.name + "' must be a mapping"); continue; }
                    check_keys(b, {"scale", "offset", "unit"}, "conversion '" + cv.name + "'");
                    if (auto x = b["scale"]) cv.scale = to_double(x);
                    if (auto x = b["offset"]) cv.offset_value = to_double(x);
                    if (auto x = b["unit"]) cv.unit = to_str(x);
                    if (!cv.scale && !cv.offset_value)
                        diag_.error(cv.loc, "conversion '" + cv.name + "' needs 'scale' and/or 'offset'");
                    f->conversions.push_back(std::move(cv));
                }
            }
        }
        if (auto v = n["crc"]) {
            f->crc = std::make_shared<ir::ChecksumSpec>();
            f->crc->present = true;
            f->crc->loc = loc(v);
            load_algo(v, *f->crc, "crc");
        }
        if (auto v = n["doc"]) f->doc = to_str(v);
        if (auto v = n["const"]) f->const_value = to_int(v);
        if (auto v = n["padding"]) {
            const auto s = to_str(v);
            f->pad_char = s.size() == 1 && !std::isdigit(static_cast<unsigned char>(s[0])) ? s[0]
                                                                                           : static_cast<char>(to_int(v));
        }
        if (auto v = n["base"]) f->base = static_cast<int>(to_int(v));
        if (auto v = n["width"]) f->width = static_cast<int>(to_int(v));
        if (auto v = n["precision"]) f->precision = static_cast<int>(to_int(v));
        return f;
    }

    /// computed: { name: { type, value, set: { field: expr, ... }, unit, doc } }
    void load_computed(const YAML::Node& n, ir::Message& m) {
        if (!n.IsMap()) { diag_.error(loc(n), "'computed' must be a mapping of name -> { type, value }"); return; }
        for (const auto& kv : n) {
            ir::Computed c;
            c.name = kv.first.as<std::string>();
            c.loc = loc(kv.first);
            const auto& b = kv.second;
            if (!b.IsMap()) { diag_.error(c.loc, "computed value '" + c.name + "' must be a mapping"); continue; }
            check_keys(b, {"type", "value", "set", "unit", "doc"}, "computed value '" + c.name + "'");
            if (auto v = b["type"]) c.type_name = to_str(v);
            else diag_.error(c.loc, "computed value '" + c.name + "' needs a 'type'");
            if (auto v = b["value"]) c.value = parse_expr(to_str(v), loc(v), diag_);
            else diag_.error(c.loc, "computed value '" + c.name + "' needs a 'value' expression");
            if (auto v = b["set"]) {
                if (!v.IsMap()) diag_.error(loc(v), "'set' must be a mapping of field -> expression over 'value'");
                else
                    for (const auto& s : v) c.set.emplace_back(s.first.as<std::string>(), parse_expr(to_str(s.second), loc(s.second), diag_));
            }
            if (auto v = b["unit"]) c.unit = to_str(v);
            if (auto v = b["doc"]) c.doc = to_str(v);
            m.computed.push_back(std::move(c));
        }
    }

    void load_fields(const YAML::Node& n, ir::Message& m) {
        if (!n) return;
        if (!n.IsSequence()) { diag_.error(loc(n), "'fields' must be a list"); return; }
        for (const auto& f : n) m.fields.push_back(load_field(f));
    }

    void apply_field_defaults(ir::Message& m) {
        auto apply = [&](auto&& self, ir::Field& f) -> void {
            if (!explicit_endian_.contains(&f)) f.endian = m.endian;
            if (!explicit_bit_order_.contains(&f)) f.bit_order = m.bit_order;
            if (f.elem) self(self, *f.elem);
        };
        for (auto& f : m.fields) apply(apply, *f);
    }

    void load_messages(const YAML::Node& n, ir::Schema& s) {
        if (!n.IsMap()) { diag_.error(loc(n), "'messages' must be a mapping of name -> definition"); return; }
        for (const auto& kv : n) {
            auto m = std::make_shared<ir::Message>();
            m->name = kv.first.as<std::string>();
            m->loc = loc(kv.first);
            m->format = s.default_format;
            m->endian = s.default_endian;
            m->bit_order = s.default_bit_order;
            m->separator = s.default_separator;
            m->want_view = s.default_view;
            m->want_struct = s.default_struct;
            const auto& b = kv.second;
            check_keys(b, {"type_id", "format", "endian", "bit_order", "separator", "access", "doc", "fields", "computed"},
                       "message '" + m->name + "'");
            if (auto v = b["format"]) m->format = to_format(v);
            if (auto v = b["endian"]) m->endian = to_endian(v);
            if (auto v = b["bit_order"]) m->bit_order = to_bit_order(v);
            if (auto v = b["separator"]) m->separator = to_char(v);
            if (auto v = b["access"]) to_access(v, m->want_view, m->want_struct);
            if (auto v = b["doc"]) m->doc = to_str(v);
            if (auto v = b["type_id"]) {
                const auto str = to_str(v);
                const bool numeric = !str.empty() && (std::isdigit(static_cast<unsigned char>(str[0])) ||
                                                      (str[0] == '-' && str.size() > 1));
                if (numeric && v.Tag() != "!") m->type_id = to_int(v);
                else m->type_id = str;
            }
            load_fields(b["fields"], *m);
            if (auto v = b["computed"]) load_computed(v, *m);
            apply_field_defaults(*m);
            s.messages.push_back(m);
        }
    }

    void load_framing(const YAML::Node& n, ir::Protocol& p) {
        auto& fr = p.framing;
        fr.loc = loc(n);
        check_keys(n, {"type", "sync", "start", "end", "escape", "size", "length", "max_frame", "framer"}, "framing");
        const auto type = n["type"] ? to_str(n["type"]) : std::string("none");
        if (type == "none") fr.type = ir::FramingSpec::Type::None;
        else if (type == "fixed") fr.type = ir::FramingSpec::Type::Fixed;
        else if (type == "length_prefixed") fr.type = ir::FramingSpec::Type::LengthPrefixed;
        else if (type == "delimited") fr.type = ir::FramingSpec::Type::Delimited;
        else if (type == "custom") fr.type = ir::FramingSpec::Type::Custom;
        else diag_.error(loc(n["type"]), "unknown framing type '" + type + "' (none, fixed, length_prefixed, delimited, custom)");

        if (auto v = n["sync"]) fr.sync = to_bytes(v);
        if (auto v = n["start"]) {
            if (!fr.sync.empty()) diag_.error(loc(v), "use either 'sync' or 'start', not both");
            fr.sync = to_bytes(v);
        }
        if (auto v = n["end"]) fr.end = to_bytes(v);
        if (auto v = n["size"]) fr.size = to_size(v);
        if (auto v = n["max_frame"]) fr.max_frame = to_size(v);
        if (auto v = n["framer"]) fr.custom_name = to_str(v);
        if (auto v = n["length"]) {
            check_keys(v, {"field", "covers", "adjust"}, "framing.length");
            if (auto f = v["field"]) fr.length_field = to_str(f);
            else diag_.error(loc(v), "framing.length needs 'field'");
            if (auto c = v["covers"]) {
                const auto s = to_str(c);
                if (s == "body") fr.length_covers = ir::FramingSpec::LengthCovers::Body;
                else if (s == "header_body") fr.length_covers = ir::FramingSpec::LengthCovers::HeaderBody;
                else if (s == "all") fr.length_covers = ir::FramingSpec::LengthCovers::All;
                else diag_.error(loc(c), "length.covers must be body, header_body or all");
            }
            if (auto a = v["adjust"]) fr.length_adjust = to_int(a);
        }
        if (auto v = n["escape"]) {
            check_keys(v, {"char", "xor", "map"}, "framing.escape");
            fr.escape.enabled = true;
            if (auto c = v["char"]) fr.escape.esc = static_cast<std::uint8_t>(to_int(c));
            else diag_.error(loc(v), "escape needs 'char'");
            if (auto x = v["xor"]) fr.escape.xor_mask = static_cast<std::uint8_t>(to_int(x));
            if (auto m = v["map"]) {
                for (const auto& kv : m)
                    fr.escape.map.emplace_back(static_cast<std::uint8_t>(to_int(kv.first)),
                                               static_cast<std::uint8_t>(to_int(kv.second)));
                if (fr.escape.map.size() > 4) diag_.error(loc(m), "escape map supports at most 4 entries");
            }
        }
    }

    /// A checksum algorithm: a built-in / custom name, or inline `{ crc: { width, poly, ... } }`.
    void load_algo(const YAML::Node& a, ir::ChecksumSpec& c, const std::string& ctx) {
        if (!a.IsMap()) { c.algo = to_str(a); return; }
        check_keys(a, {"crc"}, ctx);
        auto crc = a["crc"];
        if (!crc) { diag_.error(loc(a), "inline algo must be { crc: {...} }"); return; }
        check_keys(crc, {"width", "poly", "init", "refin", "refout", "xorout"}, "crc parameters");
        ir::CrcParams cp;
        if (auto w = crc["width"]) cp.width = static_cast<unsigned>(to_size(w));
        else diag_.error(loc(crc), "crc needs 'width'");
        if (auto w = crc["poly"]) cp.poly = static_cast<std::uint64_t>(to_int(w));
        else diag_.error(loc(crc), "crc needs 'poly'");
        if (auto w = crc["init"]) cp.init = static_cast<std::uint64_t>(to_int(w));
        if (auto w = crc["xorout"]) cp.xorout = static_cast<std::uint64_t>(to_int(w));
        if (auto w = crc["refin"]) cp.refin = to_bool(w);
        if (auto w = crc["refout"]) cp.refout = to_bool(w);
        c.algo = "crc";
        c.crc = cp;
    }

    void load_checksum(const YAML::Node& n, ir::Protocol& p) {
        auto& c = p.checksum;
        c.present = true;
        c.loc = loc(n);
        c.endian = p.endian;
        check_keys(n, {"algo", "covers", "endian", "separator", "required"}, "checksum");
        if (auto a = n["algo"]) load_algo(a, c, "checksum.algo");
        else diag_.error(c.loc, "checksum needs 'algo'");
        if (auto v = n["covers"]) {
            const auto s = to_str(v);
            if (s == "header_body") c.covers = ir::ChecksumSpec::Covers::HeaderBody;
            else if (s == "body") c.covers = ir::ChecksumSpec::Covers::Body;
            else if (s == "all") c.covers = ir::ChecksumSpec::Covers::All;
            else diag_.error(loc(v), "checksum.covers must be header_body, body or all");
        }
        if (auto v = n["endian"]) c.endian = to_endian(v);
        if (auto v = n["separator"]) c.separator = to_char(v);
        if (auto v = n["required"]) c.required = to_bool(v);
    }

    void load_protocols(const YAML::Node& n, ir::Schema& s) {
        if (!n.IsMap()) { diag_.error(loc(n), "'protocols' must be a mapping of name -> definition"); return; }
        for (const auto& kv : n) {
            auto p = std::make_shared<ir::Protocol>();
            p->name = kv.first.as<std::string>();
            p->loc = loc(kv.first);
            p->format = s.default_format;
            p->endian = s.default_endian;
            p->separator = s.default_separator;
            const auto& b = kv.second;
            check_keys(b, {"format", "endian", "separator", "doc", "header", "framing", "checksum", "message_type",
                           "messages"},
                       "protocol '" + p->name + "'");
            if (auto v = b["format"]) p->format = to_format(v);
            if (auto v = b["endian"]) p->endian = to_endian(v);
            if (auto v = b["separator"]) p->separator = to_char(v);
            if (auto v = b["doc"]) p->doc = to_str(v);

            p->header = std::make_shared<ir::Message>();
            p->header->name = p->name + "Header";
            p->header->loc = p->loc;
            p->header->is_header = true;
            p->header->format = Format::Binary;
            p->header->endian = p->endian;
            p->header->bit_order = s.default_bit_order;
            load_fields(b["header"], *p->header);
            apply_field_defaults(*p->header);

            if (auto v = b["framing"]) load_framing(v, *p);
            if (auto v = b["checksum"]) load_checksum(v, *p);
            if (auto v = b["message_type"]) {
                p->message_type.loc = loc(v);
                check_keys(v, {"field", "token"}, "message_type");
                if (auto f = v["field"]) {
                    p->message_type.kind = ir::MessageTypeSpec::Kind::Field;
                    p->message_type.field_name = to_str(f);
                } else if (auto t = v["token"]) {
                    p->message_type.kind = ir::MessageTypeSpec::Kind::Token;
                    p->message_type.token = to_size(t);
                } else {
                    diag_.error(p->message_type.loc, "message_type needs 'field' or 'token'");
                }
            }
            if (auto v = b["messages"]) {
                if (!v.IsSequence()) diag_.error(loc(v), "protocol messages must be a list of message names");
                else for (const auto& m : v) p->message_names.push_back(to_str(m));
            } else {
                diag_.error(p->loc, "protocol '" + p->name + "' needs a 'messages' list");
            }
            s.protocols.push_back(p);
        }
    }

    std::string file_;
    Diagnostics& diag_;
    std::set<const ir::Field*> explicit_endian_;
    std::set<const ir::Field*> explicit_bit_order_;
};

} // namespace

std::unique_ptr<ir::Schema> load_schema_string(const std::string& yaml, const std::string& file, Diagnostics& diag) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml);
    } catch (const YAML::Exception& e) {
        diag.error({file, e.mark.line + 1, e.mark.column + 1}, "YAML syntax error: " + e.msg);
        return nullptr;
    }
    Loader l(file, diag);
    auto s = l.load(root);
    if (diag.has_errors()) return nullptr;
    return s;
}

std::unique_ptr<ir::Schema> load_schema_file(const std::string& path, Diagnostics& diag) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::BadFile&) {
        diag.error({path, 0, 0}, "cannot open file");
        return nullptr;
    } catch (const YAML::Exception& e) {
        diag.error({path, e.mark.line + 1, e.mark.column + 1}, "YAML syntax error: " + e.msg);
        return nullptr;
    }
    Loader l(path, diag);
    auto s = l.load(root);
    if (diag.has_errors()) return nullptr;
    return s;
}

} // namespace gmpc
