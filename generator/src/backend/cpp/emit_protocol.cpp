// C++ emission for protocols: framing, checksum validation, message selection, streaming and encoding.

#include <algorithm>
#include <set>
#include <sstream>

#include "backend/cpp/cpp_common.hpp"

namespace gmpc::cpp {

namespace {

using namespace ir;
using FT = FramingSpec::Type;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string fail(const char* err) { return std::string("return gmsg::fail(gmsg::Error::") + err + ");"; }

std::string hex_byte(std::uint8_t b) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << (b < 16 ? "0" : "") << static_cast<int>(b);
    return os.str();
}

std::string byte_list(const std::vector<std::uint8_t>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + hex_byte(v[i]);
    return s;
}

std::string pattern(const std::vector<std::uint8_t>& v) {
    return "gmsg::Pattern{{" + byte_list(v) + "}, " + sz(v.size()) + "}";
}

std::string escape_config(const Protocol& p) {
    const auto& e = p.framing.escape;
    if (!e.enabled) return "gmsg::EscapeConfig{}";
    std::string raw, code;
    for (const auto& [r, c] : e.table) {
        if (e.code == EscapeSpec::Code::Xor && r == e.esc) continue;  // always escaped
        raw += (raw.empty() ? "" : ", ") + hex_byte(r);
        code += (code.empty() ? "" : ", ") + hex_byte(c);
    }
    switch (e.code) {
        case EscapeSpec::Code::Xor:
            return "gmsg::make_xor_escape(" + hex_byte(e.esc) + ", " + hex_byte(e.xor_mask) + ", {" + raw + "})";
        case EscapeSpec::Code::Index:
            return "gmsg::make_index_escape(" + hex_byte(e.esc) + ", {" + raw + "}" +
                   (e.base ? ", " + hex_byte(e.base) : std::string()) + ")";
        case EscapeSpec::Code::Map:
            return "gmsg::make_map_escape(" + hex_byte(e.esc) + ", {" + raw + "}, {" + code + "})";
    }
    return "gmsg::EscapeConfig{}";
}

char text_sep(const Protocol& p) { return p.separator; }

std::string char_lit(char ch) {
    if (ch == '\'') return "'\\''";
    if (ch == '\\') return "'\\\\'";
    return std::string("'") + ch + "'";
}

class ProtocolEmitter {
public:
    ProtocolEmitter(Ctx& c, const Protocol& p) : c_(c), w_(c.w), p_(p) {
        text_ = p.format == Format::Text;
        has_header_ = !text_ && !p.header->fields.empty();
        escape_ = p.framing.escape.enabled;
        wire_cs_ = escape_ && p.checksum.present && p.checksum.over_wire && !text_;
        // escaped binary frames with a built-in checksum: verify it over the escaped bytes, then unescape
        cs_first_ = escape_ && !text_ && p.checksum.present && !p.checksum.over_wire && !p.checksum.custom;
        stream_ = p.framing.type != FT::None && p.max_frame > 0;
        all_struct_ = std::all_of(p.messages.begin(), p.messages.end(), [](const Message* m) { return m->want_struct; });
        // Lowering guarantees every protocol message has a view and (text) the protocol's separator.
        if (!c.schema.protocols.empty() && c.schema.protocols.front().get() == &p) check_reserved_names();  // once per schema
    }

    void emit() {
        doc(w_, p_.doc);
        w_.open("struct " + p_.name + " {");
        emit_constants();
        emit_framing();
        emit_frame_struct();
        emit_open();
        emit_visit();
        emit_parse();
        emit_process();
        if (stream_) emit_callbacks();
        emit_encode();
        w_.close("};");
        w_.line();
    }

private:
    /// Names declared inside every protocol struct. A schema message or enum with one of these names would be
    /// shadowed inside the struct (e.g. `encode(const Callbacks&)` would refer to the nested type).
    void check_reserved_names() {
        static const std::set<std::string> reserved = {"Header", "HeaderView", "AnyView", "AnyMessage", "Frame", "Stream",
                                                       "StreamWith", "Callbacks", "CallbackStream", "Received", "Endpoint"};
        auto check = [&](const std::string& name, const SourceLoc& loc, const char* what) {
            if (reserved.contains(name))
                c_.diag.error(loc, std::string(what) + " '" + name + "' clashes with a name generated inside protocol '" +
                                       p_.name + "' (reserved: Header, HeaderView, AnyView, AnyMessage, Frame, Stream, "
                                       "StreamWith, Callbacks, CallbackStream, Received, Endpoint); rename it");
        };
        for (const auto& m : c_.schema.messages)
            if (!m->is_header) check(m->name, m->loc, "message");
        for (const auto& e : c_.schema.enums) check(e->name, e->loc, "enum");
    }

    // -------------------------------------------------------------------------------------------------
    void emit_constants() {
        w_.line("static constexpr std::size_t max_frame = " + sz(p_.max_frame) + ";");
        w_.line("static constexpr std::size_t prefix_size = " + sz(p_.prefix_size) + ";   // sync / start delimiter");
        w_.line("static constexpr std::size_t header_size = " + sz(p_.header_size) + ";");
        w_.line("static constexpr std::size_t trailer_size = " + sz(p_.trailer_size) + ";  // binary checksum");
        w_.line("static constexpr std::size_t suffix_size = " + sz(p_.suffix_size) + ";   // end delimiter");
        if (!p_.framing.sync.empty())
            w_.line("static constexpr std::array<std::uint8_t, " + sz(p_.framing.sync.size()) + "> prefix{" +
                    byte_list(p_.framing.sync) + "};");
        if (!p_.framing.end.empty())
            w_.line("static constexpr std::array<std::uint8_t, " + sz(p_.framing.end.size()) + "> suffix{" +
                    byte_list(p_.framing.end) + "};");
        if (has_header_) {
            w_.line("using Header = " + p_.header->name + ";");
            w_.line("using HeaderView = " + view_name(*p_.header) + ";");
        }
        std::string views, structs;
        for (std::size_t i = 0; i < p_.messages.size(); ++i) {
            views += (i ? ", " : "") + view_name(*p_.messages[i]);
            structs += (i ? ", " : "") + p_.messages[i]->name;
        }
        w_.line("using AnyView = std::variant<" + views + ">;");
        if (all_struct_) w_.line("using AnyMessage = std::variant<" + structs + ">;");
        if (stream_) {
            w_.line("/// Incremental parser; Capacity must be >= max_frame. Pass the handler on each feed()/next() call.");
            w_.line("template <std::size_t Capacity = max_frame>");
            w_.line("using Stream = gmsg::StreamParser<" + p_.name + ", Capacity>;");
            w_.line("/// Incremental parser that stores its handler (a named type): feed(bytes) / next() without a handler.");
            w_.line("template <class Handler, std::size_t Capacity = max_frame>");
            w_.line("using StreamWith = gmsg::StreamParser<" + p_.name + ", Capacity, Handler>;");
            w_.line("/// StreamWith for any handler, e.g. make_stream(gmsg::overloaded{[](const AView&) {}, ...}).");
            w_.line("template <std::size_t Capacity = max_frame, class Handler>");
            w_.line("static auto make_stream(Handler&& h) { return StreamWith<std::decay_t<Handler>, Capacity>(std::forward<Handler>(h)); }");
            w_.line("/// A frame returned by Stream::poll_frame(). Views point into the parser buffer (valid until its next call).");
            w_.open("struct Received {");
            if (has_header_) w_.line("HeaderView header{};");
            w_.line("AnyView message;");
            w_.line(escape_ ? "gmsg::bytes_view bytes{};  ///< the frame in the buffer (unescaped in place: only header and body are meaningful)"
                            : "gmsg::bytes_view bytes{};  ///< the whole frame as received");
            w_.line(std::string("std::size_t extra = 0;  ///< ") + (text_ ? "tokens" : "bytes") +
                    " in the frame after the message (ignored" + (p_.exact_size ? "; always 0 with exact_size)" : ")"));
            w_.close("};");
        }
        w_.line();
    }

    void emit_framing() {
        const auto& fr = p_.framing;
        switch (fr.type) {
            case FT::None:
                w_.line("/// No framing: the whole buffer is one frame.");
                w_.line("static constexpr gmsg::FrameScan scan(gmsg::bytes_view b) noexcept { return gmsg::FrameScan::frame(b.size(), b.size(), b.size()); }");
                break;
            case FT::Fixed:
                w_.line("static constexpr gmsg::FixedFrameConfig framing_config{" + pattern(fr.sync) + ", " + sz(*fr.size) +
                        (escape_ ? ", " + escape_config(p_) + ", suffix_size" : std::string()) + "};");
                w_.line("static constexpr gmsg::FrameScan scan(gmsg::bytes_view b) noexcept { return gmsg::scan_fixed(b, framing_config); }");
                break;
            case FT::LengthPrefixed: {
                const auto prefix = static_cast<std::int64_t>(p_.prefix_size);
                const auto header = static_cast<std::int64_t>(p_.header_size);
                const auto trailer = static_cast<std::int64_t>(p_.trailer_size);
                const auto suffix = static_cast<std::int64_t>(p_.suffix_size);
                std::int64_t add = fr.length_adjust + suffix;
                if (fr.length_counts_wire) add = fr.length_adjust;  // body wire size = value + adjust
                else if (fr.length_covers == FramingSpec::LengthCovers::Body) add += prefix + header + trailer;
                else if (fr.length_covers == FramingSpec::LengthCovers::BodyChecksum) add += prefix + header;
                else if (fr.length_covers == FramingSpec::LengthCovers::HeaderBody) add += prefix + trailer;
                w_.line("static constexpr gmsg::LengthPrefixedConfig framing_config{" + pattern(fr.sync) + ", " +
                        sz(p_.length_field_offset) + ", " + sz(prim_size(fr.length->prim)) + ", " +
                        endian_expr(fr.length->endian) + ", " + int_literal(add) + ", " +
                        sz(p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size) + ", max_frame" +
                        (escape_ ? ", " + escape_config(p_) + ", suffix_size" : std::string()) +
                        (fr.length_counts_wire
                             ? std::string(", true, header_size, trailer_size, ") +
                                   (fr.length_covers == FramingSpec::LengthCovers::BodyChecksum ? "true" : "false")
                             : std::string()) +
                        "};");
                if (p_.size_checks.empty()) {
                    w_.line("static constexpr gmsg::FrameScan scan(gmsg::bytes_view b) noexcept { return gmsg::scan_length_prefixed(b, framing_config); }");
                    break;
                }
                emit_checked_scan();
                break;
            }
            case FT::Delimited:
                w_.line("static constexpr gmsg::DelimitedConfig framing_config{" + pattern(fr.sync) + ", " + pattern(fr.end) +
                        ", " + escape_config(p_) + ", max_frame};");
                w_.line("static constexpr gmsg::FrameScan scan(gmsg::bytes_view b) noexcept { return gmsg::scan_delimited(b, framing_config); }");
                break;
            case FT::Custom: {
                const auto t = lang_value(fr.custom->lang, "type");
                w_.line("static_assert(gmsg::Framer<" + t + ">, \"custom framer '" + fr.custom->name +
                        "' must provide static gmsg::FrameScan scan(gmsg::bytes_view)\");");
                w_.line("static gmsg::FrameScan scan(gmsg::bytes_view b) noexcept { return " + t + "::scan(b); }");
                break;
            }
        }
        w_.line();
    }

    /// Condition rejecting `body` for one size check ("" when it cannot reject anything).
    static std::string size_fail(const SizeCheck& c) {
        std::string s;
        if (c.min > 0) s = "body < " + std::to_string(c.min);
        if (c.max) s += (s.empty() ? "" : " || ") + std::string("body > ") + std::to_string(*c.max);
        return s;
    }

    /// length_prefixed scan that also rejects a length the frame's message type cannot have, as soon as the header has
    /// arrived (instead of waiting for a body that may never come).
    void emit_checked_scan() {
        w_.line(std::string("/// Scan for a frame. A length the frame's message type cannot have (") +
                (p_.exact_size ? "shorter or longer than the message" : "shorter than the message") +
                ") is rejected with BadLength as soon as the header has arrived.");
        w_.open("static gmsg::FrameScan scan(gmsg::bytes_view b) noexcept {");
        w_.line("const gmsg::FrameScan s = gmsg::scan_length_prefixed(b, framing_config);");
        w_.line("if (s.kind == gmsg::FrameScan::Kind::skip) return s;");
        w_.line("std::array<std::uint8_t, header_size> h{};");
        w_.line("if (!gmsg::read_frame_header(b, framing_config.sync.len, framing_config.escape, gmsg::mut_bytes(h))) return s;");
        w_.line("const auto hv = HeaderView::unchecked(gmsg::bytes_view(h));");
        w_.line("const std::int64_t body = static_cast<std::int64_t>(hv." + ident(p_.framing.length->name) + "()) + " +
                int_literal(p_.length_to_body) + ";");
        if (p_.message_type.kind == MessageTypeSpec::Kind::None) {
            w_.line("if (" + size_fail(p_.size_checks.front()) + ") return gmsg::FrameScan::skip(1, gmsg::Error::BadLength);");
        } else {
            w_.open("switch (gmsg::to_int(hv." + ident(p_.message_type.field->name) + "())) {");
            for (const auto& c : p_.size_checks)
                w_.line("case " + type_id_literal(c.message) + ": if (" + size_fail(c) +
                        ") return gmsg::FrameScan::skip(1, gmsg::Error::BadLength); break;  // " + p_.messages[c.message]->name);
            w_.line("default: break;");
            w_.close();
        }
        w_.line("return s;");
        w_.close();
    }

    void emit_frame_struct() {
        w_.line("/// A validated frame split into its parts (views into the input buffer).");
        w_.open("struct Frame {");
        if (has_header_) w_.line("HeaderView header{};");
        if (text_) w_.line("std::string_view type_id{};  // text: the first token");
        w_.line("gmsg::bytes_view body{};");
        if (escape_) w_.line("std::size_t unescaped_size = 0;  ///< size of the unescaped part (for restore_frame)");
        w_.close("};");
        w_.line();
    }

    std::string frame_param() const { return escape_ ? "gmsg::mut_bytes frame" : "gmsg::bytes_view frame"; }

    void emit_checksum_compare(const std::string& covered, const std::string& stored) {
        bool fn = false;
        const auto impl = checksum_impl(p_.checksum, fn);
        const auto call = fn ? impl + "(" + covered + ")" : impl + "::compute(" + covered + ")";
        w_.line("const auto computed = static_cast<std::uint64_t>(" + call + ");");
        w_.line("if (" + stored + " != computed) " + fail("BadChecksum"));
    }

    void emit_open() {
        w_.line("/// Validate one complete frame (as delimited by scan()) and split it into header and body.");
        if (escape_) {
            w_.line(cs_first_ || wire_cs_ ? "/// The checksum is verified over the escaped bytes; then the frame is unescaped in place."
                              : "/// The frame is unescaped in place.");
            w_.line("/// On failure the frame holds the bytes as received again (see restore_frame).");
        }
        w_.open("[[nodiscard]] static gmsg::Result<Frame> open(" + frame_param() + ") noexcept {");
        w_.line("if (frame.size() < prefix_size + header_size + trailer_size + suffix_size) " + fail("Truncated"));
        if (wire_cs_) emit_wire_checksum();
        if (!escape_) {
            w_.line("const gmsg::bytes_view interior = frame.subspan(prefix_size, frame.size() - prefix_size - suffix_size);");
            if (text_) emit_open_text();
            else emit_open_binary();
            w_.close();
            w_.line();
            return;
        }
        w_.line("const gmsg::mut_bytes wire = frame.subspan(prefix_size, frame.size() - prefix_size - suffix_size);");
        w_.line("const auto n = gmsg::check_escaped(wire, framing_config.escape);");
        w_.line("if (!n) return gmsg::fail(n.error());");
        if (cs_first_ || wire_cs_) emit_escaped_checksum();
        w_.line("(void)gmsg::unescape_in_place(wire, framing_config.escape);");
        w_.line("const gmsg::bytes_view interior(frame.data() + prefix_size, *n);");
        w_.open("auto r = [&]() -> gmsg::Result<Frame> {");
        if (text_) emit_open_text();
        else emit_open_binary();
        w_.close("}();");
        w_.line("if (!r) restore_frame(frame, *n);");
        w_.line("else r->unescaped_size = *n;");
        w_.line("return r;");
        w_.close();
        w_.line();
        w_.line("/// Escape the unescaped part of a frame opened by open() again, so the frame holds the bytes as received");
        w_.line("/// (an escape pair of a byte that needs no escaping comes back as the plain byte).");
        w_.open("static void restore_frame(gmsg::mut_bytes frame, std::size_t unescaped_size) noexcept {");
        w_.line("(void)gmsg::escape_in_place(frame.first(frame.size() - suffix_size), prefix_size, unescaped_size, framing_config.escape);");
        w_.close();
        w_.line();
    }

    /// Escaped binary frames with a built-in checksum: verify it before anything is unescaped.
    void emit_escaped_checksum() {
        bool fn = false;
        const auto impl = checksum_impl(p_.checksum, fn);
        w_.line("if (*n < header_size + trailer_size) " + fail("Truncated"));
        w_.open("{");
        w_.line("std::array<std::uint8_t, trailer_size> sb{};");
        w_.line("std::size_t k = 0;");
        w_.line("gmsg::for_each_unescaped(wire, framing_config.escape, *n - trailer_size, *n, [&](gmsg::bytes_view d) { for (auto b : d) sb[k++] = b; });");
        w_.line("const auto stored = gmsg::load_uint(sb.data(), trailer_size, " + endian_expr(p_.checksum.endian) + ");");
        if (wire_cs_) {
            w_.line("if (stored != computed) " + fail("BadChecksum"));
            w_.close();
            return;
        }
        w_.line("auto c = " + impl + "::init();");
        if (p_.checksum.covers == ChecksumSpec::Covers::All)
            w_.line("c = " + impl + "::update(c, gmsg::bytes_view(frame.data(), prefix_size));");
        const std::string from = p_.checksum.covers == ChecksumSpec::Covers::Body ? "header_size" : "0";
        w_.line("gmsg::for_each_unescaped(wire, framing_config.escape, " + from + ", *n - trailer_size, [&](gmsg::bytes_view d) { c = " +
                impl + "::update(c, d); });");
        w_.line("if (stored != static_cast<std::uint64_t>(" + impl + "::finish(c))) " + fail("BadChecksum"));
        w_.close();
    }

    /// checksum.over: wire - compute the checksum over the escaped bytes before unescaping.
    void emit_wire_checksum() {
        w_.line("std::uint64_t computed = 0;  // checksum.over: wire");
        w_.open("{");
        w_.line("const gmsg::bytes_view wire = frame.subspan(prefix_size, frame.size() - prefix_size - suffix_size);");
        w_.line("const auto un = gmsg::unescaped_size(wire, framing_config.escape);");
        w_.line("if (!un) return gmsg::fail(un.error());");
        w_.line("if (*un < header_size + trailer_size) " + fail("Truncated"));
        w_.line("const auto t = gmsg::walk_escaped(wire, 0, *un - trailer_size, framing_config.escape);");
        w_.line("if (t.kind != gmsg::EscapeWalk::Kind::done) return gmsg::fail(t.kind == gmsg::EscapeWalk::Kind::bad ? t.error : gmsg::Error::Truncated);");
        std::string covered;
        switch (p_.checksum.covers) {
            case ChecksumSpec::Covers::HeaderBody: covered = "wire.first(t.wire)"; break;
            case ChecksumSpec::Covers::Body:
                w_.line("const auto h = gmsg::walk_escaped(wire, 0, header_size, framing_config.escape);");
                covered = "wire.subspan(h.wire, t.wire - h.wire)";
                break;
            case ChecksumSpec::Covers::All: covered = "gmsg::bytes_view(frame.data(), prefix_size + t.wire)"; break;
        }
        w_.line("computed = static_cast<std::uint64_t>(" + checksum_call(covered) + ");");
        w_.close();
    }

    void emit_open_binary() {
        w_.line("if (interior.size() < header_size + trailer_size) " + fail("Truncated"));
        if (p_.checksum.present && !cs_first_ && !wire_cs_) {
            std::string covered;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: covered = "interior.first(interior.size() - trailer_size)"; break;
                case ChecksumSpec::Covers::Body:
                    covered = "interior.subspan(header_size, interior.size() - header_size - trailer_size)";
                    break;
                case ChecksumSpec::Covers::All:
                    covered = "gmsg::bytes_view(frame.data(), prefix_size + interior.size() - trailer_size)";
                    break;
            }
            w_.open("{");
            w_.line("const auto stored = gmsg::load_uint(interior.data() + interior.size() - trailer_size, trailer_size, " +
                    endian_expr(p_.checksum.endian) + ");");
            emit_checksum_compare(covered, "stored");
            w_.close();
        }
        w_.line("Frame f;");
        if (has_header_) {
            w_.line("auto h = HeaderView::from(interior.first(header_size));");
            w_.line("if (!h) return gmsg::fail(h.error());");
            w_.line("f.header = *h;");
        }
        w_.line("f.body = interior.subspan(header_size, interior.size() - header_size - trailer_size);");
        w_.line("return f;");
    }

    void emit_open_text() {
        w_.line("std::string_view text = gmsg::as_string_view(interior);");
        if (p_.checksum.present) {
            const auto n = p_.checksum.size;
            w_.line("const auto star = text.rfind(" + char_lit(p_.checksum.separator) + ");");
            w_.open("if (star == std::string_view::npos) {");
            if (p_.checksum.required) w_.line(fail("BadChecksum"));
            w_.dedent();
            w_.open("} else {");
            w_.line("const std::string_view hex = text.substr(star + 1);");
            w_.line("if (hex.size() != " + sz(2 * n) + ") " + fail("BadChecksum"));
            w_.line("std::uint64_t stored = 0;");
            w_.open("for (std::size_t i = 0; i < " + sz(n) + "; ++i) {");
            w_.line("auto b = gmsg::text::parse_hex_byte(hex.substr(2 * i, 2));");
            w_.line("if (!b) " + fail("BadChecksum"));
            w_.line("stored = (stored << 8) | *b;");
            w_.close();
            emit_checksum_compare("gmsg::as_bytes(text.data(), star)", "stored");
            w_.line("text = text.substr(0, star);");
            w_.close();
        }
        w_.line("Frame f;");
        w_.line("const auto p = text.find(" + char_lit(text_sep(p_)) + ");");
        w_.line("f.type_id = text.substr(0, p);");
        w_.line("const std::string_view rest = p == std::string_view::npos ? std::string_view{} : text.substr(p + 1);");
        w_.line("f.body = gmsg::as_bytes(rest.data(), rest.size());");
        w_.line("return f;");
    }

    std::string type_id_literal(std::size_t i) const {
        const auto& id = p_.type_ids[i];
        if (auto* s = std::get_if<std::string>(&id)) return string_literal(*s);
        if (auto* n = std::get_if<std::int64_t>(&id)) return int_literal(*n);
        return "";
    }

    void emit_visit() {
        w_.line("/// Decode the message view selected by the frame's type id (message_type) and pass it to fn(view).");
        w_.line("template <class F>");
        w_.open("static gmsg::Error visit_frame(const Frame& f, F&& fn) noexcept {");
        auto body = [&](std::size_t i) {
            const auto v = view_name(*p_.messages[i]);
            w_.line("auto v = " + v + "::from(f.body);");
            w_.line("if (!v) return v.error();");
            if (p_.exact_size) w_.line("if (gmsg::extra_of(*v) != 0) return gmsg::Error::BadLength;  // exact_size");
            w_.line("fn(*v);");
            w_.line("return gmsg::Error::None;");
        };
        switch (p_.message_type.kind) {
            case MessageTypeSpec::Kind::None:
                body(0);
                break;
            case MessageTypeSpec::Kind::Field: {
                w_.open("switch (gmsg::to_int(f.header." + ident(p_.message_type.field->name) + "())) {");
                for (std::size_t i = 0; i < p_.messages.size(); ++i) {
                    w_.open("case " + type_id_literal(i) + ": {  // " + p_.messages[i]->name);
                    body(i);
                    w_.close();
                }
                w_.line("default: return gmsg::Error::UnknownType;");
                w_.close();
                break;
            }
            case MessageTypeSpec::Kind::Token: {
                for (std::size_t i = 0; i < p_.messages.size(); ++i) {
                    w_.open("if (f.type_id == " + type_id_literal(i) + ") {");
                    body(i);
                    w_.close();
                }
                w_.line("return gmsg::Error::UnknownType;");
                break;
            }
        }
        w_.close();
        w_.line();
        w_.open("[[nodiscard]] static gmsg::Result<AnyView> select_message(const Frame& f) noexcept {");
        w_.line("gmsg::Result<AnyView> out = gmsg::fail(gmsg::Error::UnknownType);");
        w_.line("const gmsg::Error e = visit_frame(f, [&](const auto& v) { out = AnyView(v); });");
        w_.line("if (e != gmsg::Error::None) return gmsg::fail(e);");
        w_.line("return out;");
        w_.close();
        w_.line();
    }

    void emit_parse() {
        const auto buf = escape_ ? std::string("gmsg::mut_bytes buffer") : std::string("gmsg::bytes_view buffer");
        w_.line("/// Parse one frame located at the start of `buffer` (whole-buffer / datagram mode).");
        w_.open("[[nodiscard]] static gmsg::Result<AnyView> parse(" + buf + ") noexcept {");
        w_.line("const gmsg::FrameScan s = scan(buffer);");
        w_.line("if (s.kind == gmsg::FrameScan::Kind::need_more) " + fail("Truncated"));
        w_.line("if (s.kind == gmsg::FrameScan::Kind::skip) return gmsg::fail(s.error == gmsg::Error::None ? gmsg::Error::BadSync : s.error);");
        w_.line("auto f = open(buffer.first(s.size));");
        w_.line("if (!f) return gmsg::fail(f.error());");
        if (escape_) {
            w_.line("auto v = select_message(*f);");
            w_.line("if (!v) restore_frame(buffer.first(s.size), f->unescaped_size);");
            w_.line("return v;");
        } else {
            w_.line("return select_message(*f);");
        }
        w_.close();
        if (all_struct_) {
            w_.line("/// Parse one frame and decode it into the eager message struct.");
            w_.open("[[nodiscard]] static gmsg::Result<AnyMessage> decode(" + buf + ") noexcept {");
            w_.line("auto v = parse(buffer);");
            w_.line("if (!v) return gmsg::fail(v.error());");
            w_.open("return std::visit([](const auto& view) -> gmsg::Result<AnyMessage> {");
            w_.line("auto m = view.to_struct();");
            w_.line("if (!m) return gmsg::fail(m.error());");
            w_.line("return AnyMessage(std::move(*m));");
            w_.close("}, *v);");
            w_.close();
        }
        w_.line();
    }

    void emit_process() {
        w_.line("/// StreamParser hook: validate a frame, select its message type and deliver it to `handler`.");
        w_.line("/// The handler is called as handler(header_view, message_view) or handler(message_view),");
        w_.line("/// whichever it accepts; message types it does not accept are ignored. `extra` (if given) receives the");
        w_.line(std::string("/// ") + (text_ ? "tokens" : "bytes") + " after the message in the frame.");
        w_.line("template <class H>");
        w_.open("static gmsg::Error process(gmsg::mut_bytes frame, H& handler, std::size_t* extra = nullptr) {");
        w_.line("auto f = open(frame);");
        w_.line("if (!f) return f.error();");
        w_.open(std::string(escape_ ? "const gmsg::Error e = " : "return ") + "visit_frame(*f, [&](const auto& v) {");
        w_.line("using V = std::decay_t<decltype(v)>;");
        w_.line("if (extra) *extra = gmsg::extra_of(v);");
        if (has_header_) {
            w_.line("if constexpr (std::invocable<H&, const HeaderView&, const V&>) handler(f->header, v);");
            w_.line("else if constexpr (std::invocable<H&, const V&>) handler(v);");
        } else {
            w_.line("if constexpr (std::invocable<H&, const V&>) handler(v);");
        }
        w_.close("});");
        if (escape_) {
            w_.line("if (e != gmsg::Error::None) restore_frame(frame, f->unescaped_size);");
            w_.line("return e;");
        }
        w_.close();
        w_.line();
    }

    /// Runtime callbacks (one listener per message, every frame, errors) and CallbackStream.
    void emit_callbacks() {
        const std::string hdr = has_header_ ? "const HeaderView& h, " : "";
        const std::string hdr_arg = has_header_ ? "h, " : "";
        const std::string hdr_type = has_header_ ? "const HeaderView&, " : "";
        auto slot_name = [](const Message& m) { return "gmsg_on_" + m.name + "_"; };

        w_.line("/// Runtime callbacks for CallbackStream: one listener per message, one for every frame (AnyView) and one");
        w_.line("/// for dropped data (gmsg::FrameError). on<Fn>(ctx) picks the slot from Fn's last parameter.");
        if (has_header_)
            w_.line("/// Fn may take (View) or (HeaderView, View); the header is passed only when Fn asks for it.");
        w_.open("class Callbacks {");
        w_.label("public:");
        w_.line("/// Register Fn with context `ctx`: a member function of C, or a function taking C& / C* first.");
        w_.line("template <auto Fn, class C>");
        w_.line("Callbacks& on(C& ctx) noexcept { slot<gmsg::callback_message_t<decltype(Fn)>>().template bind<Fn>(ctx); return *this; }");
        w_.line("/// Register Fn that takes no context.");
        w_.line("template <auto Fn>");
        w_.line("Callbacks& on() noexcept { slot<gmsg::callback_message_t<decltype(Fn)>>().template bind<Fn>(); return *this; }");
        w_.line("/// Register a function pointer chosen at run time, called as fn(ctx, ...) (C-style void* or a typed pointer).");
        w_.line("template <class F, class C> requires std::is_function_v<F>");
        w_.line("Callbacks& on(F* fn, C* ctx) noexcept { slot<gmsg::callback_message_t<F*>>().bind(fn, ctx); return *this; }");
        w_.line("template <class F> requires std::is_function_v<F>");
        w_.line("Callbacks& on(F* fn) noexcept { slot<gmsg::callback_message_t<F*>>().bind(fn); return *this; }");
        w_.line("/// Remove the listener of V (a message view, AnyView or gmsg::FrameError).");
        w_.line("template <class V>");
        w_.line("Callbacks& off() noexcept { slot<V>().reset(); return *this; }");
        w_.line("template <class V>");
        w_.line("[[nodiscard]] bool has() const noexcept { return static_cast<bool>(const_cast<Callbacks*>(this)->slot<V>()); }");
        w_.line("void clear() noexcept { *this = Callbacks{}; }");
        w_.line();
        w_.line("// handler interface used by the stream parser: the message listener first, then the every-frame listener");
        for (const auto* m : p_.messages) {
            const auto v = view_name(*m);
            w_.line("void operator()(" + hdr + "const " + v + "& v) const { " + slot_name(*m) + "(" + hdr_arg +
                    "v); if (gmsg_on_frame_) gmsg_on_frame_(" + hdr_arg + "AnyView(v)); }");
        }
        w_.line("void operator()(const gmsg::FrameError& e) const { gmsg_on_error_(e); }");
        w_.line();
        w_.label("private:");
        w_.line("template <class V>");
        w_.open("auto& slot() noexcept {");
        bool first = true;
        auto branch = [&](const std::string& type, const std::string& member) {
            w_.line(std::string(first ? "if" : "else if") + " constexpr (std::is_same_v<V, " + type + ">) return " + member + ";");
            first = false;
        };
        for (const auto* m : p_.messages) branch(view_name(*m), slot_name(*m));
        branch("AnyView", "gmsg_on_frame_");
        branch("gmsg::FrameError", "gmsg_on_error_");
        w_.line("else static_assert(gmsg::detail::always_false<V>, \"" + p_.name +
                "::Callbacks: the callback's last parameter must be one of this protocol's message views, AnyView or "
                "gmsg::FrameError\");");
        w_.close();
        w_.line();
        for (const auto* m : p_.messages)
            w_.line("gmsg::CallbackSlot<" + hdr_type + "const " + view_name(*m) + "&> " + slot_name(*m) + ";");
        w_.line("gmsg::CallbackSlot<" + hdr_type + "const AnyView&> gmsg_on_frame_;");
        w_.line("gmsg::CallbackSlot<const gmsg::FrameError&> gmsg_on_error_;");
        w_.close("};");
        w_.line();

        const auto base = "gmsg::StreamParser<" + p_.name + ", Capacity, Callbacks&>";
        w_.line("/// Stream parser with runtime callbacks: register with on<&App::fn>(app), then feed(bytes).");
        w_.line("template <std::size_t Capacity = max_frame>");
        w_.open("class CallbackStream : private Callbacks, public " + base + " {");
        w_.label("public:");
        w_.line("CallbackStream() noexcept : Callbacks(), " + base + "(static_cast<Callbacks&>(*this)) {}");
        w_.line("CallbackStream(const CallbackStream&) = delete;");
        w_.line("CallbackStream& operator=(const CallbackStream&) = delete;");
        w_.line("using Callbacks::on;");
        w_.line("using Callbacks::off;");
        w_.line("using Callbacks::has;");
        w_.line("using Callbacks::clear;");
        w_.line("[[nodiscard]] Callbacks& callbacks() noexcept { return *this; }");
        w_.close("};");
        w_.line();

        w_.line("/// CallbackStream joined to a byte source, a byte sink and an optional clock: service(), send(), wait_for<M>(),");
        w_.line("/// transact<M>() (gmp/endpoint.hpp).");
        w_.line("template <class Source, class Sink, class Clock = void, std::size_t Capacity = max_frame>");
        w_.line("using Endpoint = gmsg::Endpoint<" + p_.name + ", Source, Sink, Clock, Capacity>;");
        w_.line("/// Endpoint over `source` and `sink` (lvalues are kept by reference) and a clock (a std::chrono clock, or a");
        w_.line("/// callable returning a monotonic unsigned time).");
        w_.line("template <std::size_t Capacity = max_frame, class Source, class Sink>");
        w_.line("static auto make_endpoint(Source&& source, Sink&& sink) { return gmsg::make_endpoint<" + p_.name +
                ", Capacity>(std::forward<Source>(source), std::forward<Sink>(sink)); }");
        w_.line("template <std::size_t Capacity = max_frame, class Source, class Sink, class Clock>");
        w_.line("static auto make_endpoint(Source&& source, Sink&& sink, Clock&& clock) { return gmsg::make_endpoint<" + p_.name +
                ", Capacity>(std::forward<Source>(source), std::forward<Sink>(sink), std::forward<Clock>(clock)); }");
        w_.line();
    }

    // -------------------------------------------------------------------------------------------------
    // encode
    // -------------------------------------------------------------------------------------------------
    void emit_encode() {
        for (std::size_t i = 0; i < p_.messages.size(); ++i) {
            const auto* m = p_.messages[i];
            if (!m->want_struct) continue;
            if (text_) emit_encode_text(i);
            else emit_encode_binary(i);
        }
        if (all_struct_) {
            w_.line("/// Encode any message of this protocol.");
            w_.open("static gmsg::Result<std::size_t> encode(const AnyMessage& m, gmsg::mut_bytes out) noexcept {");
            w_.line("return std::visit([&](const auto& x) { return encode(x, out); }, m);");
            w_.close();
        }
    }

    void emit_encode_binary(std::size_t i) {
        const auto* m = p_.messages[i];
        const auto& fr = p_.framing;
        if (has_header_) {
            w_.line("static gmsg::Result<std::size_t> encode(const " + m->name +
                    "& m, gmsg::mut_bytes out) noexcept { return encode(Header{}, m, out); }");
            w_.line("/// Encode with explicit header values (id/length fields are filled in automatically).");
            w_.open("static gmsg::Result<std::size_t> encode([[maybe_unused]] Header h, const " + m->name +
                    "& m, gmsg::mut_bytes out) noexcept {");
        } else {
            w_.open("static gmsg::Result<std::size_t> encode(const " + m->name + "& m, gmsg::mut_bytes out) noexcept {");
        }
        w_.line("gmsg::ByteWriter w(out);");
        if (p_.prefix_size) w_.line("if (auto e = w.write_bytes(prefix); !e) return gmsg::fail(e.error());");
        w_.line("[[maybe_unused]] const std::size_t header_at = w.position();");
        if (has_header_) {
            if (p_.message_type.kind == MessageTypeSpec::Kind::Field) {
                const auto& df = *p_.message_type.field;
                if (!df.const_value)
                    w_.line("h." + ident(df.name) + " = " + typed_literal(df, std::get<std::int64_t>(p_.type_ids[i])) + ";");
            }
            w_.line("if (auto e = h.write(w); !e) return gmsg::fail(e.error());");
        }
        w_.line("[[maybe_unused]] const std::size_t body_at = w.position();");
        w_.line("if (auto e = m.write(w); !e) return gmsg::fail(e.error());");
        w_.line("[[maybe_unused]] const std::size_t body_len = w.position() - body_at;");
        if (fr.type == FT::Fixed) {
            const auto body = *fr.size - (p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size);
            w_.line("if (body_len > " + sz(body) + ") " + fail("BadLength"));
            w_.line("if (auto e = w.fill(0, " + sz(body) + " - body_len); !e) return gmsg::fail(e.error());");
        }
        if (fr.type == FT::LengthPrefixed) {
            const auto n = prim_size(fr.length->prim);
            std::string covered;
            switch (fr.length_covers) {
                case FramingSpec::LengthCovers::Body:
                    covered = fr.length_counts_wire
                                  ? "body_len + gmsg::escape_overhead(gmsg::bytes_view(out.data() + body_at, body_len), framing_config.escape)"
                                  : "body_len";
                    break;
                case FramingSpec::LengthCovers::BodyChecksum:
                    covered = fr.length_counts_wire ? "body_wire + trailer_wire" : "body_len + trailer_size";
                    break;
                case FramingSpec::LengthCovers::HeaderBody: covered = "header_size + body_len"; break;
                case FramingSpec::LengthCovers::All: covered = "prefix_size + header_size + body_len + trailer_size"; break;
            }
            w_.open("{");
            if (fr.length_counts_wire && fr.length_covers == FramingSpec::LengthCovers::BodyChecksum) emit_wire_trailer_size();
            w_.line("const std::int64_t value = static_cast<std::int64_t>(" + covered + ") - (" + int_literal(fr.length_adjust) + ");");
            w_.line("if (value < 0 || static_cast<std::uint64_t>(value) > " + max_uint(n) + ") " + fail("BadLength"));
            w_.line("w.patch_uint(header_at + " + sz(*fr.length->static_offset) + ", static_cast<std::uint64_t>(value), " +
                    sz(n) + ", " + endian_expr(fr.length->endian) + ");");
            w_.close();
        }
        if (p_.checksum.present && !wire_cs_) {
            std::string covered;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: covered = "w.written().subspan(header_at)"; break;
                case ChecksumSpec::Covers::Body: covered = "w.written().subspan(body_at)"; break;
                case ChecksumSpec::Covers::All: covered = "w.written()"; break;
            }
            w_.line("const auto cs = static_cast<std::uint64_t>(" + checksum_call(covered) + ");");
            w_.line("if (auto e = w.write_uint(cs, trailer_size, " + endian_expr(p_.checksum.endian) +
                    "); !e) return gmsg::fail(e.error());");
        }
        w_.line("std::size_t total = w.position();");
        if (wire_cs_) {
            if (p_.checksum.covers == ChecksumSpec::Covers::Body)
                w_.line("const std::size_t body_wire = body_at + gmsg::escape_overhead(gmsg::bytes_view(out.data() + prefix_size, "
                        "body_at - prefix_size), framing_config.escape);");
            w_.line("auto esc = gmsg::escape_in_place(out, prefix_size, total - prefix_size, framing_config.escape);");
            w_.line("if (!esc) return gmsg::fail(esc.error());");
            w_.line("total = prefix_size + *esc;");
            std::string covered;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: covered = "gmsg::bytes_view(out.data() + prefix_size, total - prefix_size)"; break;
                case ChecksumSpec::Covers::Body: covered = "gmsg::bytes_view(out.data() + body_wire, total - body_wire)"; break;
                case ChecksumSpec::Covers::All: covered = "gmsg::bytes_view(out.data(), total)"; break;
            }
            w_.line("const auto cs = static_cast<std::uint64_t>(" + checksum_call(covered) + ");  // over the wire bytes");
            w_.line("if (out.size() - total < trailer_size) " + fail("BufferFull"));
            w_.line("gmsg::store_uint(out.data() + total, trailer_size, cs, " + endian_expr(p_.checksum.endian) + ");");
            w_.line("auto esc_cs = gmsg::escape_in_place(out, total, trailer_size, framing_config.escape);");
            w_.line("if (!esc_cs) return gmsg::fail(esc_cs.error());");
            w_.line("total += *esc_cs;");
        } else if (escape_) {
            w_.line("auto esc = gmsg::escape_in_place(out, prefix_size, total - prefix_size, framing_config.escape);");
            w_.line("if (!esc) return gmsg::fail(esc.error());");
            w_.line("total = prefix_size + *esc;");
        }
        if (p_.suffix_size) {
            w_.line("if (out.size() - total < suffix_size) " + fail("BufferFull"));
            w_.line("std::copy(suffix.begin(), suffix.end(), out.begin() + static_cast<std::ptrdiff_t>(total));");
            w_.line("total += suffix_size;");
        }
        w_.line("return total;");
        w_.close();
    }

    /// length.counts: wire + covers: body_checksum - the checksum only covers the body (checked in lowering),
    /// so it can be computed before the length is written to find how many of its bytes need escaping.
    void emit_wire_trailer_size() {
        w_.line("// the length counts the escaped body and the escaped checksum");
        w_.line("const std::size_t body_wire = body_len + gmsg::escape_overhead(gmsg::bytes_view(out.data() + body_at, body_len), framing_config.escape);");
        if (!p_.checksum.present) {
            w_.line("const std::size_t trailer_wire = 0;");
            return;
        }
        if (wire_cs_) {
            // over the escaped body: escape it in place, checksum it, and restore it
            w_.line("std::uint64_t cs0 = 0;");
            w_.open("{");
            w_.line("auto e = gmsg::escape_in_place(out, body_at, body_len, framing_config.escape);");
            w_.line("if (!e) return gmsg::fail(e.error());");
            w_.line("cs0 = static_cast<std::uint64_t>(" + checksum_call("gmsg::bytes_view(out.data() + body_at, *e)") + ");");
            w_.line("(void)gmsg::unescape_in_place(out.subspan(body_at, *e), framing_config.escape);");
            w_.close();
        } else {
            w_.line("const auto cs0 = static_cast<std::uint64_t>(" + checksum_call("gmsg::bytes_view(out.data() + body_at, body_len)") + ");");
        }
        w_.line("std::array<std::uint8_t, 8> cs_bytes{};");
        w_.line("gmsg::store_uint(cs_bytes.data(), trailer_size, cs0, " + endian_expr(p_.checksum.endian) + ");");
        w_.line("const std::size_t trailer_wire = trailer_size + gmsg::escape_overhead(gmsg::bytes_view(cs_bytes.data(), trailer_size), framing_config.escape);");
    }

    static std::string max_uint(std::size_t n) {
        return n >= 8 ? "UINT64_MAX" : sz((std::uint64_t{1} << (8 * n)) - 1) + "ull";
    }

    std::string checksum_call(const std::string& covered) const {
        bool fn = false;
        const auto impl = checksum_impl(p_.checksum, fn);
        return fn ? impl + "(" + covered + ")" : impl + "::compute(" + covered + ")";
    }

    void emit_encode_text(std::size_t i) {
        const auto* m = p_.messages[i];
        w_.open("static gmsg::Result<std::size_t> encode(const " + m->name + "& m, gmsg::mut_bytes out) noexcept {");
        w_.line("std::size_t pos = 0;");
        if (p_.prefix_size) {
            w_.line("if (out.size() < prefix_size) " + fail("BufferFull"));
            w_.line("std::copy(prefix.begin(), prefix.end(), out.begin());");
            w_.line("pos = prefix_size;");
        }
        w_.open("{");
        w_.line("gmsg::text::TextWriter w(out.subspan(pos), " + char_lit(text_sep(p_)) + ");");
        if (p_.message_type.kind == MessageTypeSpec::Kind::Token)
            w_.line("if (auto e = w.token(std::string_view(" + type_id_literal(i) + ")); !e) return gmsg::fail(e.error());");
        w_.line("if (auto e = m.write_text(w); !e) return gmsg::fail(e.error());");
        w_.line("pos += w.position();");
        w_.close();
        if (p_.checksum.present) {
            const auto n = p_.checksum.size;
            bool fn = false;
            const auto impl = checksum_impl(p_.checksum, fn);
            const auto covered = "gmsg::bytes_view(out.data() + prefix_size, pos - prefix_size)";
            const auto call = fn ? impl + "(" + covered + ")" : impl + "::compute(" + std::string(covered) + ")";
            w_.line("const auto cs = static_cast<std::uint64_t>(" + call + ");");
            w_.line("if (out.size() - pos < " + sz(1 + 2 * n) + ") " + fail("BufferFull"));
            w_.line("out[pos++] = static_cast<std::uint8_t>(" + char_lit(p_.checksum.separator) + ");");
            w_.open("for (int k = " + sz(n - 1) + "; k >= 0; --k) {");
            w_.line("const auto b = static_cast<unsigned>((cs >> (8 * k)) & 0xFFu);");
            w_.line("out[pos++] = static_cast<std::uint8_t>(gmsg::text::hex_digit(b >> 4));");
            w_.line("out[pos++] = static_cast<std::uint8_t>(gmsg::text::hex_digit(b & 0xFu));");
            w_.close();
        }
        if (p_.suffix_size) {
            w_.line("if (out.size() - pos < suffix_size) " + fail("BufferFull"));
            w_.line("std::copy(suffix.begin(), suffix.end(), out.begin() + static_cast<std::ptrdiff_t>(pos));");
            w_.line("pos += suffix_size;");
        }
        w_.line("return pos;");
        w_.close();
    }

    Ctx& c_;
    CodeWriter& w_;
    const Protocol& p_;
    bool text_ = false, has_header_ = false, escape_ = false, wire_cs_ = false, cs_first_ = false, stream_ = false,
         all_struct_ = false;
};

} // namespace

void emit_protocol(Ctx& c, const Protocol& p) {
    if (p.format == Format::Binary && !p.header->fields.empty()) emit_binary_message(c, *p.header);
    ProtocolEmitter(c, p).emit();
}

} // namespace gmpc::cpp
