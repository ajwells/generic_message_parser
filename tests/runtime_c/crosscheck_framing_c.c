/* C side of the framing / stream cross-check (compiled as C11); see test_c_vs_cpp.cpp. */
#include "crosscheck_c.h"

#include "gmp/c/runtime.h"

static void reserve(gmsg_escape_config* c, uint8_t b) { c->mask[b >> 5] |= (uint32_t)1 << (b & 31u); }

/* Same construction as gmsg::make_xor_escape / make_index_escape / make_map_escape. */
static gmsg_escape_config make_xor(uint8_t esc, uint8_t xm, const uint8_t* reserved, size_t n) {
    gmsg_escape_config c;
    size_t i;
    gmsg_memset(&c, 0, sizeof c);
    c.enabled = true;
    c.esc = esc;
    c.style = GMSG_ESCAPE_XOR;
    c.xor_mask = xm;
    reserve(&c, esc);
    for (i = 0; i < n; ++i) {
        if (c.count < GMSG_ESCAPE_MAX_RESERVED) c.raw[c.count++] = reserved[i];
        reserve(&c, reserved[i]);
    }
    return c;
}

static gmsg_escape_config make_index(uint8_t esc, const uint8_t* reserved, size_t n, uint8_t base) {
    gmsg_escape_config c;
    size_t i;
    gmsg_memset(&c, 0, sizeof c);
    c.enabled = true;
    c.esc = esc;
    c.style = GMSG_ESCAPE_TABLE;
    reserve(&c, esc);
    for (i = 0; i < n; ++i) {
        if (c.count < GMSG_ESCAPE_MAX_RESERVED) {
            c.code[c.count] = (uint8_t)(base + c.count);
            c.raw[c.count++] = reserved[i];
        }
        reserve(&c, reserved[i]);
    }
    return c;
}

static gmsg_escape_config make_map(uint8_t esc, const uint8_t* raw, const uint8_t* code, size_t n) {
    gmsg_escape_config c;
    size_t i;
    gmsg_memset(&c, 0, sizeof c);
    c.enabled = true;
    c.esc = esc;
    c.style = GMSG_ESCAPE_TABLE;
    reserve(&c, esc);
    for (i = 0; i < n; ++i) {
        c.raw[c.count] = raw[i];
        c.code[c.count++] = code[i];
        reserve(&c, raw[i]);
    }
    return c;
}

static gmsg_escape_config escape_cfg(int which) {
    static const uint8_t hdlc[] = {0x7E};
    static const uint8_t idx[] = {0x80, 0x81};
    static const uint8_t slip_raw[] = {0xC0, 0xDB};
    static const uint8_t slip_code[] = {0xDC, 0xDD};
    switch (which) {
        case 0: return make_xor(0x7D, 0x20, hdlc, 1);
        case 1: return make_index(0x81, idx, 2, 0);
        default: return make_map(0xDB, slip_raw, slip_code, 2);
    }
}

static gmsg_pattern pat(const uint8_t* b, uint8_t n) {
    gmsg_pattern p;
    gmsg_memset(&p, 0, sizeof p);
    gmsg_memcpy(p.bytes, b, n);
    p.len = n;
    return p;
}

static gmsg_frame_scan scan_cfg(int which, const uint8_t* buf, size_t n) {
    static const uint8_t aa55[] = {0xAA, 0x55};
    static const uint8_t flag[] = {0x7E};
    static const uint8_t dollar[] = {0x24};
    static const uint8_t crlf[] = {0x0D, 0x0A};
    static const uint8_t x80[] = {0x80};
    gmsg_fixed_config f;
    gmsg_length_prefixed_config l;
    gmsg_delimited_config d;
    gmsg_memset(&f, 0, sizeof f);
    gmsg_memset(&l, 0, sizeof l);
    gmsg_memset(&d, 0, sizeof d);
    switch (which) {
        case 0: /* fixed, sync */
            f.sync = pat(aa55, 2);
            f.frame_size = 6;
            return gmsg_scan_fixed(buf, n, &f);
        case 1: /* fixed, no sync */
            f.frame_size = 4;
            return gmsg_scan_fixed(buf, n, &f);
        case 2: /* fixed, escaped, index style */
            f.sync = pat(x80, 1);
            f.frame_size = 5;
            f.escape = escape_cfg(1);
            return gmsg_scan_fixed(buf, n, &f);
        case 3: /* length prefixed, 1-byte length at 2 */
            l.sync = pat(aa55, 2);
            l.length_offset = 2;
            l.length_size = 1;
            l.length_endian = GMSG_ENDIAN_BIG;
            l.add = 5;
            l.min_frame = 5;
            l.max_frame = 24;
            return gmsg_scan_length_prefixed(buf, n, &l);
        case 4: /* length prefixed, 2-byte little-endian length, negative adjust, no sync */
            l.length_size = 2;
            l.length_endian = GMSG_ENDIAN_LITTLE;
            l.add = -1;
            l.min_frame = 3;
            l.max_frame = 20;
            return gmsg_scan_length_prefixed(buf, n, &l);
        case 5: /* length prefixed, escaped (xor), unescaped count */
            l.sync = pat(flag, 1);
            l.length_offset = 1;
            l.length_size = 1;
            l.length_endian = GMSG_ENDIAN_BIG;
            l.add = 4;
            l.min_frame = 4;
            l.max_frame = 32;
            l.escape = escape_cfg(0);
            return gmsg_scan_length_prefixed(buf, n, &l);
        case 6: /* length prefixed, escaped, counts wire bytes */
            l.sync = pat(flag, 1);
            l.length_offset = 1;
            l.length_size = 1;
            l.length_endian = GMSG_ENDIAN_BIG;
            l.min_frame = 4;
            l.max_frame = 32;
            l.escape = escape_cfg(0);
            l.counts_wire = true;
            l.header_len = 2;
            l.trailer_len = 1;
            return gmsg_scan_length_prefixed(buf, n, &l);
        case 7: /* delimited, shared flag, escaped (HDLC) */
            d.start = pat(flag, 1);
            d.end = pat(flag, 1);
            d.escape = escape_cfg(0);
            d.max_frame = 24;
            return gmsg_scan_delimited(buf, n, &d);
        default: /* delimited text, start and end */
            d.start = pat(dollar, 1);
            d.end = pat(crlf, 2);
            d.max_frame = 16;
            return gmsg_scan_delimited(buf, n, &d);
    }
}

void gmpct_scan(int which, const uint8_t* buf, size_t n, size_t out[5]) {
    const gmsg_frame_scan s = scan_cfg(which, buf, n);
    out[0] = (size_t)s.kind;
    out[1] = s.size;
    out[2] = s.consume;
    out[3] = s.consume_on_error;
    out[4] = (size_t)s.error;
}

int gmpct_unescape(int which, uint8_t* buf, size_t n, size_t* out_n) {
    const gmsg_escape_config c = escape_cfg(which);
    return (int)gmsg_unescape_in_place(buf, n, &c, out_n);
}

int gmpct_escape(int which, uint8_t* buf, size_t cap, size_t at, size_t len, size_t* out_len) {
    const gmsg_escape_config c = escape_cfg(which);
    return (int)gmsg_escape_in_place(buf, cap, at, len, &c, out_len);
}

void gmpct_walk(int which, const uint8_t* buf, size_t n, size_t at, size_t count, size_t pick_at, uint8_t* pick,
                size_t pick_n, size_t out[3]) {
    const gmsg_escape_config c = escape_cfg(which);
    const gmsg_escape_walk w = gmsg_walk_escaped(buf, n, at, count, &c, pick_at, pick, pick_n);
    out[0] = (size_t)w.kind;
    out[1] = w.wire;
    out[2] = (size_t)w.error;
}

/* ---- stream ----------------------------------------------------------------------------------------- */

typedef struct test_stream {
    gmsg_stream base; /* first member: the callbacks get the wrapper by a cast */
    size_t* events;
    size_t max_events;
    size_t count;
} test_stream;

static int g_stream_cfg = 0;

static gmsg_frame_scan test_scan(const uint8_t* buf, size_t n) { return scan_cfg(g_stream_cfg, buf, n); }

static void record(test_stream* t, size_t kind, size_t code, size_t size) {
    if (t->count < t->max_events) {
        t->events[3 * t->count] = kind;
        t->events[3 * t->count + 1] = code;
        t->events[3 * t->count + 2] = size;
    }
    ++t->count;
}

static gmsg_error test_process(gmsg_stream* s, uint8_t* frame, size_t n) {
    test_stream* t = (test_stream*)s;
    if (n == 0 || (frame[n - 1] & 1u) != 0) return GMSG_ERR_BAD_CHECKSUM;
    record(t, 0, 0, n);
    return GMSG_OK;
}

static void test_error(gmsg_stream* s, gmsg_error e, const uint8_t* data, size_t n) {
    (void)data;
    record((test_stream*)s, 1, (size_t)e, n);
}

size_t gmpct_stream(int which, size_t capacity, const uint8_t* data, size_t n, size_t chunk, size_t* events,
                    size_t max_events, uint32_t stats[4]) {
    static uint8_t buf[256];
    test_stream t;
    size_t pos = 0;
    g_stream_cfg = which;
    if (capacity > sizeof buf || gmsg_stream_init(&t.base, buf, capacity, 1, test_scan, test_process, test_error) != GMSG_OK)
        return 0;
    t.events = events;
    t.max_events = max_events;
    t.count = 0;
    while (pos < n) {
        const size_t k = n - pos < chunk ? n - pos : chunk;
        (void)gmsg_stream_feed(&t.base, data + pos, k);
        pos += k;
    }
    stats[0] = t.base.stats.frames;
    stats[1] = t.base.stats.frame_errors;
    stats[2] = t.base.stats.bytes_skipped;
    stats[3] = t.base.stats.overflows;
    return t.count;
}
