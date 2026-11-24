// C emission for binary protocols: framing config, frame validation (checksum, escaping), message selection,
// whole-buffer parse / decode / process, callbacks, the stream wrapper and encoding.
//
// Mirrors backend/cpp/emit_protocol.cpp (same checks in the same order, same error codes). Text protocols, custom
// framers and custom checksums are not supported yet (c_backend.cpp skips those protocols with a warning).

#include <algorithm>
#include <sstream>
#include <string>

#include "backend/c/c_common.hpp"

namespace gmpc::c {

namespace {

using namespace ir;
using FT = FramingSpec::Type;

std::string sz(std::size_t v) { return std::to_string(v); }
std::string try_(const std::string& call) { return "if ((e = " + call + ") != GMSG_OK) return e;"; }
std::string fail(const char* err) { return std::string("return GMSG_ERR_") + err + ";"; }

std::string hex_byte(std::uint8_t b) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << (b < 16 ? "0" : "") << static_cast<int>(b);
    return os.str();
}

std::string byte_list(const std::vector<std::uint8_t>& v) {
    if (v.empty()) return "0";
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + hex_byte(v[i]);
    return s;
}

std::string pattern(const std::vector<std::uint8_t>& v) { return "{{" + byte_list(v) + "}, " + sz(v.size()) + "}"; }

/// gmsg_escape_config initializer, built like gmsg::make_xor_escape / make_index_escape / make_map_escape.
std::string escape_init(const Protocol& p) {
    const auto& e = p.framing.escape;
    if (!e.enabled) return "{false, 0, 0, 0, 0, {0}, {0}, {0}}";
    std::vector<std::uint8_t> raw, code;
    std::uint32_t mask[8] = {};
    auto reserve = [&](std::uint8_t b) { mask[b >> 5] |= std::uint32_t{1} << (b & 31u); };
    reserve(e.esc);
    for (const auto& [r, c] : e.table) {
        if (e.code == EscapeSpec::Code::Xor && r == e.esc) continue;  // always escaped
        if (raw.size() < 16) {
            raw.push_back(r);
            code.push_back(e.code == EscapeSpec::Code::Index ? static_cast<std::uint8_t>(e.base + code.size())
                           : e.code == EscapeSpec::Code::Map ? c
                                                             : std::uint8_t{0});
        }
        reserve(r);
    }
    std::ostringstream m;
    for (int i = 0; i < 8; ++i) m << (i ? ", " : "") << "0x" << std::hex << std::uppercase << mask[i] << "u";
    const bool xor_style = e.code == EscapeSpec::Code::Xor;
    return std::string("{true, ") + hex_byte(e.esc) + ", " + (xor_style ? "GMSG_ESCAPE_XOR" : "GMSG_ESCAPE_TABLE") + ", " +
           hex_byte(xor_style ? e.xor_mask : 0) + ", " + sz(raw.size()) + ", {" + byte_list(raw) + "}, {" +
           (xor_style ? std::string("0") : byte_list(code)) + "}, {" + m.str() + "}}";
}

class ProtocolEmitter {
public:
    ProtocolEmitter(Ctx& c, const Protocol& p) : c_(c), h_(c.h), s_(c.c), p_(p) {
        text_ = p.format == Format::Text;
        has_header_ = !text_ && !p.header->fields.empty();
        escape_ = p.framing.escape.enabled;
        wire_cs_ = escape_ && p.checksum.present && p.checksum.over_wire;
        cs_first_ = escape_ && !text_ && p.checksum.present && !p.checksum.over_wire && !p.checksum.custom;
        stream_ = p.framing.type != FT::None && p.max_frame > 0;
        all_struct_ = std::all_of(p.messages.begin(), p.messages.end(), [](const Message* m) { return m->want_struct; });
        n_ = c.names.prefix + snake(p.name);
        M_ = c.names.macro + upper(snake(p.name));
        if (has_header_) {
            hv_ = view_name(c, *p.header);
            ht_ = type_name(c, *p.header);
        }
        buf_ = escape_ ? "uint8_t*" : "const uint8_t*";
        cfg_ = n_ + "_framing_";
    }

    void emit() {
        h_.line("/* ==== protocol " + p_.name + " " + std::string(std::max<int>(0, 84 - static_cast<int>(p_.name.size())), '=') + " */");
        h_.line();
        s_.line("/* ==== protocol " + p_.name + " " + std::string(std::max<int>(0, 84 - static_cast<int>(p_.name.size())), '=') + " */");
        s_.line();
        doc(h_, p_.doc);
        emit_types();
        emit_prototypes();
        emit_framing();
        emit_open();
        emit_select();
        emit_deliver();
        emit_parse();
        if (stream_) emit_stream();
        emit_encode();
    }

private:
    std::string kind(const Message& m) const { return M_ + "_" + upper(snake(m.name)); }
    std::string member(const Message& m) const { return ident(snake(m.name)); }
    std::string hdr_param() const { return has_header_ ? "const " + hv_ + "* h, " : ""; }

    // ---- header file: types -------------------------------------------------------------------------
    void emit_types() {
        if (stream_) h_.line("#define " + M_ + "_MAX_FRAME " + sz(p_.max_frame) + "  /* minimum stream buffer size */");
        h_.line();
        h_.line("/* Message of a frame (kind 0 = none). */");
        h_.open("typedef enum " + n_ + "_kind {");
        h_.line(M_ + "_NONE = 0,");
        for (std::size_t i = 0; i < p_.messages.size(); ++i) h_.line(kind(*p_.messages[i]) + " = " + sz(i + 1) + ",");
        h_.close("} " + n_ + "_kind;");
        h_.line();
        auto any = [&](const std::string& suffix, bool view) {
            h_.open("typedef struct " + n_ + "_" + suffix + " {");
            h_.line(n_ + "_kind kind;");
            h_.open("union {");
            for (const auto* m : p_.messages) h_.line((view ? view_name(c_, *m) : type_name(c_, *m)) + " " + member(*m) + ";");
            h_.close("} as;");
            h_.close("} " + n_ + "_" + suffix + ";");
            h_.line();
        };
        h_.line("/* Any message view of " + p_.name + "; views point into the frame buffer. */");
        any("any_view", true);
        if (all_struct_) {
            h_.line("/* Any decoded message of " + p_.name + ". */");
            any("any_message", false);
        }
        h_.line("/* Callbacks for " + n_ + "_process() and the stream. NULL slots are skipped; for each frame the");
        h_.line(" * message's callback runs first, then on_frame. Views are valid only during the call. */");
        h_.open("typedef struct " + n_ + "_callbacks {");
        h_.line("void* ctx;  /* passed to every callback */");
        for (const auto* m : p_.messages)
            h_.line("void (*on_" + snake(m->name) + ")(void* ctx, " + hdr_param() + "const " + view_name(c_, *m) + "* v);");
        h_.line("void (*on_frame)(void* ctx, " + hdr_param() + "const " + n_ + "_any_view* v);  /* every frame */");
        h_.line("void (*on_error)(void* ctx, gmsg_error e, const uint8_t* data, size_t size);  /* stream: dropped bytes */");
        h_.close("} " + n_ + "_callbacks;");
        h_.line();
        if (stream_) {
            h_.line("/* A frame returned by " + n_ + "_stream_poll_frame(); views point into the stream buffer. */");
            h_.open("typedef struct " + n_ + "_received {");
            if (has_header_) h_.line(hv_ + " header;");
            h_.line(n_ + "_any_view message;");
            h_.line(std::string("const uint8_t* bytes;  /* the frame") + (escape_ ? " (unescaped in place: only header and body are meaningful)" : " as received") + " */");
            h_.line("size_t size;");
            h_.line(std::string("size_t extra;  /* ") + (text_ ? "tokens" : "bytes") + " in the frame after the message (ignored" +
                    (p_.exact_size ? "; always 0 with exact_size) */" : ") */"));
            h_.close("} " + n_ + "_received;");
            h_.line();
            h_.line("/* Result of " + n_ + "_stream_poll_event(): a message (GMSG_POLL_MESSAGE) or an error (GMSG_POLL_ERROR). */");
            h_.open("typedef struct " + n_ + "_event {");
            h_.line(n_ + "_any_view message;");
            h_.line("gmsg_error error;");
            h_.line("const uint8_t* data;  /* error: the dropped bytes */");
            h_.line("size_t size;");
            h_.close("} " + n_ + "_event;");
            h_.line();
            h_.line("/* Stream parser; set callbacks in .cb after " + n_ + "_stream_init(). */");
            h_.open("typedef struct " + n_ + "_stream {");
            h_.line("gmsg_stream base;  /* first member */");
            h_.line(n_ + "_callbacks cb;");
            h_.line("/* internal: pull API state */");
            h_.line("uint8_t mode_;");
            h_.line(n_ + "_kind want_;");
            h_.line(n_ + "_received got_;");
            if (all_struct_) h_.line(n_ + "_any_message* msg_;");
            h_.close("} " + n_ + "_stream;");
            h_.line();
        }
    }

    void emit_prototypes() {
        const auto esc_note = escape_ ? " The frame is unescaped in place." : "";
        h_.line("/* Parse one frame at the start of data[0..size) (whole-buffer / datagram mode)." + std::string(esc_note) + " */");
        h_.line("gmsg_error " + n_ + "_parse(" + buf_ + " data, size_t size, " + n_ + "_any_view* out);");
        if (all_struct_) {
            h_.line("/* Parse one frame and decode it into the message struct. */");
            h_.line("gmsg_error " + n_ + "_decode(" + buf_ + " data, size_t size, " + n_ + "_any_message* out);");
        }
        h_.line("/* Parse one frame and pass its message to the callbacks (cb may be NULL). */");
        h_.line("gmsg_error " + n_ + "_process(" + buf_ + " data, size_t size, const " + n_ + "_callbacks* cb);");
        for (const auto* m : p_.messages) {
            if (!m->want_struct) continue;
            const auto t = type_name(c_, *m);
            const auto f = n_ + "_encode_" + snake(m->name);
            h_.line("gmsg_error " + f + "(const " + t + "* m, uint8_t* out, size_t capacity, size_t* written);");
            if (has_header_)
                h_.line("gmsg_error " + f + "_with_header(const " + ht_ + "* h, const " + t +
                        "* m, uint8_t* out, size_t capacity, size_t* written);  /* id / length fields are filled in */");
        }
        if (all_struct_) h_.line("gmsg_error " + n_ + "_encode(const " + n_ + "_any_message* m, uint8_t* out, size_t capacity, size_t* written);");
        h_.line("/* Print the message held (gmp/c/print.h); kind 0 prints <none>. */");
        h_.line("void " + n_ + "_any_view_print(const " + n_ + "_any_view* v, gmsg_printer* p);");
        if (all_struct_) h_.line("void " + n_ + "_any_message_print(const " + n_ + "_any_message* m, gmsg_printer* p);");
        auto print_any = [&](const std::string& suffix, bool view) {
            s_.open("void " + n_ + "_" + suffix + "_print(const " + n_ + "_" + suffix + "* v, gmsg_printer* p) {");
            s_.open("switch (v->kind) {");
            for (const auto* m : p_.messages)
                s_.line("case " + kind(*m) + ": " + (view ? view_name(c_, *m) : type_name(c_, *m)) + "_print(&v->as." +
                        member(*m) + ", p); break;");
            s_.line("default: gmsg_print_text(p, \"<none>\", 6); break;");
            s_.close();
            s_.close();
            s_.line();
        };
        print_any("any_view", true);
        if (all_struct_) print_any("any_message", false);
        if (stream_) {
            const auto st = n_ + "_stream";
            h_.line();
            h_.line("/* GMSG_ERR_BUFFER_FULL when size < " + M_ + "_MAX_FRAME. Clears the callbacks. */");
            h_.line("gmsg_error " + st + "_init(" + st + "* s, uint8_t* buf, size_t size);");
            h_.line("/* Push data and deliver every complete frame; returns the number of frames delivered. */");
            h_.line("static inline size_t " + st + "_feed(" + st + "* s, const uint8_t* data, size_t n) { return gmsg_stream_feed(&s->base, data, n); }");
            h_.line("static inline size_t " + st + "_push(" + st + "* s, const uint8_t* data, size_t n) { return gmsg_stream_push(&s->base, data, n); }");
            h_.line("static inline bool " + st + "_next(" + st + "* s) { return gmsg_stream_next(&s->base); }");
            h_.line("/* Interrupt input: drain a gmsg_ring filled by an ISR (at most max bytes; SIZE_MAX: all). Bytes the full");
            h_.line(" * ring dropped are reported to cb.on_error as GMSG_ERR_OVERRUN; _push_ring moves bytes only (pull API). */");
            h_.line("static inline size_t " + st + "_feed_ring(" + st + "* s, gmsg_ring* ring, size_t max) { return gmsg_stream_feed_ring(&s->base, ring, max); }");
            h_.line("static inline size_t " + st + "_push_ring(" + st + "* s, gmsg_ring* ring, size_t max) { return gmsg_stream_push_ring(&s->base, ring, max); }");
            h_.line("/* Recovery (they only drop data, reported to cb.on_error as GMSG_ERR_TRUNCATED; frames that become");
            h_.line(" * complete are delivered by the next _next / _feed / poll call): give up on an incomplete frame at the");
            h_.line(" * head, drop everything, or resync from _tick(s, now) after an idle timeout (0 = off; any time unit). */");
            h_.line("static inline bool " + st + "_resync(" + st + "* s) { return gmsg_stream_resync(&s->base); }");
            h_.line("static inline bool " + st + "_flush(" + st + "* s) { return gmsg_stream_flush(&s->base); }");
            h_.line("static inline void " + st + "_set_idle_timeout(" + st + "* s, uint32_t t) { gmsg_stream_set_idle_timeout(&s->base, t); }");
            h_.line("static inline bool " + st + "_tick(" + st + "* s, uint32_t now) { return gmsg_stream_tick(&s->base, now); }");
            h_.line("static inline void " + st + "_reset(" + st + "* s) { gmsg_stream_reset(&s->base); }");
            h_.line("static inline const gmsg_stream_stats* " + st + "_stats(const " + st + "* s) { return &s->base.stats; }");
            h_.line("static inline gmsg_error " + st + "_last_error(const " + st + "* s) { return s->base.last_error; }");
            h_.line("static inline size_t " + st + "_buffered(const " + st + "* s) { return gmsg_stream_buffered(&s->base); }");
            h_.line("/* Changes whenever views returned by the poll functions expire. */");
            h_.line("static inline uint32_t " + st + "_generation(const " + st + "* s) { return gmsg_stream_generation(&s->base); }");
            h_.line();
            h_.line("/* Pull API. Returned views point into the stream buffer and stay valid until the next call on the");
            h_.line(" * stream. Message callbacks are never called (except by poll_for for other messages); errors go to");
            h_.line(" * cb.on_error, except in poll_event, which returns them. All return false / GMSG_POLL_NEED_MORE when");
            h_.line(" * more input is needed. */");
            h_.line("bool " + st + "_poll(" + st + "* s, " + n_ + "_any_view* out);");
            h_.line("bool " + st + "_poll_frame(" + st + "* s, " + n_ + "_received* out);");
            if (all_struct_)
                h_.line("bool " + st + "_poll_message(" + st + "* s, " + n_ + "_any_message* out);  /* decoded copy; decode failures are reported like bad frames */");
            h_.line("gmsg_poll " + st + "_poll_event(" + st + "* s, " + n_ + "_event* out);");
            h_.line("/* Wait for a message of `kind`; other messages go to the callbacks (message slot, then on_frame). */");
            h_.line("bool " + st + "_poll_for(" + st + "* s, " + n_ + "_kind kind, " + n_ + "_any_view* out);");
        }
        h_.line();
    }

    // ---- source file --------------------------------------------------------------------------------
    std::string esc() const { return "&" + cfg_ + ".escape"; }

    void emit_framing() {
        const auto& fr = p_.framing;
        if (!fr.sync.empty()) s_.line("static const uint8_t " + n_ + "_prefix_[" + sz(fr.sync.size()) + "] = {" + byte_list(fr.sync) + "};");
        if (!fr.end.empty()) s_.line("static const uint8_t " + n_ + "_suffix_[" + sz(fr.end.size()) + "] = {" + byte_list(fr.end) + "};");
        const auto suffix = escape_ ? sz(p_.suffix_size) : std::string("0");
        switch (fr.type) {
            case FT::None:
                s_.line("/* No framing: the whole buffer is one frame. */");
                s_.line("static gmsg_frame_scan " + n_ + "_scan_(const uint8_t* b, size_t n) { (void)b; return gmsg_scan_frame(n, n, n); }");
                break;
            case FT::Fixed:
                s_.line("static const gmsg_fixed_config " + cfg_ + " = {" + pattern(fr.sync) + ", " + sz(*fr.size) + ", " +
                        escape_init(p_) + ", " + suffix + "};");
                s_.line("static gmsg_frame_scan " + n_ + "_scan_(const uint8_t* b, size_t n) { return gmsg_scan_fixed(b, n, &" + cfg_ + "); }");
                break;
            case FT::LengthPrefixed: {
                const auto prefix = static_cast<std::int64_t>(p_.prefix_size);
                const auto header = static_cast<std::int64_t>(p_.header_size);
                const auto trailer = static_cast<std::int64_t>(p_.trailer_size);
                const auto sfx = static_cast<std::int64_t>(p_.suffix_size);
                std::int64_t add = fr.length_adjust + sfx;
                if (fr.length_counts_wire) add = fr.length_adjust;
                else if (fr.length_covers == FramingSpec::LengthCovers::Body) add += prefix + header + trailer;
                else if (fr.length_covers == FramingSpec::LengthCovers::BodyChecksum) add += prefix + header;
                else if (fr.length_covers == FramingSpec::LengthCovers::HeaderBody) add += prefix + trailer;
                const bool cw = fr.length_counts_wire;
                s_.line("static const gmsg_length_prefixed_config " + cfg_ + " = {" + pattern(fr.sync) + ", " +
                        sz(p_.length_field_offset) + ", " + sz(prim_size(fr.length->prim)) + ", " + endian_expr(fr.length->endian) +
                        ", " + int_literal(add) + ", " + sz(p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size) +
                        ", " + sz(p_.max_frame) + ", " + escape_init(p_) + ", " + suffix + ", " + (cw ? "true" : "false") + ", " +
                        (cw ? sz(p_.header_size) : "0") + ", " + (cw ? sz(p_.trailer_size) : "0") + ", " +
                        (cw && fr.length_covers == FramingSpec::LengthCovers::BodyChecksum ? "true" : "false") + "};");
                if (p_.size_checks.empty()) {
                    s_.line("static gmsg_frame_scan " + n_ + "_scan_(const uint8_t* b, size_t n) { return gmsg_scan_length_prefixed(b, n, &" + cfg_ + "); }");
                    break;
                }
                emit_checked_scan();
                break;
            }
            case FT::Delimited:
                s_.line("static const gmsg_delimited_config " + cfg_ + " = {" + pattern(fr.sync) + ", " + pattern(fr.end) + ", " +
                        escape_init(p_) + ", " + sz(p_.max_frame) + "};");
                s_.line("static gmsg_frame_scan " + n_ + "_scan_(const uint8_t* b, size_t n) { return gmsg_scan_delimited(b, n, &" + cfg_ + "); }");
                break;
            case FT::Custom: {
                const auto fn = lang_value(fr.custom->lang, "scan");
                s_.line("/* custom framer '" + fr.custom->name + "' */");
                s_.line("static gmsg_frame_scan " + n_ + "_scan_(const uint8_t* b, size_t n) { return " + fn + "(b, n); }");
                break;
            }
        }
        s_.line();
        s_.line("/* A validated frame split into its parts (pointers into the frame buffer). */");
        s_.open("typedef struct " + n_ + "_frame_ {");
        if (has_header_) s_.line(hv_ + " header;");
        if (text_) s_.line("gmsg_str type_id;  /* text: the first token */");
        s_.line("const uint8_t* body;");
        s_.line("size_t body_size;");
        if (escape_) s_.line("size_t unescaped_n;  /* size of the unescaped part (to restore the frame) */");
        s_.close("} " + n_ + "_frame_;");
        s_.line();
    }

    static std::string size_fail(const SizeCheck& c) {
        std::string s;
        if (c.min > 0) s = "body < " + std::to_string(c.min);
        if (c.max) s += (s.empty() ? "" : " || ") + std::string("body > ") + std::to_string(*c.max);
        return s;
    }

    /// length_prefixed scan that also rejects a length the frame's message type cannot have, as soon as the header has
    /// arrived (C++: the generated scan()).
    void emit_checked_scan() {
        s_.line(std::string("/* Scan for a frame; a length the message type cannot have (") +
                (p_.exact_size ? "shorter or longer than the message" : "shorter than the message") +
                ") is rejected with BAD_LENGTH as soon as the header has arrived. */");
        s_.open("static gmsg_frame_scan " + n_ + "_scan_(const uint8_t* b, size_t n) {");
        s_.line("const gmsg_frame_scan s = gmsg_scan_length_prefixed(b, n, &" + cfg_ + ");");
        s_.line("uint8_t h[" + H() + "];");
        s_.line(hv_ + " hv;");
        s_.line("int64_t body;");
        s_.line("if (s.kind == GMSG_SCAN_SKIP) return s;");
        s_.line("if (!gmsg_read_frame_header(b, n, " + cfg_ + ".sync.len, &" + cfg_ + ".escape, h, " + H() + ")) return s;");
        s_.line("hv = " + hv_ + "_unchecked(h, " + H() + ");");
        s_.line("body = (int64_t)" + hv_ + "_" + p_.framing.length->name + "(&hv) + " + int_literal(p_.length_to_body) + ";");
        if (p_.message_type.kind == MessageTypeSpec::Kind::None) {
            s_.line("if (" + size_fail(p_.size_checks.front()) + ") return gmsg_scan_skip(1, GMSG_ERR_BAD_LENGTH);");
        } else {
            s_.open("switch ((int64_t)" + hv_ + "_" + p_.message_type.field->name + "(&hv)) {");
            for (const auto& c : p_.size_checks)
                s_.line("case " + int_literal(std::get<std::int64_t>(p_.type_ids[c.message])) + ": if (" + size_fail(c) +
                        ") return gmsg_scan_skip(1, GMSG_ERR_BAD_LENGTH); break;  /* " + p_.messages[c.message]->name + " */");
            s_.line("default: break;");
            s_.close();
        }
        s_.line("return s;");
        s_.close();
    }

    std::string P() const { return sz(p_.prefix_size); }
    std::string H() const { return sz(p_.header_size); }
    std::string T() const { return sz(p_.trailer_size); }
    std::string S() const { return sz(p_.suffix_size); }

    void emit_open() {
        if (escape_) emit_open_escaped();
        else emit_open_plain();
    }

    void emit_open_plain() {
        s_.line("/* Validate one complete frame (as delimited by the scanner) and split it into header and body. */");
        s_.open("static gmsg_error " + n_ + "_open_(" + buf_ + " frame, size_t size, " + n_ + "_frame_* f) {");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line("(void)e;");
        emit_size_check();
        s_.line("const size_t interior_n = size - " + P() + " - " + S() + ";");
        s_.line("const uint8_t* interior = frame + " + P() + ";");
        emit_split_body(true);
        s_.close();
        s_.line();
    }

    // (sizes are unsigned: checks against 0 are left out, as they can never fail)
    void emit_size_check() {
        if (const auto overhead = p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size; overhead > 0)
            s_.line("if (size < " + sz(overhead) + ") " + fail("TRUNCATED"));
    }

    /// Escaped protocols: the checksum (built-in or over the wire) is verified before the frame is unescaped in
    /// place, and a frame that fails later is escaped again, so the buffer holds the bytes as received.
    void emit_open_escaped() {
        const bool check_first = cs_first_ || wire_cs_;
        s_.line("/* Split an unescaped frame into header and body. */");
        s_.open("static gmsg_error " + n_ + "_split_(const uint8_t* frame, const uint8_t* interior, size_t interior_n, " + n_ +
                "_frame_* f) {");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line("(void)e;");
        s_.line("(void)frame;");
        emit_split_body(!check_first);
        s_.close();
        s_.line();
        s_.line("/* Escape the unescaped part of an opened frame again (the frame then holds the bytes as received). */");
        s_.open("static void " + n_ + "_restore_(uint8_t* frame, size_t size, size_t unescaped_n) {");
        s_.line("size_t k = 0;");
        s_.line("(void)gmsg_escape_in_place(frame, size - " + S() + ", " + P() + ", unescaped_n, " + esc() + ", &k);");
        s_.close();
        s_.line();
        s_.line(std::string("/* Validate one complete frame (as delimited by the scanner) and split it into header and body.") +
                (check_first ? " The checksum is verified first;" : "") +
                " the frame is unescaped in place, and holds the bytes as received again on failure. */");
        s_.open("static gmsg_error " + n_ + "_open_(" + buf_ + " frame, size_t size, " + n_ + "_frame_* f) {");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line("(void)e;");
        emit_size_check();
        if (wire_cs_) emit_wire_checksum();
        s_.line("uint8_t* wire = frame + " + P() + ";");
        s_.line("const size_t wire_n = size - " + P() + " - " + S() + ";");
        s_.line("size_t un = 0;");
        s_.line(try_("gmsg_check_escaped(wire, wire_n, " + esc() + ", &un)"));
        if (check_first) emit_escaped_checksum();
        s_.line("(void)gmsg_unescape_in_place(wire, wire_n, " + esc() + ", &un);");
        s_.line("e = " + n_ + "_split_(frame, wire, un, f);");
        s_.line("if (e != GMSG_OK) " + n_ + "_restore_(frame, size, un);");
        s_.line("else f->unescaped_n = un;");
        s_.line("return e;");
        s_.close();
        s_.line();
    }

    std::string checksum_ctype() const {
        const auto n = p_.checksum.size;
        return n <= 1 ? "uint8_t" : n <= 2 ? "uint16_t" : n <= 4 ? "uint32_t" : "uint64_t";
    }

    /// Verify the checksum of an escaped binary frame without modifying it (stored value and, unless it was computed
    /// over the wire, the computed value read through the escaping).
    void emit_escaped_checksum() {
        s_.line("if (un < " + sz(p_.header_size + p_.trailer_size) + ") " + fail("TRUNCATED"));
        s_.open("{");
        s_.line("uint8_t sb[" + T() + "];");
        s_.line("uint64_t stored;");
        s_.line("size_t k = 0, pn = 0;");
        s_.line("const uint8_t* pp = NULL;");
        s_.line("gmsg_unescape_iter it;");
        s_.line("gmsg_unescape_iter_init(&it, wire, wire_n, " + esc() + ", un - " + T() + ", un);");
        s_.open("while (gmsg_unescape_next(&it, &pp, &pn)) {");
        s_.line("size_t j;");
        s_.line("for (j = 0; j < pn; ++j) sb[k++] = pp[j];");
        s_.close();
        s_.line("stored = gmsg_load_uint(sb, " + T() + ", " + endian_expr(p_.checksum.endian) + ");");
        if (wire_cs_) {
            s_.line("if (stored != computed) " + fail("BAD_CHECKSUM"));
            s_.close();
            return;
        }
        std::string init, upd, fin;
        if (p_.checksum.crc) {
            const auto& q = *p_.checksum.crc;
            auto hex = [](std::uint64_t v) {
                std::ostringstream os;
                os << "UINT64_C(0x" << std::hex << std::uppercase << v << ")";
                return os.str();
            };
            s_.line("const gmsg_crc_params cp = {" + std::to_string(q.width) + ", " + (q.refin ? "true" : "false") + ", " +
                    (q.refout ? "true" : "false") + ", " + hex(q.poly) + ", " + hex(q.init) + ", " + hex(q.xorout) + "};");
            s_.line("uint64_t c = gmsg_crc_init(&cp);");
            upd = "gmsg_crc_update(&cp, c, ";
            fin = "gmsg_crc_finish(&cp, c)";
        } else {
            const auto& a = p_.checksum.algo;
            s_.line(checksum_ctype() + " c = gmsg_" + a + "_init();");
            upd = "gmsg_" + a + "_update(c, ";
            fin = "(uint64_t)gmsg_" + a + "_finish(c)";
        }
        if (p_.checksum.covers == ChecksumSpec::Covers::All) s_.line("c = " + upd + "frame, " + P() + ");");
        const std::string from = p_.checksum.covers == ChecksumSpec::Covers::Body ? H() : "0";
        s_.line("gmsg_unescape_iter_init(&it, wire, wire_n, " + esc() + ", " + from + ", un - " + T() + ");");
        s_.line("while (gmsg_unescape_next(&it, &pp, &pn)) c = " + upd + "pp, pn);");
        s_.line("if (stored != " + fin + ") " + fail("BAD_CHECKSUM"));
        s_.close();
    }

    /// Body shared by open (plain) and split (escaped): sizes, checksum (unless verified before), header, body.
    void emit_split_body(bool with_checksum) {
        if (text_) {
            emit_open_text();
            return;
        }
        if (p_.header_size + p_.trailer_size > 0)
            s_.line("if (interior_n < " + sz(p_.header_size + p_.trailer_size) + ") " + fail("TRUNCATED"));
        if (p_.checksum.present && with_checksum) {
            std::string ptr, len;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: ptr = "interior"; len = "interior_n - " + T(); break;
                case ChecksumSpec::Covers::Body: ptr = "interior + " + H(); len = "interior_n - " + H() + " - " + T(); break;
                case ChecksumSpec::Covers::All: ptr = "frame"; len = P() + " + interior_n - " + T(); break;
            }
            s_.open("{");
            s_.line("const uint64_t stored = gmsg_load_uint(interior + interior_n - " + T() + ", " + T() + ", " +
                    endian_expr(p_.checksum.endian) + ");");
            s_.line("if (stored != " + checksum_call(p_.checksum, ptr, len) + ") " + fail("BAD_CHECKSUM"));
            s_.close();
        }
        if (has_header_) s_.line(try_(hv_ + "_from(interior, " + H() + ", &f->header)"));
        s_.line("f->body = interior + " + H() + ";");
        s_.line("f->body_size = interior_n - " + H() + " - " + T() + ";");
        s_.line("return GMSG_OK;");
    }

    std::string ch(char c) const {
        switch (c) {
            case '\'': return "'\\''";
            case '\\': return "'\\\\'";
            case '\t': return "'\\t'";
            default: return std::string("'") + c + "'";
        }
    }

    /// Text frame: optional "<sep>HH" checksum at the end (hex, upper or lower case), then the type id token.
    void emit_open_text() {
        s_.line("const char* text = (const char*)interior;");
        s_.line("size_t text_n = interior_n;");
        if (p_.checksum.present) {
            const auto n = p_.checksum.size;
            s_.open("{");
            s_.line("size_t star = text_n, k;");
            s_.open("for (k = text_n; k-- > 0;) {");
            s_.open("if (text[k] == " + ch(p_.checksum.separator) + ") {");
            s_.line("star = k;");
            s_.line("break;");
            s_.close();
            s_.close();
            s_.open("if (star == text_n) {");
            if (p_.checksum.required) s_.line(fail("BAD_CHECKSUM"));
            s_.dedent();
            s_.open("} else {");
            s_.line("uint64_t stored = 0;");
            s_.line("if (text_n - star - 1 != " + sz(2 * n) + ") " + fail("BAD_CHECKSUM"));
            s_.open("for (k = 0; k < " + sz(n) + "; ++k) {");
            s_.line("uint8_t b = 0;");
            s_.line("if (gmsg_parse_hex_byte(text + star + 1 + 2 * k, 2, &b) != GMSG_OK) " + fail("BAD_CHECKSUM"));
            s_.line("stored = (stored << 8) | b;");
            s_.close();
            s_.line("if (stored != " + checksum_call(p_.checksum, "(const uint8_t*)text", "star") + ") " + fail("BAD_CHECKSUM"));
            s_.line("text_n = star;");
            s_.close();
            s_.close();
        }
        s_.open("{");
        s_.line("size_t p = 0;");
        s_.line("while (p < text_n && text[p] != " + ch(p_.separator) + ") ++p;");
        s_.line("f->type_id = gmsg_str_make((const uint8_t*)text, p);");
        s_.line("f->body = (const uint8_t*)text + (p < text_n ? p + 1 : text_n);");
        s_.line("f->body_size = p < text_n ? text_n - p - 1 : 0;");
        s_.close();
        s_.line("return GMSG_OK;");
    }

    /// checksum.over: wire - computed over the escaped bytes before unescaping.
    void emit_wire_checksum() {
        s_.line("uint64_t computed = 0;  /* checksum.over: wire */");
        s_.open("{");
        s_.line("const uint8_t* wire = frame + " + P() + ";");
        s_.line("const size_t wire_n = size - " + P() + " - " + S() + ";");
        s_.line("size_t un = 0;");
        s_.line(try_("gmsg_unescaped_size(wire, wire_n, " + esc() + ", &un)"));
        s_.line("if (un < " + sz(p_.header_size + p_.trailer_size) + ") " + fail("TRUNCATED"));
        s_.line("const gmsg_escape_walk t = gmsg_walk_escaped(wire, wire_n, 0, un - " + T() + ", " + esc() + ", 0, NULL, 0);");
        s_.line("if (t.kind != GMSG_WALK_DONE) return t.kind == GMSG_WALK_BAD ? t.error : GMSG_ERR_TRUNCATED;");
        std::string ptr, len;
        switch (p_.checksum.covers) {
            case ChecksumSpec::Covers::HeaderBody: ptr = "wire"; len = "t.wire"; break;
            case ChecksumSpec::Covers::Body:
                s_.line("const gmsg_escape_walk hw = gmsg_walk_escaped(wire, wire_n, 0, " + H() + ", " + esc() + ", 0, NULL, 0);");
                ptr = "wire + hw.wire";
                len = "t.wire - hw.wire";
                break;
            case ChecksumSpec::Covers::All: ptr = "frame"; len = P() + " + t.wire"; break;
        }
        s_.line("computed = " + checksum_call(p_.checksum, ptr, len) + ";");
        s_.close();
    }

    /// Select the message of an opened frame; escaped protocols restore the frame when that fails.
    std::string select_call(const std::string& frame, const std::string& size, const std::string& out) const {
        if (!escape_) return n_ + "_select_(&f, " + out + ")";
        return n_ + "_select_restore_(" + frame + ", " + size + ", &f, " + out + ")";
    }

    void emit_select() {
        s_.line("/* Decode the message view selected by the frame's type id (message_type). */");
        s_.open("static gmsg_error " + n_ + "_select_(const " + n_ + "_frame_* f, " + n_ + "_any_view* out) {");
        auto body = [&](std::size_t i) {
            const auto* m = p_.messages[i];
            s_.line("out->kind = " + kind(*m) + ";");
            if (!p_.exact_size) {
                s_.line("return " + view_name(c_, *m) + "_from(f->body, f->body_size, &out->as." + member(*m) + ");");
                return;
            }
            s_.line("{ const gmsg_error e = " + view_name(c_, *m) + "_from(f->body, f->body_size, &out->as." + member(*m) +
                    "); if (e != GMSG_OK) return e; }");
            s_.line("if (" + view_name(c_, *m) + (m->format == Format::Text ? "_extra_tokens" : "_extra_bytes") + "(&out->as." +
                    member(*m) + ") != 0) return GMSG_ERR_BAD_LENGTH;  /* exact_size */");
            s_.line("return GMSG_OK;");
        };
        switch (p_.message_type.kind) {
            case MessageTypeSpec::Kind::None: body(0); break;
            case MessageTypeSpec::Kind::Field: {
                s_.open("switch ((int64_t)" + hv_ + "_" + p_.message_type.field->name + "(&f->header)) {");
                for (std::size_t i = 0; i < p_.messages.size(); ++i) {
                    s_.open("case " + int_literal(std::get<std::int64_t>(p_.type_ids[i])) + ": {  /* " + p_.messages[i]->name + " */");
                    body(i);
                    s_.close();
                }
                s_.line("default: break;");
                s_.close();
                s_.line("out->kind = " + M_ + "_NONE;");
                s_.line(fail("UNKNOWN_TYPE"));
                break;
            }
            case MessageTypeSpec::Kind::Token:
                for (std::size_t i = 0; i < p_.messages.size(); ++i) {
                    const auto id = std::get<std::string>(p_.type_ids[i]);
                    s_.open("if (f->type_id.size == " + sz(id.size()) + " && gmsg_memcmp(f->type_id.data, " + string_literal(id) +
                            ", " + sz(id.size()) + ") == 0) {");
                    body(i);
                    s_.close();
                }
                s_.line("out->kind = " + M_ + "_NONE;");
                s_.line(fail("UNKNOWN_TYPE"));
                break;
        }
        s_.close();
        s_.line();
        if (escape_) {
            s_.open("static gmsg_error " + n_ + "_select_restore_(uint8_t* frame, size_t size, const " + n_ + "_frame_* f, " + n_ +
                    "_any_view* out) {");
            s_.line("const gmsg_error e = " + n_ + "_select_(f, out);");
            s_.line("if (e != GMSG_OK) " + n_ + "_restore_(frame, size, f->unescaped_n);");
            s_.line("return e;");
            s_.close();
            s_.line();
        }
    }

    void emit_deliver() {
        const auto h = has_header_ ? "&f->header, " : "";
        s_.open("static void " + n_ + "_deliver_(const " + n_ + "_callbacks* cb, const " + n_ + "_frame_* f, const " + n_ +
                "_any_view* v) {");
        if (!has_header_) s_.line("(void)f;");
        s_.open("switch (v->kind) {");
        for (const auto* m : p_.messages) {
            const auto slot = "cb->on_" + snake(m->name);
            s_.line("case " + kind(*m) + ": if (" + slot + ") " + slot + "(cb->ctx, " + h + "&v->as." + member(*m) + "); break;");
        }
        s_.line("default: break;");
        s_.close();
        s_.line("if (cb->on_frame) cb->on_frame(cb->ctx, " + std::string(h) + "v);");
        s_.close();
        s_.line();
        s_.line("/* Validate a complete frame, select its message and deliver it (C++: P::process). */");
        s_.open("static gmsg_error " + n_ + "_process_frame_(" + buf_ + " frame, size_t size, const " + n_ + "_callbacks* cb) {");
        s_.line(n_ + "_frame_ f;");
        s_.line(n_ + "_any_view v;");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line(try_(n_ + "_open_(frame, size, &f)"));
        s_.line(try_(select_call("frame", "size", "&v")));
        s_.line("if (cb) " + n_ + "_deliver_(cb, &f, &v);");
        s_.line("return GMSG_OK;");
        s_.close();
        s_.line();
    }

    /// Scan `data` for one frame; on failure return its error (`size_t frame_n` holds the frame length).
    void scan_one() {
        s_.line("const gmsg_frame_scan s = " + n_ + "_scan_(data, size);");
        s_.line("if (s.kind == GMSG_SCAN_NEED_MORE) " + fail("TRUNCATED"));
        s_.line("if (s.kind == GMSG_SCAN_SKIP) return s.error == GMSG_OK ? GMSG_ERR_BAD_SYNC : s.error;");
    }

    void emit_parse() {
        s_.open("gmsg_error " + n_ + "_parse(" + buf_ + " data, size_t size, " + n_ + "_any_view* out) {");
        s_.line(n_ + "_frame_ f;");
        s_.line("gmsg_error e = GMSG_OK;");
        scan_one();
        s_.line(try_(n_ + "_open_(data, s.size, &f)"));
        s_.line("return " + select_call("data", "s.size", "out") + ";");
        s_.close();
        s_.line();
        if (all_struct_) {
            s_.open("gmsg_error " + n_ + "_decode(" + buf_ + " data, size_t size, " + n_ + "_any_message* out) {");
            s_.line(n_ + "_any_view v;");
            s_.line("gmsg_error e = GMSG_OK;");
            s_.line(try_(n_ + "_parse(data, size, &v)"));
            s_.line("out->kind = v.kind;");
            s_.open("switch (v.kind) {");
            for (const auto* m : p_.messages)
                s_.line("case " + kind(*m) + ": return " + view_name(c_, *m) + "_to_struct(&v.as." + member(*m) + ", &out->as." +
                        member(*m) + ");");
            s_.line("default: " + fail("UNKNOWN_TYPE"));
            s_.close();
            s_.close();
            s_.line();
        }
        s_.open("gmsg_error " + n_ + "_process(" + buf_ + " data, size_t size, const " + n_ + "_callbacks* cb) {");
        scan_one();
        s_.line("return " + n_ + "_process_frame_(data, s.size, cb);");
        s_.close();
        s_.line();
    }

    void emit_stream() {
        const auto st = n_ + "_stream";
        const auto mode = [&](const char* m) { return n_ + "_mode_" + m + "_"; };
        s_.line("/* stream modes: deliver to callbacks (feed / next), or capture for the pull API */");
        s_.line("enum { " + mode("deliver") + " = 0, " + mode("capture") + " = 1, " + mode("message") + " = 2, " + mode("for") + " = 3 };");
        s_.line();
        s_.line("/* Data after the message (bytes, or tokens for text messages). */");
        s_.open("static size_t " + n_ + "_extra_(const " + n_ + "_any_view* v) {");
        s_.open("switch (v->kind) {");
        for (const auto* m : p_.messages)
            s_.line("case " + kind(*m) + ": return " + view_name(c_, *m) + (m->format == Format::Text ? "_extra_tokens" : "_extra_bytes") +
                    "(&v->as." + member(*m) + ");");
        s_.line("default: return 0;");
        s_.close();
        s_.close();
        s_.line();
        s_.open("static gmsg_error " + st + "_process_(gmsg_stream* gs, uint8_t* frame, size_t n) {");
        s_.line(st + "* s = (" + st + "*)gs;");
        s_.line(n_ + "_frame_ f;");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line(try_(n_ + "_open_(frame, n, &f)"));
        s_.open("if (s->mode_ == " + mode("deliver") + ") {");
        s_.line(n_ + "_any_view v;");
        s_.line(try_(select_call("frame", "n", "&v")));
        s_.line("gs->last_extra = " + n_ + "_extra_(&v);");
        s_.line(n_ + "_deliver_(&s->cb, &f, &v);");
        s_.line("return GMSG_OK;");
        s_.close();
        s_.line(try_(select_call("frame", "n", "&s->got_.message")));
        s_.line("gs->last_extra = s->got_.extra = " + n_ + "_extra_(&s->got_.message);");
        if (has_header_) s_.line("s->got_.header = f.header;");
        s_.line("s->got_.bytes = frame;");
        s_.line("s->got_.size = n;");
        s_.open("if (s->mode_ == " + mode("for") + " && s->got_.message.kind != s->want_) {");
        s_.line(n_ + "_deliver_(&s->cb, &f, &s->got_.message);  /* not the awaited message: to the callbacks */");
        s_.line("s->got_.message.kind = " + M_ + "_NONE;");
        s_.close();
        if (all_struct_) {
            s_.open("if (s->mode_ == " + mode("message") + ") {");
            s_.open("switch (s->got_.message.kind) {");
            const std::string ret = escape_ ? "e = " : "return ";
            const std::string brk = escape_ ? " break;" : "";
            for (const auto* m : p_.messages)
                s_.line("case " + kind(*m) + ": s->msg_->kind = " + kind(*m) + "; " + ret + view_name(c_, *m) + "_to_struct(&s->got_.message.as." +
                        member(*m) + ", &s->msg_->as." + member(*m) + ");" + brk);
            s_.line("default: " + (escape_ ? std::string("e = GMSG_ERR_UNKNOWN_TYPE; break;") : fail("UNKNOWN_TYPE")));
            s_.close();
            if (escape_) {
                s_.line("if (e != GMSG_OK) " + n_ + "_restore_(frame, n, f.unescaped_n);  /* a frame that fails is left as received */");
                s_.line("return e;");
            }
            s_.close();
        }
        s_.line("return GMSG_OK;");
        s_.close();
        s_.line();
        // pull API
        s_.open("bool " + st + "_poll(" + st + "* s, " + n_ + "_any_view* out) {");
        s_.line("bool ok;");
        s_.line("s->mode_ = " + mode("capture") + ";");
        s_.line("ok = gmsg_stream_pull(&s->base);");
        s_.line("s->mode_ = " + mode("deliver") + ";");
        s_.line("if (ok) *out = s->got_.message;");
        s_.line("return ok;");
        s_.close();
        s_.line();
        s_.open("bool " + st + "_poll_frame(" + st + "* s, " + n_ + "_received* out) {");
        s_.line("bool ok;");
        s_.line("s->mode_ = " + mode("capture") + ";");
        s_.line("ok = gmsg_stream_pull(&s->base);");
        s_.line("s->mode_ = " + mode("deliver") + ";");
        s_.line("if (ok) *out = s->got_;");
        s_.line("return ok;");
        s_.close();
        s_.line();
        if (all_struct_) {
            s_.open("bool " + st + "_poll_message(" + st + "* s, " + n_ + "_any_message* out) {");
            s_.line("bool ok;");
            s_.line("s->mode_ = " + mode("message") + ";");
            s_.line("s->msg_ = out;");
            s_.line("ok = gmsg_stream_pull(&s->base);");
            s_.line("s->mode_ = " + mode("deliver") + ";");
            s_.line("s->msg_ = NULL;");
            s_.line("return ok;");
            s_.close();
            s_.line();
        }
        s_.open("gmsg_poll " + st + "_poll_event(" + st + "* s, " + n_ + "_event* out) {");
        s_.line("gmsg_step step;");
        s_.line("s->mode_ = " + mode("capture") + ";");
        s_.line("step = gmsg_stream_step(&s->base);");
        s_.line("s->mode_ = " + mode("deliver") + ";");
        s_.line("if (step.kind == GMSG_STEP_NEED_MORE) return GMSG_POLL_NEED_MORE;");
        s_.open("if (step.kind == GMSG_STEP_ERROR) {");
        s_.line("out->message.kind = " + M_ + "_NONE;");
        s_.line("out->error = step.error;");
        s_.line("out->data = step.data;");
        s_.line("out->size = step.size;");
        s_.line("return GMSG_POLL_ERROR;");
        s_.close();
        s_.line("out->message = s->got_.message;");
        s_.line("out->error = GMSG_OK;");
        s_.line("out->data = NULL;");
        s_.line("out->size = 0;");
        s_.line("return GMSG_POLL_MESSAGE;");
        s_.close();
        s_.line();
        s_.open("bool " + st + "_poll_for(" + st + "* s, " + n_ + "_kind kind, " + n_ + "_any_view* out) {");
        s_.line("bool hit = false;");
        s_.line("s->mode_ = " + mode("for") + ";");
        s_.line("s->want_ = kind;");
        s_.open("while (gmsg_stream_pull(&s->base)) {");
        s_.open("if (s->got_.message.kind == kind) {");
        s_.line("*out = s->got_.message;");
        s_.line("hit = true;");
        s_.line("break;");
        s_.close();
        s_.close();
        s_.line("s->mode_ = " + mode("deliver") + ";");
        s_.line("return hit;");
        s_.close();
        s_.line();
        s_.open("static void " + st + "_error_(gmsg_stream* s, gmsg_error e, const uint8_t* data, size_t n) {");
        s_.line("const " + n_ + "_callbacks* cb = &((" + st + "*)s)->cb;");
        s_.line("if (cb->on_error) cb->on_error(cb->ctx, e, data, n);");
        s_.close();
        s_.line();
        s_.open("gmsg_error " + st + "_init(" + st + "* s, uint8_t* buf, size_t size) {");
        s_.line("gmsg_memset(s, 0, sizeof *s);  /* callbacks cleared, deliver mode */");
        s_.line("return gmsg_stream_init(&s->base, buf, size, " + M_ + "_MAX_FRAME, " + n_ + "_scan_, " + st + "_process_, " + st + "_error_);");
        s_.close();
        s_.line();
    }

    // ---- encode -------------------------------------------------------------------------------------
    void emit_encode() {
        for (std::size_t i = 0; i < p_.messages.size(); ++i)
            if (p_.messages[i]->want_struct) emit_encode_message(i);
        if (all_struct_) {
            s_.open("gmsg_error " + n_ + "_encode(const " + n_ + "_any_message* m, uint8_t* out, size_t capacity, size_t* written) {");
            s_.open("switch (m->kind) {");
            for (const auto* x : p_.messages)
                s_.line("case " + kind(*x) + ": return " + n_ + "_encode_" + snake(x->name) + "(&m->as." + member(*x) +
                        ", out, capacity, written);");
            s_.line("default: break;");
            s_.close();
            s_.line("if (written) *written = 0;");
            s_.line(fail("UNKNOWN_TYPE"));
            s_.close();
            s_.line();
        }
    }

    static std::string max_uint(std::size_t n) {
        return n >= 8 ? "UINT64_MAX" : "UINT64_C(" + sz((std::uint64_t{1} << (8 * n)) - 1) + ")";
    }

    void emit_encode_text(std::size_t i) {
        const auto* m = p_.messages[i];
        const auto t = type_name(c_, *m);
        s_.open("gmsg_error " + n_ + "_encode_" + snake(m->name) + "(const " + t + "* m, uint8_t* out, size_t capacity, size_t* written) {");
        s_.line("size_t pos = 0;");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line("if (written) *written = 0;");
        if (p_.prefix_size) {
            s_.line("if (capacity < " + P() + ") " + fail("BUFFER_FULL"));
            s_.line("gmsg_memcpy(out, " + n_ + "_prefix_, " + P() + ");");
            s_.line("pos = " + P() + ";");
        }
        s_.open("{");
        s_.line("gmsg_text_writer w = gmsg_text_writer_init(out + pos, capacity - pos, " + ch(p_.separator) + ");");
        if (p_.message_type.kind == MessageTypeSpec::Kind::Token) {
            const auto id = std::get<std::string>(p_.type_ids[i]);
            s_.line(try_("gmsg_text_token(&w, " + string_literal(id) + ", " + sz(id.size()) + ")"));
        }
        s_.line(try_(t + "_write_text(m, &w)"));
        s_.line("pos += w.pos;");
        s_.close();
        if (p_.checksum.present) {
            const auto n = p_.checksum.size;
            s_.open("{");
            s_.line("const uint64_t cs = " + checksum_call(p_.checksum, "out + " + P(), "pos - " + P()) + ";");
            s_.line("int k;");
            s_.line("if (capacity - pos < " + sz(1 + 2 * n) + ") " + fail("BUFFER_FULL"));
            s_.line("out[pos++] = (uint8_t)" + ch(p_.checksum.separator) + ";");
            s_.open("for (k = " + sz(n - 1) + "; k >= 0; --k) {");
            s_.line("const unsigned b = (unsigned)((cs >> (8 * k)) & 0xFFu);");
            s_.line("out[pos++] = (uint8_t)gmsg_hex_digit(b >> 4, true);");
            s_.line("out[pos++] = (uint8_t)gmsg_hex_digit(b & 0xFu, true);");
            s_.close();
            s_.close();
        }
        if (p_.suffix_size) {
            s_.line("if (capacity - pos < " + S() + ") " + fail("BUFFER_FULL"));
            s_.line("gmsg_memcpy(out + pos, " + n_ + "_suffix_, " + S() + ");");
            s_.line("pos += " + S() + ";");
        }
        s_.line("if (written) *written = pos;");
        s_.line("return GMSG_OK;");
        s_.close();
        s_.line();
    }

    void emit_encode_message(std::size_t i) {
        if (text_) {
            emit_encode_text(i);
            return;
        }
        const auto* m = p_.messages[i];
        const auto& fr = p_.framing;
        const auto t = type_name(c_, *m);
        const auto f = n_ + "_encode_" + snake(m->name);
        if (has_header_) {
            s_.open("gmsg_error " + f + "(const " + t + "* m, uint8_t* out, size_t capacity, size_t* written) {");
            s_.line(ht_ + " h;");
            s_.line("gmsg_memset(&h, 0, sizeof h);");
            s_.line("return " + f + "_with_header(&h, m, out, capacity, written);");
            s_.close();
            s_.line();
            s_.open("gmsg_error " + f + "_with_header(const " + ht_ + "* hdr, const " + t +
                    "* m, uint8_t* out, size_t capacity, size_t* written) {");
            s_.line(ht_ + " h = *hdr;");
        } else {
            s_.open("gmsg_error " + f + "(const " + t + "* m, uint8_t* out, size_t capacity, size_t* written) {");
        }
        s_.line("gmsg_writer w = gmsg_writer_init(out, capacity);");
        s_.line("gmsg_error e = GMSG_OK;");
        s_.line("if (written) *written = 0;");
        if (p_.prefix_size) s_.line(try_("gmsg_write_bytes(&w, " + n_ + "_prefix_, " + P() + ")"));
        s_.line("const size_t header_at = w.pos;");
        s_.line("(void)header_at;");
        if (has_header_) {
            if (p_.message_type.kind == MessageTypeSpec::Kind::Field) {
                const auto& df = *p_.message_type.field;
                if (!df.const_value)
                    s_.line("h." + ident(df.name) + " = " + typed_literal(c_, df, std::get<std::int64_t>(p_.type_ids[i])) + ";");
            }
            s_.line(try_(ht_ + "_write(&h, &w)"));
        }
        s_.line("const size_t body_at = w.pos;");
        s_.line(try_(t + "_write(m, &w)"));
        s_.line("const size_t body_len = w.pos - body_at;");
        s_.line("(void)body_len;");
        if (fr.type == FT::Fixed) {
            const auto body = *fr.size - (p_.prefix_size + p_.header_size + p_.trailer_size + p_.suffix_size);
            s_.line("if (body_len > " + sz(body) + ") " + fail("BAD_LENGTH"));
            s_.line(try_("gmsg_write_fill(&w, 0, " + sz(body) + " - body_len)"));
        }
        if (fr.type == FT::LengthPrefixed) {
            const auto n = prim_size(fr.length->prim);
            std::string covered;
            switch (fr.length_covers) {
                case FramingSpec::LengthCovers::Body:
                    covered = fr.length_counts_wire ? "body_len + gmsg_escape_overhead(out + body_at, body_len, " + esc() + ")" : "body_len";
                    break;
                case FramingSpec::LengthCovers::BodyChecksum:
                    covered = fr.length_counts_wire ? "body_wire + trailer_wire" : "body_len + " + T();
                    break;
                case FramingSpec::LengthCovers::HeaderBody: covered = H() + " + body_len"; break;
                case FramingSpec::LengthCovers::All: covered = P() + " + " + H() + " + body_len + " + T(); break;
            }
            s_.open("{");
            if (fr.length_counts_wire && fr.length_covers == FramingSpec::LengthCovers::BodyChecksum) emit_wire_trailer_size();
            s_.line("const int64_t value = (int64_t)(" + covered + ") - (" + int_literal(fr.length_adjust) + ");");
            s_.line("if (value < 0 || (uint64_t)value > " + max_uint(n) + ") " + fail("BAD_LENGTH"));
            s_.line("gmsg_write_patch_uint(&w, header_at + " + sz(*fr.length->static_offset) + ", " + sz(n) + ", " +
                    endian_expr(fr.length->endian) + ", (uint64_t)value);");
            s_.close();
        }
        if (p_.checksum.present && !wire_cs_) {
            std::string ptr, len;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: ptr = "out + header_at"; len = "w.pos - header_at"; break;
                case ChecksumSpec::Covers::Body: ptr = "out + body_at"; len = "w.pos - body_at"; break;
                case ChecksumSpec::Covers::All: ptr = "out"; len = "w.pos"; break;
            }
            s_.line("const uint64_t cs = " + checksum_call(p_.checksum, ptr, len) + ";");
            s_.line(try_("gmsg_write_uint(&w, " + T() + ", " + endian_expr(p_.checksum.endian) + ", cs)"));
        }
        s_.line("size_t total = w.pos;");
        if (wire_cs_) {
            if (p_.checksum.covers == ChecksumSpec::Covers::Body)
                s_.line("const size_t body_wire_at = body_at + gmsg_escape_overhead(out + " + P() + ", body_at - " + P() + ", " + esc() + ");");
            s_.open("{");
            s_.line("size_t esc_n = 0;");
            s_.line(try_("gmsg_escape_in_place(out, capacity, " + P() + ", total - " + P() + ", " + esc() + ", &esc_n)"));
            s_.line("total = " + P() + " + esc_n;");
            s_.close();
            std::string ptr, len;
            switch (p_.checksum.covers) {
                case ChecksumSpec::Covers::HeaderBody: ptr = "out + " + P(); len = "total - " + P(); break;
                case ChecksumSpec::Covers::Body: ptr = "out + body_wire_at"; len = "total - body_wire_at"; break;
                case ChecksumSpec::Covers::All: ptr = "out"; len = "total"; break;
            }
            s_.line("const uint64_t cs = " + checksum_call(p_.checksum, ptr, len) + ";  /* over the wire bytes */");
            s_.line("if (capacity - total < " + T() + ") " + fail("BUFFER_FULL"));
            s_.line("gmsg_store_uint(out + total, " + T() + ", cs, " + endian_expr(p_.checksum.endian) + ");");
            s_.open("{");
            s_.line("size_t esc_n = 0;");
            s_.line(try_("gmsg_escape_in_place(out, capacity, total, " + T() + ", " + esc() + ", &esc_n)"));
            s_.line("total += esc_n;");
            s_.close();
        } else if (escape_) {
            s_.open("{");
            s_.line("size_t esc_n = 0;");
            s_.line(try_("gmsg_escape_in_place(out, capacity, " + P() + ", total - " + P() + ", " + esc() + ", &esc_n)"));
            s_.line("total = " + P() + " + esc_n;");
            s_.close();
        }
        if (p_.suffix_size) {
            s_.line("if (capacity - total < " + S() + ") " + fail("BUFFER_FULL"));
            s_.line("gmsg_memcpy(out + total, " + n_ + "_suffix_, " + S() + ");");
            s_.line("total += " + S() + ";");
        }
        s_.line("if (written) *written = total;");
        s_.line("return GMSG_OK;");
        s_.close();
        s_.line();
    }

    /// length.counts: wire + covers: body_checksum (see the C++ backend).
    void emit_wire_trailer_size() {
        s_.line("/* the length counts the escaped body and the escaped checksum */");
        s_.line("const size_t body_wire = body_len + gmsg_escape_overhead(out + body_at, body_len, " + esc() + ");");
        if (!p_.checksum.present) {
            s_.line("const size_t trailer_wire = 0;");
            return;
        }
        if (wire_cs_) {
            s_.line("uint64_t cs0 = 0;");
            s_.open("{");
            s_.line("size_t en = 0, un = 0;");
            s_.line(try_("gmsg_escape_in_place(out, capacity, body_at, body_len, " + esc() + ", &en)"));
            s_.line("cs0 = " + checksum_call(p_.checksum, "out + body_at", "en") + ";");
            s_.line("(void)gmsg_unescape_in_place(out + body_at, en, " + esc() + ", &un);");
            s_.close();
        } else {
            s_.line("const uint64_t cs0 = " + checksum_call(p_.checksum, "out + body_at", "body_len") + ";");
        }
        s_.line("uint8_t cs_bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};");
        s_.line("gmsg_store_uint(cs_bytes, " + T() + ", cs0, " + endian_expr(p_.checksum.endian) + ");");
        s_.line("const size_t trailer_wire = " + T() + " + gmsg_escape_overhead(cs_bytes, " + T() + ", " + esc() + ");");
    }

    Ctx& c_;
    CodeWriter& h_;
    CodeWriter& s_;
    const Protocol& p_;
    bool text_ = false, has_header_ = false, escape_ = false, wire_cs_ = false, cs_first_ = false, stream_ = false,
         all_struct_ = false;
    std::string n_, M_, hv_, ht_, buf_, cfg_;
};

} // namespace

bool protocol_supported(const Protocol& p) {
    (void)p;
    return true;
}

void emit_protocol(Ctx& c, const Protocol& p) {
    if (!p.header->fields.empty()) emit_binary_message(c, *p.header);
    ProtocolEmitter(c, p).emit();
}

} // namespace gmpc::c
