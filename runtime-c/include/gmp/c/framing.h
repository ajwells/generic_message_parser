/* gmp/c/framing.h - frame scanning and byte stuffing (ports of the gmp/framing headers; same results, checked by tests).
 *
 * A scanner looks at the bytes at the current stream position and returns a gmsg_frame_scan:
 *   GMSG_SCAN_FRAME      a complete frame of `size` bytes; consume `consume` bytes after it was processed,
 *                        or `consume_on_error` when it failed validation (resync)
 *   GMSG_SCAN_SKIP       discard `size` bytes; `error` is the reason (GMSG_OK = silent skip)
 *   GMSG_SCAN_NEED_MORE  wait for more input (`size` is a hint, 0 = unknown)
 *
 * Scanners: gmsg_scan_fixed, gmsg_scan_length_prefixed, gmsg_scan_delimited, each with a config struct that
 * generated code defines as a static constant.
 *
 * Byte stuffing (gmsg_escape_config): each reserved byte b is sent as {esc, code(b)}; xor style: code = b ^ mask,
 * table style: code from raw[] / code[]. `mask` is a 256-bit set of the bytes that must be escaped (raw bytes + esc).
 */
#ifndef GMSG_C_FRAMING_H
#define GMSG_C_FRAMING_H

#include "gmp/c/config.h"
#include "gmp/c/endian.h"
#include "gmp/c/error.h"

GMSG_C_BEGIN

/* ---- scan result -------------------------------------------------------------------------------- */

typedef enum gmsg_scan_kind { GMSG_SCAN_NEED_MORE = 0, GMSG_SCAN_FRAME = 1, GMSG_SCAN_SKIP = 2 } gmsg_scan_kind;

typedef struct gmsg_frame_scan {
    gmsg_scan_kind kind;
    size_t size;              /* frame: frame length; skip: bytes to discard; need_more: hint */
    size_t consume;           /* frame: bytes to consume after a successfully processed frame */
    size_t consume_on_error;  /* frame: bytes to consume when validation fails */
    gmsg_error error;         /* skip: reason (GMSG_OK = silent) */
} gmsg_frame_scan;

GMSG_API gmsg_frame_scan gmsg_scan_need_more(size_t hint);
GMSG_API gmsg_frame_scan gmsg_scan_frame(size_t n, size_t consume, size_t consume_on_error);
GMSG_API gmsg_frame_scan gmsg_scan_skip(size_t n, gmsg_error e);

/* Fixed byte pattern (sync word or delimiter), up to 8 bytes. */
typedef struct gmsg_pattern {
    uint8_t bytes[8];
    uint8_t len;
} gmsg_pattern;

/* Locate `pat` at the start of buf: *matched = true when it is there (or pat is empty); otherwise need_more
 * (buf is a strict prefix of pat) or skip to the first possible match. */
GMSG_API gmsg_frame_scan gmsg_find_pattern_at_start(const uint8_t* buf, size_t n, const gmsg_pattern* pat, bool* matched);
GMSG_API bool gmsg_matches_at(const uint8_t* buf, size_t n, size_t at, const gmsg_pattern* pat);

/* ---- byte stuffing ------------------------------------------------------------------------------ */

#define GMSG_ESCAPE_MAX_RESERVED 16
typedef enum gmsg_escape_style { GMSG_ESCAPE_XOR = 0, GMSG_ESCAPE_TABLE = 1 } gmsg_escape_style;

typedef struct gmsg_escape_config {
    bool enabled;
    uint8_t esc;
    uint8_t style;     /* gmsg_escape_style */
    uint8_t xor_mask;
    uint8_t count;     /* entries used in raw / code */
    uint8_t raw[GMSG_ESCAPE_MAX_RESERVED];
    uint8_t code[GMSG_ESCAPE_MAX_RESERVED];
    uint32_t mask[8];  /* 256-bit set: raw bytes + esc */
} gmsg_escape_config;

/* True for bytes that must not appear unescaped (the reserved bytes and the escape byte). */
GMSG_API bool gmsg_escape_needs(const gmsg_escape_config* c, uint8_t b);
GMSG_API uint8_t gmsg_escape_encode(const gmsg_escape_config* c, uint8_t b);
GMSG_API gmsg_error gmsg_escape_decode(const gmsg_escape_config* c, uint8_t code, uint8_t* out);

/* Unescape buf[0..n) in place; *out_n receives the new length. */
GMSG_API gmsg_error gmsg_unescape_in_place(uint8_t* buf, size_t n, const gmsg_escape_config* c, size_t* out_n);

/* Number of bytes produced by unescaping wire[0..n). */
GMSG_API gmsg_error gmsg_unescaped_size(const uint8_t* wire, size_t n, const gmsg_escape_config* c, size_t* out_n);

/* Check the escape pairs of wire[0..n) without modifying it: the same errors as gmsg_unescape_in_place, in the same
 * order. *out_n receives the unescaped size. */
GMSG_API gmsg_error gmsg_check_escaped(const uint8_t* wire, size_t n, const gmsg_escape_config* c, size_t* out_n);

/* Iterates over the unescaped bytes with index in [from, to) of wire[0..n) without modifying it: runs of plain bytes
 * point into wire, each escape pair yields its decoded byte. wire must pass gmsg_check_escaped. Used to verify a
 * checksum before a frame is unescaped in place. */
typedef struct gmsg_unescape_iter {
    const uint8_t* wire;
    size_t n, i, u, from, to;
    const gmsg_escape_config* c;
    uint8_t byte;
} gmsg_unescape_iter;

GMSG_API void gmsg_unescape_iter_init(gmsg_unescape_iter* it, const uint8_t* wire, size_t n,
                                      const gmsg_escape_config* c, size_t from, size_t to);
/* Next piece: true with *p / *len set, false at the end. */
GMSG_API bool gmsg_unescape_next(gmsg_unescape_iter* it, const uint8_t** p, size_t* len);

/* Number of escape pairs needed for raw[0..n). */
GMSG_API size_t gmsg_escape_overhead(const uint8_t* raw, size_t n, const gmsg_escape_config* c);

/* Escape buf[at..at+len) in place, expanding towards buf[cap). *out_len receives the escaped length. */
GMSG_API gmsg_error gmsg_escape_in_place(uint8_t* buf, size_t cap, size_t at, size_t len, const gmsg_escape_config* c,
                                         size_t* out_len);

typedef enum gmsg_walk_kind { GMSG_WALK_DONE = 0, GMSG_WALK_NEED_MORE = 1, GMSG_WALK_BAD = 2 } gmsg_walk_kind;

typedef struct gmsg_escape_walk {
    gmsg_walk_kind kind;
    size_t wire;       /* done: wire position after the n-th unescaped byte; bad: offending position */
    gmsg_error error;  /* bad: reason */
} gmsg_escape_walk;

/* Walk the escaped bytes of buf[0..size) from `at` until `n` unescaped bytes have been produced. Unescaped bytes
 * with index in [pick_at, pick_at + pick_n) are copied to pick (may be NULL when pick_n is 0). An unescaped reserved
 * byte is reported as GMSG_WALK_BAD. */
GMSG_API gmsg_escape_walk gmsg_walk_escaped(const uint8_t* buf, size_t size, size_t at, size_t n,
                                            const gmsg_escape_config* c, size_t pick_at, uint8_t* pick, size_t pick_n);

/* Copy the n header bytes that follow the sync pattern (`prefix` bytes) at the start of buf[0..size) into out,
 * unescaped when `escape` is enabled (NULL: no escaping). False while they have not all arrived, or when the escaping
 * is invalid (the scanner reports that). Used by generated scanners to check the length against the message type. */
GMSG_API bool gmsg_read_frame_header(const uint8_t* buf, size_t size, size_t prefix, const gmsg_escape_config* escape,
                                     uint8_t* out, size_t n);

/* ---- fixed-size frames -------------------------------------------------------------------------- */

typedef struct gmsg_fixed_config {
    gmsg_pattern sync;
    size_t frame_size;           /* unescaped */
    gmsg_escape_config escape;   /* optional stuffing of everything between sync and suffix */
    size_t suffix_len;           /* trailing bytes that are not escaped */
} gmsg_fixed_config;

GMSG_API gmsg_frame_scan gmsg_scan_fixed(const uint8_t* buf, size_t n, const gmsg_fixed_config* cfg);

/* ---- length-prefixed frames --------------------------------------------------------------------- */

typedef struct gmsg_length_prefixed_config {
    gmsg_pattern sync;
    size_t length_offset;        /* byte offset of the length field from frame start (unescaped) */
    uint8_t length_size;         /* 1..8 bytes */
    gmsg_endian length_endian;
    int64_t add;                 /* total = value + add */
    size_t min_frame;
    size_t max_frame;
    gmsg_escape_config escape;
    size_t suffix_len;
    bool counts_wire;            /* escape: the length is the body size on the wire */
    size_t header_len;           /* counts_wire: unescaped header size */
    size_t trailer_len;          /* counts_wire: unescaped trailer size */
    bool wire_includes_trailer;  /* counts_wire: the value also counts the escaped trailer */
} gmsg_length_prefixed_config;

GMSG_API gmsg_frame_scan gmsg_scan_length_prefixed(const uint8_t* buf, size_t n, const gmsg_length_prefixed_config* cfg);

/* ---- delimited frames --------------------------------------------------------------------------- */

typedef struct gmsg_delimited_config {
    gmsg_pattern start;          /* optional (len 0 = frame begins at the stream position) */
    gmsg_pattern end;            /* required */
    gmsg_escape_config escape;   /* optional; escaped bytes are skipped while searching for `end` */
    size_t max_frame;
} gmsg_delimited_config;

GMSG_API gmsg_frame_scan gmsg_scan_delimited(const uint8_t* buf, size_t n, const gmsg_delimited_config* cfg);

/* ---- implementation (header-only build, or runtime-c/src/framing.c of the library build) ------------------------------- */

#if GMSG_DEFINE_FUNCTIONS_ || defined(GMSG_IMPL_FRAMING_)

GMSG_API gmsg_frame_scan gmsg_scan_need_more(size_t hint) {
    gmsg_frame_scan s;
    s.kind = GMSG_SCAN_NEED_MORE;
    s.size = hint;
    s.consume = 0;
    s.consume_on_error = 0;
    s.error = GMSG_OK;
    return s;
}

GMSG_API gmsg_frame_scan gmsg_scan_frame(size_t n, size_t consume, size_t consume_on_error) {
    gmsg_frame_scan s;
    s.kind = GMSG_SCAN_FRAME;
    s.size = n;
    s.consume = consume;
    s.consume_on_error = consume_on_error;
    s.error = GMSG_OK;
    return s;
}

GMSG_API gmsg_frame_scan gmsg_scan_skip(size_t n, gmsg_error e) {
    gmsg_frame_scan s;
    s.kind = GMSG_SCAN_SKIP;
    s.size = n;
    s.consume = 0;
    s.consume_on_error = 0;
    s.error = e;
    return s;
}

GMSG_API gmsg_frame_scan gmsg_find_pattern_at_start(const uint8_t* buf, size_t n, const gmsg_pattern* pat, bool* matched) {
    size_t i, k;
    *matched = false;
    if (pat->len == 0) {
        *matched = true;
        return gmsg_scan_need_more(0);
    }
    for (i = 0; i < n; ++i) {
        const size_t avail = pat->len < n - i ? pat->len : n - i;
        bool ok = true;
        for (k = 0; k < avail; ++k) {
            if (buf[i + k] != pat->bytes[k]) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        if (i > 0) return gmsg_scan_skip(i, GMSG_ERR_BAD_SYNC);
        if (avail < pat->len) return gmsg_scan_need_more(pat->len);
        *matched = true;
        return gmsg_scan_need_more(0);
    }
    return n == 0 ? gmsg_scan_need_more(pat->len) : gmsg_scan_skip(n, GMSG_ERR_BAD_SYNC);
}

GMSG_API bool gmsg_matches_at(const uint8_t* buf, size_t n, size_t at, const gmsg_pattern* pat) {
    size_t k;
    if (at + pat->len > n) return false;
    for (k = 0; k < pat->len; ++k)
        if (buf[at + k] != pat->bytes[k]) return false;
    return true;
}

GMSG_API bool gmsg_escape_needs(const gmsg_escape_config* c, uint8_t b) {
    return ((c->mask[b >> 5] >> (b & 31u)) & 1u) != 0;
}

GMSG_API uint8_t gmsg_escape_encode(const gmsg_escape_config* c, uint8_t b) {
    size_t i;
    if (c->style == GMSG_ESCAPE_TABLE)
        for (i = 0; i < c->count; ++i)
            if (c->raw[i] == b) return c->code[i];
    return (uint8_t)(b ^ c->xor_mask);
}

GMSG_API gmsg_error gmsg_escape_decode(const gmsg_escape_config* c, uint8_t code, uint8_t* out) {
    size_t i;
    if (c->style == GMSG_ESCAPE_TABLE) {
        for (i = 0; i < c->count; ++i)
            if (c->code[i] == code) {
                *out = c->raw[i];
                return GMSG_OK;
            }
        return GMSG_ERR_INVALID_VALUE;
    }
    *out = (uint8_t)(code ^ c->xor_mask);
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_unescape_in_place(uint8_t* buf, size_t n, const gmsg_escape_config* c, size_t* out_n) {
    size_t r, w = 0;
    for (r = 0; r < n; ++r) {
        uint8_t b = buf[r];
        if (b == c->esc) {
            gmsg_error e;
            if (++r >= n) return GMSG_ERR_TRUNCATED;
            e = gmsg_escape_decode(c, buf[r], &b);
            if (e != GMSG_OK) return e;
        }
        buf[w++] = b;
    }
    *out_n = w;
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_unescaped_size(const uint8_t* wire, size_t n, const gmsg_escape_config* c, size_t* out_n) {
    size_t i, k = 0;
    for (i = 0; i < n; ++i, ++k)
        if (wire[i] == c->esc && ++i >= n) return GMSG_ERR_TRUNCATED;
    *out_n = k;
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_check_escaped(const uint8_t* wire, size_t n, const gmsg_escape_config* c, size_t* out_n) {
    size_t i, k = 0;
    for (i = 0; i < n; ++i, ++k) {
        uint8_t b;
        gmsg_error e;
        if (wire[i] != c->esc) continue;
        if (++i >= n) return GMSG_ERR_TRUNCATED;
        e = gmsg_escape_decode(c, wire[i], &b);
        if (e != GMSG_OK) return e;
    }
    *out_n = k;
    return GMSG_OK;
}

GMSG_API void gmsg_unescape_iter_init(gmsg_unescape_iter* it, const uint8_t* wire, size_t n,
                                      const gmsg_escape_config* c, size_t from, size_t to) {
    it->wire = wire;
    it->n = n;
    it->i = 0;
    it->u = 0;
    it->from = from;
    it->to = to;
    it->c = c;
    it->byte = 0;
}

GMSG_PRIV_ bool gmsg_unescape_pair_at_(const gmsg_unescape_iter* it) {
    return it->wire[it->i] == it->c->esc && it->i + 1 < it->n;
}

GMSG_API bool gmsg_unescape_next(gmsg_unescape_iter* it, const uint8_t** p, size_t* len) {
    size_t start;
    while (it->u < it->from && it->u < it->to && it->i < it->n) { /* skip to index `from` */
        it->i += gmsg_unescape_pair_at_(it) ? 2u : 1u;
        ++it->u;
    }
    if (it->u >= it->to || it->i >= it->n) return false;
    if (gmsg_unescape_pair_at_(it)) {
        if (gmsg_escape_decode(it->c, it->wire[it->i + 1], &it->byte) != GMSG_OK) it->byte = 0;
        it->i += 2;
        ++it->u;
        *p = &it->byte;
        *len = 1;
        return true;
    }
    start = it->i;
    while (it->i < it->n && it->u < it->to && !gmsg_unescape_pair_at_(it)) {
        ++it->i;
        ++it->u;
    }
    *p = it->wire + start;
    *len = it->i - start;
    return true;
}

GMSG_API bool gmsg_read_frame_header(const uint8_t* buf, size_t size, size_t prefix, const gmsg_escape_config* escape,
                                     uint8_t* out, size_t n) {
    if (escape == NULL || !escape->enabled) {
        size_t i;
        if (size < prefix + n) return false;
        for (i = 0; i < n; ++i) out[i] = buf[prefix + i];
        return true;
    }
    return gmsg_walk_escaped(buf, size, prefix, n, escape, 0, out, n).kind == GMSG_WALK_DONE;
}

GMSG_API size_t gmsg_escape_overhead(const uint8_t* raw, size_t n, const gmsg_escape_config* c) {
    size_t i, k = 0;
    for (i = 0; i < n; ++i) k += gmsg_escape_needs(c, raw[i]) ? 1u : 0u;
    return k;
}

GMSG_API gmsg_error gmsg_escape_in_place(uint8_t* buf, size_t cap, size_t at, size_t len, const gmsg_escape_config* c,
                                         size_t* out_len) {
    const size_t extra = gmsg_escape_overhead(buf + at, len, c);
    size_t w, r;
    if (at + len + extra > cap) return GMSG_ERR_BUFFER_FULL;
    w = at + len + extra;
    for (r = at + len; r-- > at;) {
        const uint8_t b = buf[r];
        if (gmsg_escape_needs(c, b)) {
            buf[--w] = gmsg_escape_encode(c, b);
            buf[--w] = c->esc;
        } else {
            buf[--w] = b;
        }
    }
    *out_len = len + extra;
    return GMSG_OK;
}

GMSG_PRIV_ gmsg_escape_walk gmsg_walk_result_(gmsg_walk_kind k, size_t wire, gmsg_error e) {
    gmsg_escape_walk w;
    w.kind = k;
    w.wire = wire;
    w.error = e;
    return w;
}

GMSG_API gmsg_escape_walk gmsg_walk_escaped(const uint8_t* buf, size_t size, size_t at, size_t n,
                                            const gmsg_escape_config* c, size_t pick_at, uint8_t* pick, size_t pick_n) {
    size_t i = at, u;
    for (u = 0; u < n; ++u) {
        uint8_t b;
        if (i >= size) return gmsg_walk_result_(GMSG_WALK_NEED_MORE, i, GMSG_OK);
        b = buf[i];
        if (b == c->esc) {
            if (i + 1 >= size) return gmsg_walk_result_(GMSG_WALK_NEED_MORE, i, GMSG_OK);
            if (gmsg_escape_decode(c, buf[i + 1], &b) != GMSG_OK) return gmsg_walk_result_(GMSG_WALK_BAD, i, GMSG_ERR_INVALID_VALUE);
            i += 2;
        } else if (gmsg_escape_needs(c, b)) {
            return gmsg_walk_result_(GMSG_WALK_BAD, i, GMSG_ERR_BAD_DELIMITER);
        } else {
            ++i;
        }
        if (u >= pick_at && u - pick_at < pick_n) pick[u - pick_at] = b;
    }
    return gmsg_walk_result_(GMSG_WALK_DONE, i, GMSG_OK);
}

GMSG_API gmsg_frame_scan gmsg_scan_fixed(const uint8_t* buf, size_t n, const gmsg_fixed_config* cfg) {
    bool matched = false;
    const gmsg_frame_scan r = gmsg_find_pattern_at_start(buf, n, &cfg->sync, &matched);
    size_t s, total;
    gmsg_escape_walk wk;
    if (!matched) return r;
    if (!cfg->escape.enabled) {
        if (n < cfg->frame_size) return gmsg_scan_need_more(cfg->frame_size);
        return gmsg_scan_frame(cfg->frame_size, cfg->frame_size, cfg->sync.len ? 1u : cfg->frame_size);
    }
    s = cfg->sync.len;
    wk = gmsg_walk_escaped(buf, n, s, cfg->frame_size - s - cfg->suffix_len, &cfg->escape, 0, NULL, 0);
    if (wk.kind == GMSG_WALK_NEED_MORE) return gmsg_scan_need_more(0);
    if (wk.kind == GMSG_WALK_BAD) return gmsg_scan_skip(wk.wire > 0 ? wk.wire : 1, wk.error);
    total = wk.wire + cfg->suffix_len;
    if (n < total) return gmsg_scan_need_more(total);
    return gmsg_scan_frame(total, total, s ? 1u : total);
}

GMSG_PRIV_ gmsg_frame_scan gmsg_scan_length_prefixed_wire_(const uint8_t* buf, size_t n, const gmsg_length_prefixed_config* cfg) {
    const size_t s = cfg->sync.len;
    uint8_t lb[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    gmsg_escape_walk hw = gmsg_walk_escaped(buf, n, s, cfg->header_len, &cfg->escape, cfg->length_offset - s, lb,
                                            cfg->length_size);
    uint64_t value;
    int64_t body;
    size_t body_end, end, total, i;
    if (hw.kind == GMSG_WALK_NEED_MORE) return gmsg_scan_need_more(0);
    if (hw.kind == GMSG_WALK_BAD) return gmsg_scan_skip(hw.wire > 0 ? hw.wire : 1, hw.error);
    value = gmsg_load_uint(lb, cfg->length_size, cfg->length_endian);
    body = (int64_t)value + cfg->add;
    if (body < 0 || (uint64_t)hw.wire + (uint64_t)body + cfg->suffix_len > cfg->max_frame) return gmsg_scan_skip(1, GMSG_ERR_BAD_LENGTH);
    body_end = hw.wire + (size_t)body;
    for (i = hw.wire; i < body_end; ++i) {
        if (i >= n) return gmsg_scan_need_more(body_end);
        if (buf[i] == cfg->escape.esc) {
            if (i + 1 >= body_end) return gmsg_scan_skip(1, GMSG_ERR_BAD_LENGTH);
            ++i;
        } else if (gmsg_escape_needs(&cfg->escape, buf[i])) {
            return gmsg_scan_skip(i, GMSG_ERR_BAD_DELIMITER);
        }
    }
    end = body_end;
    if (!cfg->wire_includes_trailer) {
        const gmsg_escape_walk tw = gmsg_walk_escaped(buf, n, body_end, cfg->trailer_len, &cfg->escape, 0, NULL, 0);
        if (tw.kind == GMSG_WALK_NEED_MORE) return gmsg_scan_need_more(0);
        if (tw.kind == GMSG_WALK_BAD) return gmsg_scan_skip(tw.wire, tw.error);
        end = tw.wire;
    }
    total = end + cfg->suffix_len;
    if (total < cfg->min_frame || total > cfg->max_frame) return gmsg_scan_skip(1, GMSG_ERR_BAD_LENGTH);
    if (n < total) return gmsg_scan_need_more(total);
    return gmsg_scan_frame(total, total, 1);
}

GMSG_API gmsg_frame_scan gmsg_scan_length_prefixed(const uint8_t* buf, size_t n, const gmsg_length_prefixed_config* cfg) {
    bool matched = false;
    const gmsg_frame_scan r = gmsg_find_pattern_at_start(buf, n, &cfg->sync, &matched);
    const size_t need = cfg->length_offset + cfg->length_size;
    uint64_t value = 0;
    int64_t total;
    size_t len;
    if (!matched) return r;
    if (cfg->escape.enabled && cfg->counts_wire) return gmsg_scan_length_prefixed_wire_(buf, n, cfg);
    if (!cfg->escape.enabled) {
        if (n < need) return gmsg_scan_need_more(need);
        value = gmsg_load_uint(buf + cfg->length_offset, cfg->length_size, cfg->length_endian);
    } else {
        uint8_t lb[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const size_t s = cfg->sync.len;
        const gmsg_escape_walk wk = gmsg_walk_escaped(buf, n, s, need - s, &cfg->escape, cfg->length_offset - s, lb,
                                                      cfg->length_size);
        if (wk.kind == GMSG_WALK_NEED_MORE) return gmsg_scan_need_more(0);
        if (wk.kind == GMSG_WALK_BAD) return gmsg_scan_skip(wk.wire > 0 ? wk.wire : 1, wk.error);
        value = gmsg_load_uint(lb, cfg->length_size, cfg->length_endian);
    }
    total = (int64_t)value + cfg->add;
    if (total < (int64_t)cfg->min_frame || total > (int64_t)cfg->max_frame || total <= 0) return gmsg_scan_skip(1, GMSG_ERR_BAD_LENGTH);
    len = (size_t)total;
    if (cfg->escape.enabled) {
        const size_t s = cfg->sync.len;
        gmsg_escape_walk wk;
        if (len < s + cfg->suffix_len) return gmsg_scan_skip(1, GMSG_ERR_BAD_LENGTH);
        wk = gmsg_walk_escaped(buf, n, s, len - s - cfg->suffix_len, &cfg->escape, 0, NULL, 0);
        if (wk.kind == GMSG_WALK_NEED_MORE) {
            if (wk.wire > cfg->max_frame) return gmsg_scan_skip(1, GMSG_ERR_BAD_LENGTH);
            return gmsg_scan_need_more(0);
        }
        if (wk.kind == GMSG_WALK_BAD) return gmsg_scan_skip(wk.wire > 0 ? wk.wire : 1, wk.error);
        len = wk.wire + cfg->suffix_len;
        if (len > cfg->max_frame) return gmsg_scan_skip(1, GMSG_ERR_BAD_LENGTH);
    }
    if (n < len) return gmsg_scan_need_more(len);
    return gmsg_scan_frame(len, len, 1);
}

GMSG_API gmsg_frame_scan gmsg_scan_delimited(const uint8_t* buf, size_t n, const gmsg_delimited_config* cfg) {
    bool matched = false;
    const gmsg_frame_scan r = gmsg_find_pattern_at_start(buf, n, &cfg->start, &matched);
    bool shared_flag;
    size_t s, i;
    if (!matched) return r;
    shared_flag = cfg->start.len > 0 && cfg->start.len == cfg->end.len && gmsg_matches_at(cfg->end.bytes, cfg->end.len, 0, &cfg->start);
    s = cfg->start.len;
    for (i = s; i < n; ++i) {
        if (i + 1 > cfg->max_frame) return gmsg_scan_skip(s ? s : i, GMSG_ERR_BAD_LENGTH);
        if (cfg->escape.enabled && buf[i] == cfg->escape.esc) {
            ++i;
            continue;
        }
        if (gmsg_matches_at(buf, n, i, &cfg->end)) {
            const size_t total = i + cfg->end.len;
            if (shared_flag) {
                if (i == s) return gmsg_scan_skip(s, GMSG_OK); /* back-to-back flags: drop one */
                return gmsg_scan_frame(total, total - cfg->end.len, total - cfg->end.len);
            }
            return gmsg_scan_frame(total, total, total);
        }
        if (!shared_flag && s && gmsg_matches_at(buf, n, i, &cfg->start)) return gmsg_scan_skip(i, GMSG_ERR_BAD_DELIMITER);
    }
    if (n >= cfg->max_frame) return gmsg_scan_skip(s ? s : n, GMSG_ERR_BAD_LENGTH);
    return gmsg_scan_need_more(0);
}

#endif /* implementation */

GMSG_C_END

#endif /* GMSG_C_FRAMING_H */
