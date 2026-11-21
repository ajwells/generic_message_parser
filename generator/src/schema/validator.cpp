// Declaration and post-lowering checks that are independent of any backend.

#include <cctype>
#include <map>
#include <set>

#include "ir/lowering.hpp"

namespace gmpc {

namespace {

bool is_identifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

void check_ident(const std::string& name, const SourceLoc& loc, const char* what, Diagnostics& d) {
    if (!is_identifier(name)) d.error(loc, std::string(what) + " name '" + name + "' is not a valid identifier");
}

void check_fields(const ir::Message& m, Diagnostics& d) {
    for (const auto& f : m.fields) {
        if (f->kind == ir::FieldKind::Pad && f->name.empty()) continue;
        check_ident(f->name, f->loc, "field", d);
        for (const auto& b : f->bits)
            if (!b.pad) check_ident(b.name, b.loc, "bit field", d);
    }
}

} // namespace

void validate_declarations(const ir::Schema& s, Diagnostics& d) {
    if (!s.ns.empty()) {
        std::string part;
        std::size_t i = 0;
        bool ok = true;
        while (i <= s.ns.size()) {
            const auto p = s.ns.find("::", i);
            part = s.ns.substr(i, p == std::string::npos ? std::string::npos : p - i);
            if (!is_identifier(part)) ok = false;
            if (p == std::string::npos) break;
            i = p + 2;
        }
        if (!ok) d.error({s.file, 0, 0}, "namespace '" + s.ns + "' must be identifiers separated by '::'");
    }

    // All type-level names share one namespace in the generated code.
    std::map<std::string, SourceLoc> types;
    auto add_type = [&](const std::string& n, const SourceLoc& loc, const char* what) {
        check_ident(n, loc, what, d);
        if (auto [it, inserted] = types.emplace(n, loc); !inserted)
            d.error(loc, std::string(what) + " '" + n + "' conflicts with another declaration of the same name");
        if (ir::parse_prim(n) || n == "bits" || n == "bytes" || n == "string" || n == "array" || n == "pad")
            d.error(loc, std::string(what) + " '" + n + "' shadows a built-in type");
    };
    for (const auto& e : s.enums) {
        add_type(e->name, e->loc, "enum");
        std::set<std::string> vals;
        for (const auto& [n, v] : e->values) {
            check_ident(n, e->loc, "enum value", d);
            if (!vals.insert(n).second) d.error(e->loc, "duplicate enum value '" + n + "' in '" + e->name + "'");
        }
        if (e->values.empty()) d.error(e->loc, "enum '" + e->name + "' has no values");
    }
    for (const auto& c : s.custom_types) add_type(c->name, c->loc, "custom type");
    for (const auto& m : s.messages) {
        add_type(m->name, m->loc, "message");
        add_type(m->name + "View", m->loc, "message view");
        check_fields(*m, d);
    }
    for (const auto& p : s.protocols) {
        add_type(p->name, p->loc, "protocol");
        add_type(p->header->name, p->loc, "protocol header");
        check_fields(*p->header, d);
    }
}

void validate_lowered(const ir::Schema& s, Diagnostics& d) {
    for (const auto& p : s.protocols) {
        if (p->framing.type == ir::FramingSpec::Type::None && p->max_frame == 0)
            continue;  // whole-buffer only; nothing to check
        if (p->framing.type == ir::FramingSpec::Type::Fixed && !p->framing.sync.empty() && p->checksum.present == false)
            d.warning(p->framing.loc, "fixed framing with a sync word but no checksum cannot detect false sync matches");
    }
    for (const auto& m : s.messages) {
        if (!m->want_struct && !m->want_view) d.error(m->loc, "message '" + m->name + "' disables both view and struct");
        for (const auto& f : m->fields) {
            const ir::Message* child = f->kind == ir::FieldKind::Message ? f->message
                                       : (f->kind == ir::FieldKind::Array && f->elem && f->elem->kind == ir::FieldKind::Message)
                                           ? f->elem->message
                                           : nullptr;
            if (!child) continue;
            if (m->want_view && !child->want_view)
                d.error(f->loc, "'" + m->name + "' has a view, so nested message '" + child->name + "' needs view access too");
            if (m->want_struct && !child->want_struct)
                d.error(f->loc, "'" + m->name + "' has a struct, so nested message '" + child->name + "' needs struct access too");
        }
    }
}

} // namespace gmpc
