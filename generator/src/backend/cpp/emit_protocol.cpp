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
std::string fail(const char* err) { return std::string("return gmp::fail(gmp::Error::") + err + ");"; }

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
    return "gmp::Pattern{{" + byte_list(v) + "}, " + sz(v.size()) + "}";
}

std::string escape_config(Ctx& c, const Protocol& p) {
    const auto& e = p.framing.escape;
    if (!e.enabled) return "gmp::EscapeConfig{}";
    std::string raw, code, reserved;
    std::size_t reserved_len = 0;
    for (std::size_t i = 0; i < e.map.size(); ++i) {
        raw += (i ? ", " : "") + hex_byte(e.map[i].first);
        code += (i ? ", " : "") + hex_byte(e.map[i].second);
    }
    if (e.map.empty()) {
        std::set<std::uint8_t> r;
        for (auto b : p.framing.sync) r.insert(b);
        for (auto b : p.framing.end) r.insert(b);
        r.erase(e.esc);
        if (r.size() > 4) c.diag.error(p.framing.loc, "escape: at most 4 distinct delimiter bytes are supported");
        for (auto b : r) {
            if (reserved_len == 4) break;
            reserved += (reserved_len ? ", " : "") + hex_byte(b);
            ++reserved_len;
        }
    }
    return "gmp::EscapeConfig{true, " + hex_byte(e.esc) + ", " + hex_byte(e.xor_mask) + ", " + sz(e.map.size()) + ", {" +
           raw + "}, {" + code + "}, {" + reserved + "}, " + sz(reserved_len) + "}";
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
        stream_ = p.framing.type != FT::None && p.max_frame > 0;
        all_struct_ = std::all_of(p.messages.begin(), p.messages.end(), [](const Message* m) { return m->want_struct; });
        all_view_ = std::all_of(p.messages.begin(), p.messages.end(), [](const Message* m) { return m->want_view; });
        if (!all_view_) c.diag.error(p.loc, "protocol '" + p.name + "': all messages need view access (used to select the message type)");
        if (text_)
            for (const auto* m : p.messages)
                if (m->separator != p.separator)
                    c.diag.error(m->loc, "message '" + m->name + "' separator differs from protocol '" + p.name + "'");
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
        emit_encode();
        w_.close("};");
        w_.line();
    }

private:
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
            w_.line("/// Incremental parser; Capacity must be >= max_frame.");
            w_.line("template <std::size_t Capacity = max_frame>");
            w_.line("using Stream = gmp::StreamParser<" + p_.name + ", Capacity>;");
        }
        w_.line();
    }

    void emit_framing() {
        const auto& fr = p_.framing;
        switch (fr.type) {
            case FT::None:
                w_.line("/// No framing: the whole buffer is one frame.");
                w_.line("static constexpr gmp::FrameScan scan(gmp::bytes_view b) noexcept { return gmp::FrameScan::frame(b.size(), b.size(), b.size()); }");
                break;
            case FT::Fixed:
                w_.line("static constexpr gmp::FixedFrameConfig framing_config{" + pattern(fr.sync) + ", " + sz(*fr.size) + "};");
                w_.line("static constexpr gmp::FrameScan scan(gmp::bytes_view b) noexcept { return gmp::scan_fixed(b, framing_config); }");
                break;
            case FT::LengthPrefixed: {
                const auto prefix = static_cast<std::int64_t>(p_.prefix_size);
                const auto header = static_cast<std::int64_t>(p_.header_size);
                const auto trailer = static_cast<std::int64_t>(p_.trailer_size);
                const auto suffix = static_cast<std::int64_t>(p_.suffix_size);
                std::int64_t add = fr.length_adjust + suffix;
                if (fr.length_covers == FramingSpec::LengthCovers::Body) add += prefix + header + trailer;
                else if (fr.length_covers == FramingSpec::LengthCovers::HeaderBody) add += prefix + trailer;
                w_.line("static constexpr gmp::LengthPrefixedConfig framing_config{" + pattern(fr.sync) + ", " +
                        sz(p_.length_field_offset) + ", " + sz(prim_size(fr.length->prim)) + ", " +
                        endian_expr(fr.length->endian) + ", " + int_literal(add) + ", " +
                        sz(p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size) + ", max_frame};");
                w_.line("static constexpr gmp::FrameScan scan(gmp::bytes_view b) noexcept { return gmp::scan_length_prefixed(b, framing_config); }");
                break;
            }
            case FT::Delimited:
                w_.line("static constexpr gmp::DelimitedConfig framing_config{" + pattern(fr.sync) + ", " + pattern(fr.end) +
                        ", " + escape_config(c_, p_) + ", max_frame};");
                w_.line("static constexpr gmp::FrameScan scan(gmp::bytes_view b) noexcept { return gmp::scan_delimited(b, framing_config); }");
                break;
            case FT::Custom: {
                const auto t = lang_value(fr.custom->lang, "type");
                w_.line("static_assert(gmp::Framer<" + t + ">, \"custom framer '" + fr.custom->name +
                        "' must provide static gmp::FrameScan scan(gmp::bytes_view)\");");
                w_.line("static gmp::FrameScan scan(gmp::bytes_view b) noexcept { return " + t + "::scan(b); }");
                break;
            }
        }
        w_.line();
    }

    void emit_frame_struct() {
        w_.line("/// A validated frame split into its parts (views into the input buffer).");
        w_.open("struct Frame {");
        if (has_header_) w_.line("HeaderView header{};");
        if (text_) w_.line("std::string_view type_id{};  // text: the first token");
        w_.line("gmp::bytes_view body{};");
        w_.close("};");
        w_.line();
    }

    std::string frame_param() const { return escape_ ? "gmp::mut_bytes frame" : "gmp::bytes_view frame"; }

    void emit_checksum_compare(const std::string& covered, const std::string& stored) {
        bool fn = false;
        const auto impl = checksum_impl(p_.checksum, fn);
        const auto call = fn ? impl + "(" + covered + ")" : impl + "::compute(" + covered + ")";
        w_.line("const auto computed = static_cast<std::uint64_t>(" + call + ");");
        w_.line("if (" + stored + " != computed) " + fail("BadChecksum"));
    }

    void emit_open() {
        w_.line("/// Validate one complete frame (as delimited by scan()) and split it into header and body.");
        if (escape_) w_.line("/// The frame is unescaped in place.");
        w_.open("[[nodiscard]] static gmp::Result<Frame> open(" + frame_param() + ") noexcept {");
        w_.line("if (frame.size() < prefix_size + header_size + trailer_size + suffix_size) " + fail("Truncated"));
        if (escape_) {
            w_.line("auto n = gmp::unescape_in_place(frame.subspan(prefix_size, frame.size() - prefix_size - suffix_size), framing_config.escape);");
            w_.line("if (!n) return gmp::fail(n.error());");
            w_.line("const gmp::bytes_view interior(frame.data() + prefix_size, *n);");
        } else {
            w_.line("const gmp::bytes_view interior = frame.subspan(prefix_size, frame.size() - prefix_size - suffix_size);");
        }
        if (text_) emit_open_text();
        else emit_open_binary();
        w_.close();
        w_.line();
    }

    void emit_open_binary() {
        w_.line("if (interior.size() < header_size + trailer_size) " + fail("Truncated"));
        if (p_.checksum.present) {
            std::string covered;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: covered = "interior.first(interior.size() - trailer_size)"; break;
                case ChecksumSpec::Covers::Body:
                    covered = "interior.subspan(header_size, interior.size() - header_size - trailer_size)";
                    break;
                case ChecksumSpec::Covers::All:
                    covered = "gmp::bytes_view(frame.data(), prefix_size + interior.size() - trailer_size)";
                    break;
            }
            w_.open("{");
            w_.line("const auto stored = gmp::load_uint(interior.data() + interior.size() - trailer_size, trailer_size, " +
                    endian_expr(p_.checksum.endian) + ");");
            emit_checksum_compare(covered, "stored");
            w_.close();
        }
        w_.line("Frame f;");
        if (has_header_) {
            w_.line("auto h = HeaderView::from(interior.first(header_size));");
            w_.line("if (!h) return gmp::fail(h.error());");
            w_.line("f.header = *h;");
        }
        w_.line("f.body = interior.subspan(header_size, interior.size() - header_size - trailer_size);");
        w_.line("return f;");
    }

    void emit_open_text() {
        w_.line("std::string_view text = gmp::as_string_view(interior);");
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
            w_.line("auto b = gmp::text::parse_hex_byte(hex.substr(2 * i, 2));");
            w_.line("if (!b) " + fail("BadChecksum"));
            w_.line("stored = (stored << 8) | *b;");
            w_.close();
            emit_checksum_compare("gmp::as_bytes(text.data(), star)", "stored");
            w_.line("text = text.substr(0, star);");
            w_.close();
        }
        w_.line("Frame f;");
        w_.line("const auto p = text.find(" + char_lit(text_sep(p_)) + ");");
        w_.line("f.type_id = text.substr(0, p);");
        w_.line("const std::string_view rest = p == std::string_view::npos ? std::string_view{} : text.substr(p + 1);");
        w_.line("f.body = gmp::as_bytes(rest.data(), rest.size());");
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
        w_.open("static gmp::Error visit_frame(const Frame& f, F&& fn) noexcept {");
        auto body = [&](std::size_t i) {
            const auto v = view_name(*p_.messages[i]);
            w_.line("auto v = " + v + "::from(f.body);");
            w_.line("if (!v) return v.error();");
            w_.line("fn(*v);");
            w_.line("return gmp::Error::None;");
        };
        switch (p_.message_type.kind) {
            case MessageTypeSpec::Kind::None:
                body(0);
                break;
            case MessageTypeSpec::Kind::Field: {
                w_.open("switch (gmp::to_int(f.header." + ident(p_.message_type.field->name) + "())) {");
                for (std::size_t i = 0; i < p_.messages.size(); ++i) {
                    w_.open("case " + type_id_literal(i) + ": {  // " + p_.messages[i]->name);
                    body(i);
                    w_.close();
                }
                w_.line("default: return gmp::Error::UnknownType;");
                w_.close();
                break;
            }
            case MessageTypeSpec::Kind::Token: {
                for (std::size_t i = 0; i < p_.messages.size(); ++i) {
                    w_.open("if (f.type_id == " + type_id_literal(i) + ") {");
                    body(i);
                    w_.close();
                }
                w_.line("return gmp::Error::UnknownType;");
                break;
            }
        }
        w_.close();
        w_.line();
        w_.open("[[nodiscard]] static gmp::Result<AnyView> select_message(const Frame& f) noexcept {");
        w_.line("gmp::Result<AnyView> out = gmp::fail(gmp::Error::UnknownType);");
        w_.line("const gmp::Error e = visit_frame(f, [&](const auto& v) { out = AnyView(v); });");
        w_.line("if (e != gmp::Error::None) return gmp::fail(e);");
        w_.line("return out;");
        w_.close();
        w_.line();
    }

    void emit_parse() {
        const auto buf = escape_ ? std::string("gmp::mut_bytes buffer") : std::string("gmp::bytes_view buffer");
        w_.line("/// Parse one frame located at the start of `buffer` (whole-buffer / datagram mode).");
        w_.open("[[nodiscard]] static gmp::Result<AnyView> parse(" + buf + ") noexcept {");
        w_.line("const gmp::FrameScan s = scan(buffer);");
        w_.line("if (s.kind == gmp::FrameScan::Kind::need_more) " + fail("Truncated"));
        w_.line("if (s.kind == gmp::FrameScan::Kind::skip) return gmp::fail(s.error == gmp::Error::None ? gmp::Error::BadSync : s.error);");
        w_.line("auto f = open(buffer.first(s.size));");
        w_.line("if (!f) return gmp::fail(f.error());");
        w_.line("return select_message(*f);");
        w_.close();
        if (all_struct_) {
            w_.line("/// Parse one frame and decode it into the eager message struct.");
            w_.open("[[nodiscard]] static gmp::Result<AnyMessage> decode(" + buf + ") noexcept {");
            w_.line("auto v = parse(buffer);");
            w_.line("if (!v) return gmp::fail(v.error());");
            w_.open("return std::visit([](const auto& view) -> gmp::Result<AnyMessage> {");
            w_.line("auto m = view.to_struct();");
            w_.line("if (!m) return gmp::fail(m.error());");
            w_.line("return AnyMessage(std::move(*m));");
            w_.close("}, *v);");
            w_.close();
        }
        w_.line();
    }

    void emit_process() {
        w_.line("/// StreamParser hook: validate a frame, select its message type and deliver it to `handler`.");
        w_.line("/// The handler is called as handler(header_view, message_view) or handler(message_view),");
        w_.line("/// whichever it accepts; message types it does not accept are ignored.");
        w_.line("template <class H>");
        w_.open("static gmp::Error process(gmp::mut_bytes frame, H& handler) {");
        w_.line("auto f = open(frame);");
        w_.line("if (!f) return f.error();");
        w_.open("return visit_frame(*f, [&](const auto& v) {");
        w_.line("using V = std::decay_t<decltype(v)>;");
        if (has_header_) {
            w_.line("if constexpr (std::invocable<H&, const HeaderView&, const V&>) handler(f->header, v);");
            w_.line("else if constexpr (std::invocable<H&, const V&>) handler(v);");
        } else {
            w_.line("if constexpr (std::invocable<H&, const V&>) handler(v);");
        }
        w_.close("});");
        w_.close();
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
            w_.open("static gmp::Result<std::size_t> encode(const AnyMessage& m, gmp::mut_bytes out) noexcept {");
            w_.line("return std::visit([&](const auto& x) { return encode(x, out); }, m);");
            w_.close();
        }
    }

    void emit_encode_binary(std::size_t i) {
        const auto* m = p_.messages[i];
        const auto& fr = p_.framing;
        if (has_header_) {
            w_.line("static gmp::Result<std::size_t> encode(const " + m->name +
                    "& m, gmp::mut_bytes out) noexcept { return encode(Header{}, m, out); }");
            w_.line("/// Encode with explicit header values (id/length fields are filled in automatically).");
            w_.open("static gmp::Result<std::size_t> encode([[maybe_unused]] Header h, const " + m->name +
                    "& m, gmp::mut_bytes out) noexcept {");
        } else {
            w_.open("static gmp::Result<std::size_t> encode(const " + m->name + "& m, gmp::mut_bytes out) noexcept {");
        }
        w_.line("gmp::ByteWriter w(out);");
        if (p_.prefix_size) w_.line("if (auto e = w.write_bytes(prefix); !e) return gmp::fail(e.error());");
        w_.line("[[maybe_unused]] const std::size_t header_at = w.position();");
        if (has_header_) {
            if (p_.message_type.kind == MessageTypeSpec::Kind::Field) {
                const auto& df = *p_.message_type.field;
                if (!df.const_value)
                    w_.line("h." + ident(df.name) + " = " + typed_literal(df, std::get<std::int64_t>(p_.type_ids[i])) + ";");
            }
            w_.line("if (auto e = h.write(w); !e) return gmp::fail(e.error());");
        }
        w_.line("[[maybe_unused]] const std::size_t body_at = w.position();");
        w_.line("if (auto e = m.write(w); !e) return gmp::fail(e.error());");
        w_.line("[[maybe_unused]] const std::size_t body_len = w.position() - body_at;");
        if (fr.type == FT::Fixed) {
            const auto body = *fr.size - (p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size);
            w_.line("if (body_len > " + sz(body) + ") " + fail("BadLength"));
            w_.line("if (auto e = w.fill(0, " + sz(body) + " - body_len); !e) return gmp::fail(e.error());");
        }
        if (fr.type == FT::LengthPrefixed) {
            const auto n = prim_size(fr.length->prim);
            std::string covered;
            switch (fr.length_covers) {
                case FramingSpec::LengthCovers::Body: covered = "body_len"; break;
                case FramingSpec::LengthCovers::HeaderBody: covered = "header_size + body_len"; break;
                case FramingSpec::LengthCovers::All: covered = "prefix_size + header_size + body_len + trailer_size"; break;
            }
            const std::string maxv = n >= 8 ? "UINT64_MAX" : sz((std::uint64_t{1} << (8 * n)) - 1) + "ull";
            w_.open("{");
            w_.line("const std::int64_t value = static_cast<std::int64_t>(" + covered + ") - (" + int_literal(fr.length_adjust) + ");");
            w_.line("if (value < 0 || static_cast<std::uint64_t>(value) > " + maxv + ") " + fail("BadLength"));
            w_.line("w.patch_uint(header_at + " + sz(*fr.length->static_offset) + ", static_cast<std::uint64_t>(value), " +
                    sz(n) + ", " + endian_expr(fr.length->endian) + ");");
            w_.close();
        }
        if (p_.checksum.present) {
            std::string covered;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: covered = "w.written().subspan(header_at)"; break;
                case ChecksumSpec::Covers::Body: covered = "w.written().subspan(body_at)"; break;
                case ChecksumSpec::Covers::All: covered = "w.written()"; break;
            }
            bool fn = false;
            const auto impl = checksum_impl(p_.checksum, fn);
            const auto call = fn ? impl + "(" + covered + ")" : impl + "::compute(" + covered + ")";
            w_.line("const auto cs = static_cast<std::uint64_t>(" + call + ");");
            w_.line("if (auto e = w.write_uint(cs, trailer_size, " + endian_expr(p_.checksum.endian) +
                    "); !e) return gmp::fail(e.error());");
        }
        w_.line("std::size_t total = w.position();");
        if (escape_) {
            w_.line("auto esc = gmp::escape_in_place(out, prefix_size, total - prefix_size, framing_config.escape);");
            w_.line("if (!esc) return gmp::fail(esc.error());");
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

    void emit_encode_text(std::size_t i) {
        const auto* m = p_.messages[i];
        w_.open("static gmp::Result<std::size_t> encode(const " + m->name + "& m, gmp::mut_bytes out) noexcept {");
        w_.line("std::size_t pos = 0;");
        if (p_.prefix_size) {
            w_.line("if (out.size() < prefix_size) " + fail("BufferFull"));
            w_.line("std::copy(prefix.begin(), prefix.end(), out.begin());");
            w_.line("pos = prefix_size;");
        }
        w_.open("{");
        w_.line("gmp::text::TextWriter w(out.subspan(pos), " + char_lit(text_sep(p_)) + ");");
        if (p_.message_type.kind == MessageTypeSpec::Kind::Token)
            w_.line("if (auto e = w.token(std::string_view(" + type_id_literal(i) + ")); !e) return gmp::fail(e.error());");
        w_.line("if (auto e = m.write_text(w); !e) return gmp::fail(e.error());");
        w_.line("pos += w.position();");
        w_.close();
        if (p_.checksum.present) {
            const auto n = p_.checksum.size;
            bool fn = false;
            const auto impl = checksum_impl(p_.checksum, fn);
            const auto covered = "gmp::bytes_view(out.data() + prefix_size, pos - prefix_size)";
            const auto call = fn ? impl + "(" + covered + ")" : impl + "::compute(" + std::string(covered) + ")";
            w_.line("const auto cs = static_cast<std::uint64_t>(" + call + ");");
            w_.line("if (out.size() - pos < " + sz(1 + 2 * n) + ") " + fail("BufferFull"));
            w_.line("out[pos++] = static_cast<std::uint8_t>(" + char_lit(p_.checksum.separator) + ");");
            w_.open("for (int k = " + sz(n - 1) + "; k >= 0; --k) {");
            w_.line("const auto b = static_cast<unsigned>((cs >> (8 * k)) & 0xFFu);");
            w_.line("out[pos++] = static_cast<std::uint8_t>(gmp::text::hex_digit(b >> 4));");
            w_.line("out[pos++] = static_cast<std::uint8_t>(gmp::text::hex_digit(b & 0xFu));");
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
    bool text_ = false, has_header_ = false, escape_ = false, stream_ = false, all_struct_ = false, all_view_ = false;
};

} // namespace

void emit_protocol(Ctx& c, const Protocol& p) {
    if (p.format == Format::Binary && !p.header->fields.empty()) emit_binary_message(c, *p.header);
    ProtocolEmitter(c, p).emit();
}

} // namespace gmpc::cpp
