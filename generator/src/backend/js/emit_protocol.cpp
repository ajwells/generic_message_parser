// JavaScript emission for protocols: a class of static members with the framing configuration, open / select /
// process / parse / decode, encode and a stream factory. Semantics follow backend/cpp/emit_protocol.cpp.

#include <algorithm>
#include <string>

#include "backend/js/js_common.hpp"

namespace gmpc::js {

namespace {

using namespace ir;
using FT = FramingSpec::Type;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string fail(const char* err) { return std::string("gmsg.fail(Err.") + err + ");"; }

std::string byte_array(const std::vector<std::uint8_t>& v) {
    std::string s = "Uint8Array.of(";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + std::to_string(v[i]);
    return s + ")";
}

std::string escape_config(const Protocol& p) {
    const auto& e = p.framing.escape;
    if (!e.enabled) return "gmsg.NO_ESCAPE";
    std::string raw, code;
    for (const auto& [r, c] : e.table) {
        if (e.code == EscapeSpec::Code::Xor && r == e.esc) continue;
        raw += (raw.empty() ? "" : ", ") + std::to_string(r);
        code += (code.empty() ? "" : ", ") + std::to_string(c);
    }
    switch (e.code) {
        case EscapeSpec::Code::Xor: return "gmsg.xorEscape(" + sz(e.esc) + ", " + sz(e.xor_mask) + ", [" + raw + "])";
        case EscapeSpec::Code::Index: return "gmsg.indexEscape(" + sz(e.esc) + ", [" + raw + "], " + sz(e.base) + ")";
        case EscapeSpec::Code::Map: return "gmsg.mapEscape(" + sz(e.esc) + ", [" + raw + "], [" + code + "])";
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
    }

    void emit() {
        doc(w_, p_.doc);
        w_.open("class " + n_ + " {");
        emit_constants();
        emit_framing();
        emit_open();
        emit_select();
        emit_parse();
        emit_encode();
        w_.close("}");
        w_.line();
        c_.exports.push_back(n_);
    }

private:
    void emit_constants() {
        w_.line("static maxFrame = " + sz(p_.max_frame) + ";");
        w_.line("static prefixSize = " + sz(p_.prefix_size) + ";  // sync / start delimiter");
        w_.line("static headerSize = " + sz(p_.header_size) + ";");
        w_.line("static trailerSize = " + sz(p_.trailer_size) + ";  // binary checksum");
        w_.line("static suffixSize = " + sz(p_.suffix_size) + ";  // end delimiter");
        w_.line("static prefix = " + byte_array(p_.framing.sync) + ";");
        w_.line("static suffix = " + byte_array(p_.framing.end) + ";");
        if (has_header_) {
            w_.line("static Header = " + p_.header->name + ";");
            w_.line("static HeaderView = " + view_name(*p_.header) + ";");
        }
        std::string views, structs;
        for (const auto* m : p_.messages) {
            views += (views.empty() ? "" : ", ") + view_name(*m);
            if (m->want_struct) structs += (structs.empty() ? "" : ", ") + m->name;
        }
        w_.line("/** The message views of this protocol. */");
        w_.line("static views = [" + views + "];");
        if (all_struct_) w_.line("static messages = [" + structs + "];");
        w_.line("static escape = " + escape_config(p_) + ";");
    }

    void emit_framing() {
        const auto& fr = p_.framing;
        const auto sync = "this.prefix";
        switch (fr.type) {
            case FT::None:
                w_.line("/** No framing: the whole buffer is one frame. */");
                w_.line("static scan(b) { return { kind: 'frame', size: b.length, consume: b.length, consumeOnError: b.length, error: Err.None }; }");
                break;
            case FT::Fixed:
                w_.line("static framing = { sync: " + std::string(sync) + ", frameSize: " + sz(*fr.size) + ", escape: this.escape, suffixLen: " +
                        (escape_ ? sz(p_.suffix_size) : "0") + " };");
                w_.line("static scan(b) { return gmsg.scanFixed(b, " + n_ + ".framing); }");
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
                std::string cfg = "{ sync: " + std::string(sync) + ", lengthOffset: " + sz(p_.length_field_offset) +
                                  ", lengthSize: " + sz(prim_size(fr.length->prim)) + ", lengthEndian: " + endian(fr.length->endian) +
                                  ", add: " + std::to_string(add) + ", minFrame: " +
                                  sz(p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size) +
                                  ", maxFrame: this.maxFrame, escape: this.escape, suffixLen: " + (escape_ ? sz(p_.suffix_size) : "0");
                if (fr.length_counts_wire)
                    cfg += ", countsWire: true, headerLen: " + sz(p_.header_size) + ", trailerLen: " + sz(p_.trailer_size) +
                           ", wireIncludesTrailer: " +
                           (fr.length_covers == FramingSpec::LengthCovers::BodyChecksum ? "true" : "false");
                w_.line("static framing = " + cfg + " };");
                w_.line("static scan(b) { return gmsg.scanLengthPrefixed(b, " + n_ + ".framing); }");
                break;
            }
            case FT::Delimited:
                w_.line("static framing = { start: " + std::string(sync) + ", end: this.suffix, escape: this.escape, maxFrame: this.maxFrame };");
                w_.line("static scan(b) { return gmsg.scanDelimited(b, " + n_ + ".framing); }");
                break;
            case FT::Custom:
                w_.line("static scan(b) { return " + ext_fn(c_, fr.custom->lang, "scan") + "(b); }");
                break;
        }
    }

    std::string checksum_of(const std::string& bytes, const std::string& start, const std::string& end) {
        return checksum_value(c_, p_.checksum, bytes, start, end);
    }

    void emit_open() {
        w_.line("/** Validate one complete frame (as found by scan()) and split it: { header, body } (text: { typeId, body }). */");
        w_.open("static open(frame) {");
        w_.line("frame = gmsg.toBytes(frame);");
        w_.line("if (frame.length < " + sz(p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size) + ") " + fail("Truncated"));
        if (wire_cs_) emit_wire_checksum();
        if (escape_) {
            w_.line("const fr = frame.slice();  // unescaped in a copy");
            w_.line("const interior = fr.subarray(" + sz(p_.prefix_size) + ", gmsg.unescapeInPlace(fr, " + n_ + ".escape, " +
                    sz(p_.prefix_size) + ", fr.length - " + sz(p_.suffix_size) + "));");
        } else {
            w_.line("const fr = frame;");
            w_.line("const interior = fr.subarray(" + sz(p_.prefix_size) + ", fr.length - " + sz(p_.suffix_size) + ");");
        }
        if (text_) emit_open_text();
        else emit_open_binary();
        w_.close();
    }

    void emit_wire_checksum() {
        const auto H = sz(p_.header_size), T = sz(p_.trailer_size), P = sz(p_.prefix_size);
        w_.line("let computed;  // checksum.over: wire");
        w_.open("{");
        w_.line("const wire = frame.subarray(" + P + ", frame.length - " + sz(p_.suffix_size) + ");");
        w_.line("const un = gmsg.unescapedSize(wire, " + n_ + ".escape);");
        w_.line("if (un < " + H + " + " + T + ") " + fail("Truncated"));
        w_.line("const t = gmsg.walkEscaped(wire, 0, un - " + T + ", " + n_ + ".escape);");
        w_.line("if (t.kind !== 'done') gmsg.fail(t.kind === 'bad' ? t.error : Err.Truncated);");
        switch (p_.checksum.covers) {
            case ChecksumSpec::Covers::HeaderBody: w_.line("computed = " + checksum_of("wire", "0", "t.wire") + ";"); break;
            case ChecksumSpec::Covers::Body:
                w_.line("const h = gmsg.walkEscaped(wire, 0, " + H + ", " + n_ + ".escape);");
                w_.line("computed = " + checksum_of("wire", "h.wire", "t.wire") + ";");
                break;
            case ChecksumSpec::Covers::All: w_.line("computed = " + checksum_of("frame", "0", P + " + t.wire") + ";"); break;
        }
        w_.close();
    }

    void emit_open_binary() {
        const auto H = sz(p_.header_size), T = sz(p_.trailer_size), P = sz(p_.prefix_size);
        w_.line("if (interior.length < " + H + " + " + T + ") " + fail("Truncated"));
        if (p_.checksum.present) {
            w_.open("{");
            w_.line("const stored = gmsg.loadUintBig(interior, interior.length - " + T + ", " + T + ", " + endian(p_.checksum.endian) + ");");
            if (!wire_cs_) {
                std::string computed;
                switch (p_.checksum.covers) {
                    case ChecksumSpec::Covers::HeaderBody: computed = checksum_of("interior", "0", "interior.length - " + T); break;
                    case ChecksumSpec::Covers::Body: computed = checksum_of("interior", H, "interior.length - " + T); break;
                    case ChecksumSpec::Covers::All: computed = checksum_of("fr", "0", P + " + interior.length - " + T); break;
                }
                w_.line("const computed = " + computed + ";");
            }
            w_.line("if (stored !== computed) " + fail("BadChecksum"));
            w_.close();
        }
        w_.line("const f = {};");
        if (has_header_) w_.line("f.header = " + view_name(*p_.header) + ".from(interior.subarray(0, " + H + "));");
        w_.line("f.body = interior.subarray(" + H + ", interior.length - " + T + ");");
        w_.line("return f;");
    }

    void emit_open_text() {
        w_.line("let text = gmsg.latin1String(interior);");
        if (p_.checksum.present) {
            const auto n = p_.checksum.size;
            w_.line("const star = text.lastIndexOf(" + string_literal(std::string(1, p_.checksum.separator)) + ");");
            w_.open("if (star < 0) {");
            if (p_.checksum.required) w_.line(fail("BadChecksum"));
            w_.dedent();
            w_.open("} else {");
            w_.line("const stored = gmsg.text.hexValue(text.slice(star + 1), " + sz(n) + ");");
            w_.line("if (stored === null || stored !== " + checksum_of("interior", "0", "star") + ") " + fail("BadChecksum"));
            w_.line("text = text.slice(0, star);");
            w_.close();
        }
        w_.line("const p = text.indexOf(" + string_literal(std::string(1, p_.separator)) + ");");
        w_.line("return { typeId: p < 0 ? text : text.slice(0, p), body: p < 0 ? '' : text.slice(p + 1) };");
    }

    std::string type_id_literal(std::size_t i) const {
        const auto& id = p_.type_ids[i];
        if (auto* s = std::get_if<std::string>(&id)) return string_literal(*s);
        if (auto* n = std::get_if<std::int64_t>(&id)) return big_literal(*n);
        return "";
    }

    void emit_select() {
        w_.line("/** The message view selected by the frame's type id (message_type). Throws GmpError (UnknownType, ...). */");
        w_.open("static select(f) {");
        switch (p_.message_type.kind) {
            case MessageTypeSpec::Kind::None:
                w_.line("return " + view_name(*p_.messages[0]) + ".from(f.body);");
                break;
            case MessageTypeSpec::Kind::Field:
                w_.line("const id = gmsg.toInt(f.header." + ident(p_.message_type.field->name) + ");");
                for (std::size_t i = 0; i < p_.messages.size(); ++i)
                    w_.line("if (id === " + type_id_literal(i) + ") return " + view_name(*p_.messages[i]) + ".from(f.body);");
                w_.line("return " + fail("UnknownType"));
                break;
            case MessageTypeSpec::Kind::Token:
                for (std::size_t i = 0; i < p_.messages.size(); ++i)
                    w_.line("if (f.typeId === " + type_id_literal(i) + ") return " + view_name(*p_.messages[i]) + ".from(f.body);");
                w_.line("return " + fail("UnknownType"));
                break;
        }
        w_.close();
        w_.line("/** gmsg.StreamParser hook: { header, message } of a complete frame. */");
        w_.open("static process(frame) {");
        w_.line("const f = " + n_ + ".open(frame);");
        w_.line("return { header: f.header, message: " + n_ + ".select(f) };");
        w_.close();
    }

    void emit_parse() {
        w_.line("/** Parse one frame at the start of `buffer` (whole-buffer / datagram mode): the message view. */");
        w_.open("static parse(buffer) {");
        w_.line("buffer = gmsg.toBytes(buffer);");
        w_.line("const s = " + n_ + ".scan(buffer);");
        w_.line("if (s.kind === 'need_more') " + fail("Truncated"));
        w_.line("if (s.kind === 'skip') gmsg.fail(s.error === Err.None ? Err.BadSync : s.error);");
        w_.line("return " + n_ + ".select(" + n_ + ".open(buffer.subarray(0, s.size)));");
        w_.close();
        if (all_struct_) {
            w_.line("/** Parse one frame and decode it into its message class. */");
            w_.line("static decode(buffer) { return " + n_ + ".parse(buffer).toStruct(); }");
        }
        if (stream_) {
            w_.line("/** A stream parser for this protocol (options: { capacity }, default maxFrame). */");
            w_.line("static stream(options) { return new gmsg.StreamParser(" + n_ + ", options); }");
        }
    }

    // ---- encode ------------------------------------------------------------------------------------
    void emit_encode() {
        bool any = false;
        for (const auto* m : p_.messages) any = any || m->want_struct;
        if (!any) return;
        w_.line(std::string("/** Encode a message of this protocol") + (has_header_ ? " (optionally with header values; type id and length are filled in)" : "") +
                " to a new Uint8Array. */");
        w_.open(std::string("static encode(m") + (has_header_ ? ", header" : "") + ") {");
        for (std::size_t i = 0; i < p_.messages.size(); ++i) {
            const auto* m = p_.messages[i];
            if (!m->want_struct) continue;
            w_.line("if (m instanceof " + m->name + ") return " + n_ + ".#encode" + sz(i) + "(m" + (has_header_ ? ", header" : "") + ");");
        }
        w_.line("return gmsg.fail(Err.InvalidValue, 'not a message of " + n_ + " (use the message classes)');");
        w_.close();
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
        w_.open("static #encode" + sz(i) + "(m" + (has_header_ ? ", header" : "") + ") {");
        w_.line("const w = new gmsg.Writer();");
        if (p_.prefix_size) w_.line("w.put(" + n_ + ".prefix);");
        w_.line("const headerAt = w.pos;");
        if (has_header_) {
            const auto& hm = *p_.header;
            w_.line("const h = Object.assign(new " + hm.name + "(), header);");
            if (p_.message_type.kind == MessageTypeSpec::Kind::Field) {
                const auto& df = *p_.message_type.field;
                if (!df.const_value) w_.line("h." + ident(df.name) + " = " + typed_literal(df, std::get<std::int64_t>(p_.type_ids[i])) + ";");
            }
            w_.line(hm.name + ".write(w, h);");
        }
        w_.line("const bodyAt = w.pos;");
        w_.line(m->name + ".write(w, m);");
        w_.line("const bodyLen = w.pos - bodyAt;");
        if (fr.type == FT::Fixed) {
            const auto body = *fr.size - (p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size);
            w_.line("if (bodyLen > " + sz(body) + ") " + fail("BadLength"));
            w_.line("w.zeros(" + sz(body) + " - bodyLen);");
        }
        if (fr.type == FT::LengthPrefixed) {
            const auto n = prim_size(fr.length->prim);
            std::string covered;
            switch (fr.length_covers) {
                case FramingSpec::LengthCovers::Body:
                    covered = fr.length_counts_wire ? "bodyLen + gmsg.escapeOverhead(w.buf, " + n_ + ".escape, bodyAt, bodyAt + bodyLen)" : "bodyLen";
                    break;
                case FramingSpec::LengthCovers::BodyChecksum:
                    covered = fr.length_counts_wire ? "bodyWire + trailerWire" : "bodyLen + " + T;
                    break;
                case FramingSpec::LengthCovers::HeaderBody: covered = H + " + bodyLen"; break;
                case FramingSpec::LengthCovers::All: covered = P + " + " + H + " + bodyLen + " + T; break;
            }
            w_.open("{");
            if (fr.length_counts_wire && fr.length_covers == FramingSpec::LengthCovers::BodyChecksum) emit_wire_trailer_size();
            const auto max = n >= 8 ? std::string("18446744073709551615") : std::to_string((std::uint64_t{1} << (8 * n)) - 1);
            w_.line("const value = BigInt(" + covered + ") - (" + big_literal(fr.length_adjust) + ");");
            w_.line("if (value < 0n || value > " + max + "n) " + fail("BadLength"));
            w_.line("w.patchUint(headerAt + " + sz(*fr.length->static_offset) + ", value, " + sz(n) + ", " + endian(fr.length->endian) + ");");
            w_.close();
        }
        if (p_.checksum.present && !wire_cs_) {
            std::string start;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: start = "headerAt"; break;
                case ChecksumSpec::Covers::Body: start = "bodyAt"; break;
                case ChecksumSpec::Covers::All: start = "0"; break;
            }
            w_.line("w.uint(" + checksum_of("w.buf", start, "w.pos") + ", " + T + ", " + endian(p_.checksum.endian) + ");");
        }
        w_.line("let out = w.toBytes();");
        if (wire_cs_) {
            if (p_.checksum.covers == ChecksumSpec::Covers::Body)
                w_.line("const bodyWire = bodyAt + gmsg.escapeOverhead(out, " + n_ + ".escape, " + P + ", bodyAt);");
            w_.line("out = gmsg.escapeRange(out, " + P + ", out.length - " + P + ", " + n_ + ".escape);");
            std::string cs;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: cs = checksum_of("out", P, "out.length"); break;
                case ChecksumSpec::Covers::Body: cs = checksum_of("out", "bodyWire", "out.length"); break;
                case ChecksumSpec::Covers::All: cs = checksum_of("out", "0", "out.length"); break;
            }
            w_.line("const cs = new Uint8Array(" + T + ");");
            w_.line("gmsg.storeUint(cs, 0, " + T + ", " + cs + ", " + endian(p_.checksum.endian) + ");  // over the wire bytes");
            w_.line("out = gmsg.concat(out, gmsg.escape(cs, " + n_ + ".escape));");
        } else if (escape_) {
            w_.line("out = gmsg.escapeRange(out, " + P + ", out.length - " + P + ", " + n_ + ".escape);");
        }
        if (p_.suffix_size) w_.line("out = gmsg.concat(out, " + n_ + ".suffix);");
        w_.line("return out;");
        w_.close();
    }

    void emit_wire_trailer_size() {
        const auto T = sz(p_.trailer_size);
        w_.line("// the length counts the escaped body and the escaped checksum");
        w_.line("const bodyWire = bodyLen + gmsg.escapeOverhead(w.buf, " + n_ + ".escape, bodyAt, bodyAt + bodyLen);");
        if (!p_.checksum.present) {
            w_.line("const trailerWire = 0;");
            return;
        }
        if (wire_cs_) {
            w_.line("const eb = gmsg.escape(w.buf.subarray(bodyAt, bodyAt + bodyLen), " + n_ + ".escape);");
            w_.line("const cs0 = " + checksum_of("eb", "0", "eb.length") + ";");
        } else {
            w_.line("const cs0 = " + checksum_of("w.buf", "bodyAt", "bodyAt + bodyLen") + ";");
        }
        w_.line("const csb = new Uint8Array(" + T + ");");
        w_.line("gmsg.storeUint(csb, 0, " + T + ", cs0, " + endian(p_.checksum.endian) + ");");
        w_.line("const trailerWire = " + T + " + gmsg.escapeOverhead(csb, " + n_ + ".escape);");
    }

    void emit_encode_text(std::size_t i) {
        const auto* m = p_.messages[i];
        w_.open("static #encode" + sz(i) + "(m) {");
        w_.line("const w = new gmsg.text.TextWriter(" + string_literal(std::string(1, p_.separator)) + ");");
        if (p_.message_type.kind == MessageTypeSpec::Kind::Token) w_.line("w.token(" + type_id_literal(i) + ");");
        w_.line(m->name + ".writeText(w, m);");
        w_.line("let out = gmsg.concat(" + n_ + ".prefix, gmsg.latin1Bytes(w.toString()));");
        if (p_.checksum.present) {
            const auto n = p_.checksum.size;
            w_.line("const cs = " + checksum_of("out", sz(p_.prefix_size), "out.length") + ";");
            w_.line("out = gmsg.concat(out, gmsg.latin1Bytes(" + string_literal(std::string(1, p_.checksum.separator)) +
                    " + gmsg.text.hexText(cs, " + sz(n) + ")));");
        }
        if (p_.suffix_size) w_.line("out = gmsg.concat(out, " + n_ + ".suffix);");
        w_.line("return out;");
        w_.close();
    }

    Ctx& c_;
    CodeWriter& w_;
    const Protocol& p_;
    std::string n_;
    bool text_ = false, has_header_ = false, escape_ = false, wire_cs_ = false, stream_ = false, all_struct_ = false;
};

} // namespace

void emit_protocol(Ctx& c, const Protocol& p) {
    if (p.format == Format::Binary && !p.header->fields.empty()) emit_binary_message(c, *p.header);
    ProtocolEmitter(c, p).emit();
}

} // namespace gmpc::js
