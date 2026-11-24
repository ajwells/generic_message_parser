// Python emission for protocols: a class with the framing configuration and static methods open / select / process /
// parse / decode, encode and a stream factory. Semantics follow backend/js/emit_protocol.cpp.

#include <algorithm>
#include <string>

#include "backend/py/py_common.hpp"

namespace gmpc::py {

namespace {

using namespace ir;
using FT = FramingSpec::Type;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string fail(const char* err) { return std::string("gmsg.fail(Err.") + err + ")"; }

std::string int_list(const std::vector<std::uint8_t>& v) {
    std::string s;
    for (auto b : v) s += (s.empty() ? "" : ", ") + std::to_string(b);
    return "[" + s + "]";
}

std::string escape_config(const Protocol& p) {
    const auto& e = p.framing.escape;
    if (!e.enabled) return "gmsg.NO_ESCAPE";
    std::vector<std::uint8_t> raw, code;
    for (const auto& [r, c] : e.table) {
        if (e.code == EscapeSpec::Code::Xor && r == e.esc) continue;
        raw.push_back(r);
        code.push_back(c);
    }
    switch (e.code) {
        case EscapeSpec::Code::Xor: return "gmsg.xor_escape(" + sz(e.esc) + ", " + sz(e.xor_mask) + ", " + int_list(raw) + ")";
        case EscapeSpec::Code::Index: return "gmsg.index_escape(" + sz(e.esc) + ", " + int_list(raw) + ", " + sz(e.base) + ")";
        case EscapeSpec::Code::Map: return "gmsg.map_escape(" + sz(e.esc) + ", " + int_list(raw) + ", " + int_list(code) + ")";
    }
    return "gmsg.NO_ESCAPE";
}

class ProtocolEmitter {
public:
    ProtocolEmitter(Ctx& c, const Protocol& p) : c_(c), w_(c.w), p_(p) {
        text_ = p.format == Format::Text;
        has_header_ = !text_ && !p.header->fields.empty();
        escape_ = p.framing.escape.enabled;
        wire_cs_ = escape_ && p.checksum.present && p.checksum.over_wire && !text_;
        stream_ = p.framing.type != FT::None && p.max_frame > 0;
        all_struct_ = std::all_of(p.messages.begin(), p.messages.end(), [](const Message* m) { return m->want_struct; });
        n_ = p.name;
        from_ = text_ ? ".from_text" : ".from_bytes";
    }

    void emit() {
        w_.open("class " + n_ + ":");
        doc(w_, p_.doc);
        emit_constants();
        emit_framing();
        emit_open();
        emit_select();
        emit_parse();
        emit_encode();
        w_.dedent();
        w_.line();
        w_.line();
        c_.exports.push_back(n_);
    }

private:
    void method(const std::string& signature, const std::string& docstring = {}) {
        w_.line();
        w_.line("@staticmethod");
        w_.open("def " + signature + ":");
        if (!docstring.empty()) w_.line("\"\"\"" + docstring + "\"\"\"");
    }

    void emit_constants() {
        w_.line("max_frame = " + sz(p_.max_frame));
        w_.line("prefix_size = " + sz(p_.prefix_size) + "  # sync / start delimiter");
        w_.line("header_size = " + sz(p_.header_size));
        w_.line("trailer_size = " + sz(p_.trailer_size) + "  # binary checksum");
        w_.line("suffix_size = " + sz(p_.suffix_size) + "  # end delimiter");
        w_.line("prefix = " + bytes_literal(p_.framing.sync));
        w_.line("suffix = " + bytes_literal(p_.framing.end));
        if (has_header_) {
            w_.line("Header = " + p_.header->name);
            w_.line("HeaderView = " + view_name(*p_.header));
        }
        std::string views, structs;
        for (const auto* m : p_.messages) {
            views += view_name(*m) + ", ";
            if (m->want_struct) structs += m->name + ", ";
        }
        w_.line("# The message views of this protocol.");
        w_.line("views = (" + views.substr(0, views.size() - (p_.messages.size() > 1 ? 2 : 1)) + ")");
        if (all_struct_) w_.line("messages = (" + structs.substr(0, structs.size() - (p_.messages.size() > 1 ? 2 : 1)) + ")");
        w_.line("escape = " + escape_config(p_));
    }

    void emit_framing() {
        const auto& fr = p_.framing;
        switch (fr.type) {
            case FT::None:
                method("scan(b) -> gmsg.FrameScan", "No framing: the whole buffer is one frame.");
                w_.line("return gmsg.FrameScan.frame(len(b), len(b), len(b))");
                w_.dedent();
                break;
            case FT::Fixed:
                w_.line("framing = gmsg.FixedConfig(sync=prefix, frame_size=" + sz(*fr.size) + ", escape=escape, suffix_len=" +
                        (escape_ ? sz(p_.suffix_size) : "0") + ")");
                method("scan(b) -> gmsg.FrameScan");
                w_.line("return gmsg.scan_fixed(b, " + n_ + ".framing)");
                w_.dedent();
                break;
            case FT::LengthPrefixed: {
                const auto prefix = static_cast<std::int64_t>(p_.prefix_size);
                const auto header = static_cast<std::int64_t>(p_.header_size);
                const auto trailer = static_cast<std::int64_t>(p_.trailer_size);
                const auto suffix = static_cast<std::int64_t>(p_.suffix_size);
                std::int64_t add = fr.length_adjust + suffix;
                if (fr.length_counts_wire) add = fr.length_adjust;
                else if (fr.length_covers == FramingSpec::LengthCovers::Body) add += prefix + header + trailer;
                else if (fr.length_covers == FramingSpec::LengthCovers::BodyChecksum) add += prefix + header;
                else if (fr.length_covers == FramingSpec::LengthCovers::HeaderBody) add += prefix + trailer;
                std::string cfg = "gmsg.LengthPrefixedConfig(sync=prefix, length_offset=" + sz(p_.length_field_offset) +
                                  ", length_size=" + sz(prim_size(fr.length->prim)) + ", length_endian=" + endian(fr.length->endian) +
                                  ", add=" + std::to_string(add) + ", min_frame=" +
                                  sz(p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size) +
                                  ", max_frame=max_frame, escape=escape, suffix_len=" + (escape_ ? sz(p_.suffix_size) : "0");
                if (fr.length_counts_wire)
                    cfg += ", counts_wire=True, header_len=" + sz(p_.header_size) + ", trailer_len=" + sz(p_.trailer_size) +
                           ", wire_includes_trailer=" +
                           (fr.length_covers == FramingSpec::LengthCovers::BodyChecksum ? "True" : "False");
                w_.line("framing = " + cfg + ")");
                if (p_.size_checks.empty()) {
                    method("scan(b) -> gmsg.FrameScan");
                    w_.line("return gmsg.scan_length_prefixed(b, " + n_ + ".framing)");
                    w_.dedent();
                    break;
                }
                emit_checked_scan();
                break;
            }
            case FT::Delimited:
                w_.line("framing = gmsg.DelimitedConfig(start=prefix, end=suffix, escape=escape, max_frame=max_frame)");
                method("scan(b) -> gmsg.FrameScan");
                w_.line("return gmsg.scan_delimited(b, " + n_ + ".framing)");
                w_.dedent();
                break;
            case FT::Custom:
                method("scan(b) -> gmsg.FrameScan");
                w_.line("return " + ext_fn(c_, fr.custom->lang, "scan") + "(b)");
                w_.dedent();
                break;
        }
    }

    std::string checksum_of(const std::string& data, const std::string& start, const std::string& end) {
        return checksum_value(c_, p_.checksum, data, start, end);
    }

    void emit_open() {
        method("open(frame) -> gmsg.Opened",
               std::string("Validate one complete frame (as found by scan()) and split it: ") +
                   (text_ ? "type token and record text." : "header view and body."));
        const auto P = sz(p_.prefix_size), S = sz(p_.suffix_size);
        w_.line("frame = gmsg.to_bytes(frame)");
        w_.line("if len(frame) < " + sz(p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size) + ": " + fail("TRUNCATED"));
        if (wire_cs_) emit_wire_checksum();
        const auto inner = "frame[" + P + ":len(frame) - " + S + "]";
        if (escape_) {
            w_.line("fr = bytes(frame[:" + P + "]) + gmsg.unescape(" + inner + ", " + n_ + ".escape)  # unescaped copy");
            w_.line("interior = memoryview(fr)[" + P + ":]");
        } else {
            w_.line("fr = frame");
            w_.line("interior = memoryview(frame)[" + P + ":len(frame) - " + S + "]");
        }
        if (text_) emit_open_text();
        else emit_open_binary();
        w_.dedent();
    }

    void emit_wire_checksum() {
        const auto H = sz(p_.header_size), T = sz(p_.trailer_size), P = sz(p_.prefix_size);
        w_.line("# checksum.over: wire");
        w_.line("wire = frame[" + P + ":len(frame) - " + sz(p_.suffix_size) + "]");
        w_.line("un = gmsg.unescaped_size(wire, " + n_ + ".escape)");
        w_.line("if un < " + H + " + " + T + ": " + fail("TRUNCATED"));
        w_.line("t = gmsg.walk_escaped(wire, 0, un - " + T + ", " + n_ + ".escape)");
        w_.line("if t.kind != 'done': gmsg.fail(t.error if t.kind == 'bad' else Err.TRUNCATED)");
        switch (p_.checksum.covers) {
            case ChecksumSpec::Covers::HeaderBody: w_.line("computed = " + checksum_of("wire", "0", "t.wire")); break;
            case ChecksumSpec::Covers::Body:
                w_.line("h = gmsg.walk_escaped(wire, 0, " + H + ", " + n_ + ".escape)");
                w_.line("computed = " + checksum_of("wire", "h.wire", "t.wire"));
                break;
            case ChecksumSpec::Covers::All: w_.line("computed = " + checksum_of("frame", "0", P + " + t.wire")); break;
        }
    }

    void emit_open_binary() {
        const auto H = sz(p_.header_size), T = sz(p_.trailer_size), P = sz(p_.prefix_size);
        w_.line("if len(interior) < " + H + " + " + T + ": " + fail("TRUNCATED"));
        if (p_.checksum.present) {
            w_.line("stored = gmsg.load_uint(interior, len(interior) - " + T + ", " + T + ", " + endian(p_.checksum.endian) + ")");
            if (!wire_cs_) {
                std::string computed;
                switch (p_.checksum.covers) {
                    case ChecksumSpec::Covers::HeaderBody: computed = checksum_of("interior", "0", "len(interior) - " + T); break;
                    case ChecksumSpec::Covers::Body: computed = checksum_of("interior", H, "len(interior) - " + T); break;
                    case ChecksumSpec::Covers::All: computed = checksum_of("fr", "0", P + " + len(interior) - " + T); break;
                }
                w_.line("computed = " + computed);
            }
            w_.line("if stored != computed: " + fail("BAD_CHECKSUM"));
        }
        const auto body = "interior[" + H + ":len(interior) - " + T + "]";
        if (has_header_) w_.line("return gmsg.Opened(" + body + ", " + view_name(*p_.header) + ".from_bytes(interior[:" + H + "]))");
        else w_.line("return gmsg.Opened(" + body + ")");
    }

    void emit_open_text() {
        w_.line("text = gmsg.latin1_string(interior)");
        if (p_.checksum.present) {
            const auto n = p_.checksum.size;
            w_.line("star = text.rfind(" + string_literal(std::string(1, p_.checksum.separator)) + ")");
            if (p_.checksum.required) w_.line("if star < 0: " + fail("BAD_CHECKSUM"));
            else w_.open("if star >= 0:");
            w_.line("stored = gmsg.text.hex_value(text[star + 1:], " + sz(n) + ")");
            w_.line("if stored is None or stored != " + checksum_of("interior", "0", "star") + ": " + fail("BAD_CHECKSUM"));
            w_.line("text = text[:star]");
            if (!p_.checksum.required) w_.dedent();
        }
        w_.line("sp = text.find(" + string_literal(std::string(1, p_.separator)) + ")");
        w_.line("return gmsg.Opened('' if sp < 0 else text[sp + 1:], None, text if sp < 0 else text[:sp])");
    }

    static std::string size_fail(const SizeCheck& c) {
        std::string s;
        if (c.min > 0) s = "body < " + std::to_string(c.min);
        if (c.max) s += (s.empty() ? "" : " or ") + std::string("body > ") + std::to_string(*c.max);
        return s;
    }

    /// length_prefixed scan that also rejects a length the frame's message type cannot have, as soon as the header has
    /// arrived (C++: the generated scan()).
    void emit_checked_scan() {
        method("scan(b) -> gmsg.FrameScan",
               std::string("Scan for a frame; a length the message type cannot have (") +
                   (p_.exact_size ? "shorter or longer than the message" : "shorter than the message") +
                   ") is rejected with BadLength as soon as the header has arrived.");
        w_.line("s = gmsg.scan_length_prefixed(b, " + n_ + ".framing)");
        w_.line("if s.kind == 'skip': return s");
        w_.line("h = gmsg.read_frame_header(b, " + sz(p_.prefix_size) + ", " + n_ + ".escape, " + sz(p_.header_size) + ")");
        w_.line("if h is None: return s");
        w_.line("hv = " + view_name(*p_.header) + ".unchecked(h)");
        w_.line("body = int(hv." + ident(p_.framing.length->name) + ") + " + std::to_string(p_.length_to_body));
        w_.line("bad = gmsg.FrameScan.skip(1, Err.BAD_LENGTH)");
        if (p_.message_type.kind == MessageTypeSpec::Kind::None) {
            w_.line("if " + size_fail(p_.size_checks.front()) + ": return bad");
        } else {
            w_.line("tid = gmsg.to_int(hv." + ident(p_.message_type.field->name) + ")");
            for (const auto& c : p_.size_checks)
                w_.line("if tid == " + type_id_literal(c.message) + " and (" + size_fail(c) + "): return bad  # " +
                        p_.messages[c.message]->name);
        }
        w_.line("return s");
        w_.dedent();
    }

    std::string type_id_literal(std::size_t i) const {
        const auto& id = p_.type_ids[i];
        if (auto* s = std::get_if<std::string>(&id)) return string_literal(*s);
        if (auto* n = std::get_if<std::int64_t>(&id)) return std::to_string(*n);
        return "";
    }

    void emit_select() {
        method("select(f: gmsg.Opened)", "The message view selected by the frame's type id. Raises GmpError (UnknownType, ...).");
        if (p_.exact_size) {
            w_.line("v = " + n_ + ".select_view(f)");
            w_.line("if gmsg.extra_of(v) != 0: " + fail("BAD_LENGTH") + "  # exact_size");
            w_.line("return v");
            w_.dedent();
            method("select_view(f: gmsg.Opened)");
        }
        switch (p_.message_type.kind) {
            case MessageTypeSpec::Kind::None:
                w_.line("return " + view_name(*p_.messages[0]) + from_ + "(f.body)");
                break;
            case MessageTypeSpec::Kind::Field:
                w_.line("tid = gmsg.to_int(f.header." + ident(p_.message_type.field->name) + ")");
                for (std::size_t i = 0; i < p_.messages.size(); ++i)
                    w_.line("if tid == " + type_id_literal(i) + ": return " + view_name(*p_.messages[i]) + from_ + "(f.body)");
                w_.line(fail("UNKNOWN_TYPE"));
                break;
            case MessageTypeSpec::Kind::Token:
                for (std::size_t i = 0; i < p_.messages.size(); ++i)
                    w_.line("if f.type_id == " + type_id_literal(i) + ": return " + view_name(*p_.messages[i]) + from_ + "(f.body)");
                w_.line(fail("UNKNOWN_TYPE"));
                break;
        }
        w_.dedent();
        method("process(frame) -> gmsg.Received", "gmsg.StreamParser hook: the message and header views of a complete frame.");
        w_.line("f = " + n_ + ".open(frame)");
        w_.line("return gmsg.Received(" + n_ + ".select(f), f.header)");
        w_.dedent();
    }

    void emit_parse() {
        method("parse(buffer)", "Parse one frame at the start of buffer (whole-buffer / datagram mode): the message view.");
        w_.line("buffer = gmsg.to_bytes(buffer)");
        w_.line("s = " + n_ + ".scan(buffer)");
        w_.line("if s.kind == 'need_more': " + fail("TRUNCATED"));
        w_.line("if s.kind == 'skip': gmsg.fail(Err.BAD_SYNC if s.error == Err.NONE else s.error)");
        w_.line("return " + n_ + ".select(" + n_ + ".open(buffer[:s.size]))");
        w_.dedent();
        if (all_struct_) {
            method("decode(buffer)", "Parse one frame and decode it into its message dataclass.");
            w_.line("return " + n_ + ".parse(buffer).to_struct()");
            w_.dedent();
        }
        if (stream_) {
            method("stream(capacity: int | None = None) -> gmsg.StreamParser", "A stream parser for this protocol (default capacity: max_frame).");
            w_.line("return gmsg.StreamParser(" + n_ + ", capacity)");
            w_.dedent();
            if (all_struct_) {
                method("endpoint(source: object = None, sink: object = None, **options: object) -> gmsg.Endpoint",
                       "The stream joined to a byte source and sink (blocking; options: clock, idle, idle_timeout, send_policy, ...).");
                w_.line("return gmsg.Endpoint(" + n_ + ", source, sink, **options)");
                w_.dedent();
                method("async_endpoint(reader: object = None, writer: object = None, **options: object) -> gmsg.AsyncEndpoint",
                       "The stream joined to an asyncio reader and writer (options: idle_timeout, send_policy, ...).");
                w_.line("return gmsg.AsyncEndpoint(" + n_ + ", reader, writer, **options)");
                w_.dedent();
            }
        }
    }

    // ---- encode ------------------------------------------------------------------------------------
    void emit_encode() {
        bool any = false;
        for (const auto* m : p_.messages) any = any || m->want_struct;
        if (!any) return;
        const std::string hdr = has_header_ ? ", header=None" : "";
        method("encode(m" + hdr + ") -> bytes",
               std::string("Encode a message of this protocol") +
                   (has_header_ ? " (optionally with header values: a dict or a header instance; type id and length are filled in)" : "") +
                   " to a frame.");
        for (std::size_t i = 0; i < p_.messages.size(); ++i) {
            const auto* m = p_.messages[i];
            if (!m->want_struct) continue;
            w_.line("if isinstance(m, " + m->name + "): return " + n_ + "._encode" + sz(i) + "(m" + (has_header_ ? ", header" : "") + ")");
        }
        w_.line("gmsg.fail(Err.INVALID_VALUE, 'not a message of " + n_ + " (use the message classes)')");
        w_.dedent();
        for (std::size_t i = 0; i < p_.messages.size(); ++i) {
            if (!p_.messages[i]->want_struct) continue;
            if (text_) emit_encode_text(i);
            else emit_encode_binary(i);
        }
    }

    void emit_encode_binary(std::size_t i) {
        const auto* m = p_.messages[i];
        const auto& fr = p_.framing;
        const auto P = sz(p_.prefix_size), H = sz(p_.header_size), T = sz(p_.trailer_size);
        method("_encode" + sz(i) + "(m: " + m->name + (has_header_ ? ", header" : "") + ") -> bytes");
        w_.line("w = gmsg.Writer()");
        if (p_.prefix_size) w_.line("w.put(" + n_ + ".prefix)");
        w_.line("header_at = w.pos");
        if (has_header_) {
            const auto& hm = *p_.header;
            w_.line("h = gmsg.build(" + hm.name + ", header)");
            if (p_.message_type.kind == MessageTypeSpec::Kind::Field) {
                const auto& df = *p_.message_type.field;
                if (!df.const_value)
                    w_.line("h." + ident(df.name) + " = " + enum_of(df, typed_literal(df, std::get<std::int64_t>(p_.type_ids[i]))));
            }
            w_.line(hm.name + ".write(w, h)");
        }
        w_.line("body_at = w.pos");
        w_.line(m->name + ".write(w, m)");
        w_.line("body_len = w.pos - body_at");
        if (fr.type == FT::Fixed) {
            const auto body = *fr.size - (p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size);
            w_.line("if body_len > " + sz(body) + ": " + fail("BAD_LENGTH"));
            w_.line("w.zeros(" + sz(body) + " - body_len)");
        }
        if (fr.type == FT::LengthPrefixed) {
            const auto n = prim_size(fr.length->prim);
            std::string covered;
            switch (fr.length_covers) {
                case FramingSpec::LengthCovers::Body:
                    covered = fr.length_counts_wire ? "body_len + gmsg.escape_overhead(w.buf, " + n_ + ".escape, body_at, body_at + body_len)" : "body_len";
                    break;
                case FramingSpec::LengthCovers::BodyChecksum:
                    covered = fr.length_counts_wire ? "body_wire + trailer_wire" : "body_len + " + T;
                    break;
                case FramingSpec::LengthCovers::HeaderBody: covered = H + " + body_len"; break;
                case FramingSpec::LengthCovers::All: covered = P + " + " + H + " + body_len + " + T; break;
            }
            if (fr.length_counts_wire && fr.length_covers == FramingSpec::LengthCovers::BodyChecksum) emit_wire_trailer_size();
            const auto max = n >= 8 ? std::string("18446744073709551615") : std::to_string((std::uint64_t{1} << (8 * n)) - 1);
            w_.line("value = " + covered + " - (" + std::to_string(fr.length_adjust) + ")");
            w_.line("if value < 0 or value > " + max + ": " + fail("BAD_LENGTH"));
            w_.line("w.patch_uint(header_at + " + sz(*fr.length->static_offset) + ", value, " + sz(n) + ", " + endian(fr.length->endian) + ")");
        }
        if (p_.checksum.present && !wire_cs_) {
            std::string start;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: start = "header_at"; break;
                case ChecksumSpec::Covers::Body: start = "body_at"; break;
                case ChecksumSpec::Covers::All: start = "0"; break;
            }
            w_.line("w.uint(" + checksum_of("w.buf", start, "w.pos") + ", " + T + ", " + endian(p_.checksum.endian) + ")");
        }
        w_.line("out = w.to_bytes()");
        if (wire_cs_) {
            if (p_.checksum.covers == ChecksumSpec::Covers::Body)
                w_.line("body_wire = body_at + gmsg.escape_overhead(out, " + n_ + ".escape, " + P + ", body_at)");
            w_.line("out = gmsg.escape_range(out, " + P + ", len(out) - " + P + ", " + n_ + ".escape)");
            std::string cs;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: cs = checksum_of("out", P, "len(out)"); break;
                case ChecksumSpec::Covers::Body: cs = checksum_of("out", "body_wire", "len(out)"); break;
                case ChecksumSpec::Covers::All: cs = checksum_of("out", "0", "len(out)"); break;
            }
            w_.line("cs = bytearray(" + T + ")");
            w_.line("gmsg.store_uint(cs, 0, " + T + ", " + cs + ", " + endian(p_.checksum.endian) + ")  # over the wire bytes");
            w_.line("out += gmsg.escape(cs, " + n_ + ".escape)");
        } else if (escape_) {
            w_.line("out = gmsg.escape_range(out, " + P + ", len(out) - " + P + ", " + n_ + ".escape)");
        }
        if (p_.suffix_size) w_.line("out += " + n_ + ".suffix");
        w_.line("return out");
        w_.dedent();
    }

    void emit_wire_trailer_size() {
        const auto T = sz(p_.trailer_size);
        w_.line("# the length counts the escaped body and the escaped checksum");
        w_.line("body_wire = body_len + gmsg.escape_overhead(w.buf, " + n_ + ".escape, body_at, body_at + body_len)");
        if (!p_.checksum.present) {
            w_.line("trailer_wire = 0");
            return;
        }
        if (wire_cs_) {
            w_.line("eb = gmsg.escape(w.buf[body_at:body_at + body_len], " + n_ + ".escape)");
            w_.line("cs0 = " + checksum_of("eb", "0", "len(eb)"));
        } else {
            w_.line("cs0 = " + checksum_of("w.buf", "body_at", "body_at + body_len"));
        }
        w_.line("csb = bytearray(" + T + ")");
        w_.line("gmsg.store_uint(csb, 0, " + T + ", cs0, " + endian(p_.checksum.endian) + ")");
        w_.line("trailer_wire = " + T + " + gmsg.escape_overhead(csb, " + n_ + ".escape)");
    }

    void emit_encode_text(std::size_t i) {
        const auto* m = p_.messages[i];
        method("_encode" + sz(i) + "(m: " + m->name + ") -> bytes");
        w_.line("w = gmsg.text.TextWriter(" + string_literal(std::string(1, p_.separator)) + ")");
        if (p_.message_type.kind == MessageTypeSpec::Kind::Token) w_.line("w.token(" + type_id_literal(i) + ")");
        w_.line(m->name + ".write_text(w, m)");
        w_.line("out = " + n_ + ".prefix + gmsg.latin1_bytes(w.text())");
        if (p_.checksum.present) {
            const auto n = p_.checksum.size;
            w_.line("cs = " + checksum_of("out", sz(p_.prefix_size), "len(out)"));
            w_.line("out += gmsg.latin1_bytes(" + string_literal(std::string(1, p_.checksum.separator)) +
                    " + gmsg.text.hex_text(cs, " + sz(n) + "))");
        }
        if (p_.suffix_size) w_.line("out += " + n_ + ".suffix");
        w_.line("return out");
        w_.dedent();
    }

    Ctx& c_;
    CodeWriter& w_;
    const Protocol& p_;
    std::string n_, from_;
    bool text_ = false, has_header_ = false, escape_ = false, wire_cs_ = false, stream_ = false, all_struct_ = false;
};

} // namespace

void emit_protocol(Ctx& c, const Protocol& p) {
    if (p.format == Format::Binary && !p.header->fields.empty()) emit_binary_message(c, *p.header);
    ProtocolEmitter(c, p).emit();
}

} // namespace gmpc::py
