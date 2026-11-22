/* gmp/c/print.h - human readable printing of generated messages for logs and troubleshooting.
 *
 * The C version of gmp/print.hpp; the text is identical. Generated structs, views and bits groups get
 * `void <type>_print(const <type>* m, gmsg_printer* p)`, and protocols `<protocol>_any_view_print` /
 * `<protocol>_any_message_print`. Set up a printer for a sink, print, then finish:
 *
 *   char line[256];
 *   gmsg_printer p;
 *   gmsg_printer_init_buffer(&p, line, sizeof line, NULL);   // always NUL terminated
 *   acme_status_print(&status, &p);
 *   gmsg_error e = gmsg_printer_finish(&p, false, &n);       // GMSG_ERR_BUFFER_FULL when truncated
 *
 *   gmsg_printer_init_file(&p, stdout, NULL);                // FILE* (needs GMSG_USE_LIBC)
 *   gmsg_printer_init(&p, my_write, my_ctx, NULL);           // bool my_write(void* ctx, const char* s, size_t n)
 *
 * Output (multi-line by default; options.style = GMSG_PRINT_SINGLE_LINE for one line):
 *
 *   Status {
 *     voltage: 12600 (12.6 V)
 *     state: {
 *       armed: true
 *       mode: run (0x01)
 *     }
 *     samples: [1, 2, 3]
 *     fault_code: <absent>
 *   }
 *
 * Floats use gmp/c/float_text.h (GMSG_FLOAT_TEXT); with GMSG_FLOAT_TEXT_NONE they print as <float>.
 * Custom types print through the `print` function of their `c:` block, otherwise as <?>.
 */
#ifndef GMSG_C_PRINT_H
#define GMSG_C_PRINT_H

#include "gmp/c/config.h"
#include "gmp/c/error.h"
#include "gmp/c/float_text.h"

#if GMSG_USE_LIBC
#include <stdio.h>
#endif

GMSG_C_BEGIN

typedef enum gmsg_print_style {
    GMSG_PRINT_MULTI_LINE = 0, /* one field per line, nested values indented */
    GMSG_PRINT_SINGLE_LINE     /* everything on one line: `Status { voltage: 12600 (12.6 V), ... }` */
} gmsg_print_style;

typedef struct gmsg_print_options {
    gmsg_print_style style;
    uint8_t indent;   /* spaces per nesting level (multi-line); default 2 */
    size_t max_items; /* array elements shown before "... (+N more)"; 0 = all; default 64 */
    size_t max_bytes; /* bytes shown for bytes fields before "... (+N more)"; 0 = all; default 64 */
    int precision;    /* significant digits of scaled / converted values; <= 0 = shortest; default 9 */
} gmsg_print_options;

/* The defaults (same as gmp::PrintOptions{}). */
GMSG_API gmsg_print_options gmsg_print_defaults(void);

/* Writes s[0..n); returns false when the sink is full (the printer then stops writing). */
typedef bool (*gmsg_print_write_fn)(void* ctx, const char* s, size_t n);

typedef struct gmsg_printer {
    void* ctx;
    gmsg_print_write_fn fn;
    gmsg_print_options opts;
    size_t written;   /* characters handed to the sink */
    unsigned depth;
    uint64_t first;   /* bit d-1: no entry yet in the block at depth d */
    bool note_open;
    bool full;        /* a bounded sink ran out of space */
    /* char buffer sink (gmsg_printer_init_buffer) */
    char* buf;
    size_t cap;
    size_t len;
} gmsg_printer;

GMSG_API void gmsg_printer_init(gmsg_printer* p, gmsg_print_write_fn fn, void* ctx, const gmsg_print_options* opts);

/* Print into buf[0..cap): always NUL terminated; text that does not fit is cut (finish reports BUFFER_FULL). */
GMSG_API void gmsg_printer_init_buffer(gmsg_printer* p, char* buf, size_t cap, const gmsg_print_options* opts);

#if GMSG_USE_LIBC

/* Print to a FILE* (console, or a log opened with fopen). */
GMSG_API void gmsg_printer_init_file(gmsg_printer* p, FILE* f, const gmsg_print_options* opts);
#endif

/* ---- low level output ----------------------------------------------------------------------------------- */

GMSG_API void gmsg_print_raw(gmsg_printer* p, const char* s, size_t n);

/* Closes a pending note; gmsg_printer_finish does this. */
GMSG_API void gmsg_print_flush(gmsg_printer* p);

/* ---- values --------------------------------------------------------------------------------------------- */

GMSG_API void gmsg_print_u64(gmsg_printer* p, uint64_t v);
GMSG_API void gmsg_print_i64(gmsg_printer* p, int64_t v);
GMSG_API void gmsg_print_bool(gmsg_printer* p, bool v);

/* Shortest round-trip text of a double / float (as std::to_chars(v)). */
GMSG_API void gmsg_print_f64(gmsg_printer* p, double v);
GMSG_API void gmsg_print_f32(gmsg_printer* p, float v);

/* 'c' */
GMSG_API void gmsg_print_char(gmsg_printer* p, char c);

/* "text" with escapes */
GMSG_API void gmsg_print_str(gmsg_printer* p, const char* s, size_t n);

/* [0A 1B 2C] */
GMSG_API void gmsg_print_hex(gmsg_printer* p, const uint8_t* b, size_t n);

/* Enum value: `name (0x01)`, or `<unknown> (0x07)` when name is NULL; raw is printed with `bytes` bytes. */
GMSG_API void gmsg_print_enum(gmsg_printer* p, const char* name, uint64_t raw, size_t bytes);

/* <error: text> (a view field that could not be read) */
GMSG_API void gmsg_print_error(gmsg_printer* p, gmsg_error e);

/* ---- structure (used by generated code) ----------------------------------------------------------------- */

/* Opens a block: `Type {` (or `{` when type is NULL). */
GMSG_API void gmsg_print_begin(gmsg_printer* p, const char* type);

/* Closes the block opened by gmsg_print_begin(). */
GMSG_API void gmsg_print_end(gmsg_printer* p);

/* Starts a field: `name: ` (then print the value). */
GMSG_API void gmsg_print_key(gmsg_printer* p, const char* name);

/* `<absent>` (a conditional / optional value that is not present) */
GMSG_API void gmsg_print_absent_value(gmsg_printer* p);

/* A conditional field that is not present: `name: <absent>`. */
GMSG_API void gmsg_print_absent(gmsg_printer* p, const char* name);

/* Unit of the value just printed: `12 V`. */
GMSG_API void gmsg_print_unit(gmsg_printer* p, const char* unit);

/* Free text added to the parenthesised note after a value. */
GMSG_API void gmsg_print_note(gmsg_printer* p, const char* s);

/* Engineering conversion of the value just printed, collected in parentheses: `12600 (12.6 V, 54.7 degF)`. */
GMSG_API void gmsg_print_conversion(gmsg_printer* p, double v, const char* unit);

/* Raw text (for custom print functions). */
GMSG_API void gmsg_print_text(gmsg_printer* p, const char* s, size_t n);

/* Lists: `[a, b, c]`; `blocks` (elements are messages) puts one element per line in multi-line mode.
 *
 *   gmsg_print_list l = gmsg_print_list_begin(p, n, false);
 *   for (i = 0; i < l.shown; ++i) { gmsg_print_list_item(p, &l, i); gmsg_print_u64(p, a[i]); }
 *   gmsg_print_list_end(p, &l);
 */
typedef struct gmsg_print_list {
    size_t n, shown;
    bool lines;
} gmsg_print_list;

GMSG_API gmsg_print_list gmsg_print_list_begin(gmsg_printer* p, size_t n, bool blocks);
GMSG_API void gmsg_print_list_item(gmsg_printer* p, const gmsg_print_list* l, size_t i);
GMSG_API void gmsg_print_list_end(gmsg_printer* p, const gmsg_print_list* l);

/* Ends printing: closes a pending note, adds a newline when asked, and reports the result.
 * *written (may be NULL) receives the characters written; GMSG_ERR_BUFFER_FULL when the sink was full. */
GMSG_API gmsg_error gmsg_printer_finish(gmsg_printer* p, bool newline, size_t* written);

/* ---- implementation (header-only build, or runtime-c/src/print.c of the library build) --------------------------------- */

#if GMSG_DEFINE_FUNCTIONS_ || defined(GMSG_IMPL_PRINT_)

GMSG_API gmsg_print_options gmsg_print_defaults(void) {
    gmsg_print_options o;
    o.style = GMSG_PRINT_MULTI_LINE;
    o.indent = 2;
    o.max_items = 64;
    o.max_bytes = 64;
    o.precision = 9;
    return o;
}

#define GMSG_PRINT_MAX_DEPTH_ 63u

GMSG_API void gmsg_printer_init(gmsg_printer* p, gmsg_print_write_fn fn, void* ctx, const gmsg_print_options* opts) {
    p->ctx = ctx;
    p->fn = fn;
    p->opts = opts ? *opts : gmsg_print_defaults();
    p->written = 0;
    p->depth = 0;
    p->first = 0;
    p->note_open = false;
    p->full = false;
    p->buf = NULL;
    p->cap = 0;
    p->len = 0;
}

GMSG_PRIV_ bool gmsg_print_buffer_write_(void* ctx, const char* s, size_t n) {
    gmsg_printer* p = (gmsg_printer*)ctx;
    size_t room, k, i;
    if (p->cap == 0) return false;
    room = p->cap - 1 - p->len;
    k = n < room ? n : room;
    for (i = 0; i < k; ++i) p->buf[p->len + i] = s[i];
    p->len += k;
    p->buf[p->len] = '\0';
    return k == n;
}

GMSG_API void gmsg_printer_init_buffer(gmsg_printer* p, char* buf, size_t cap, const gmsg_print_options* opts) {
    gmsg_printer_init(p, gmsg_print_buffer_write_, NULL, opts);
    p->ctx = p;
    p->buf = buf;
    p->cap = cap;
    if (cap) buf[0] = '\0';
}
#if GMSG_USE_LIBC
GMSG_PRIV_ bool gmsg_print_file_write_(void* ctx, const char* s, size_t n) { return fwrite(s, 1, n, (FILE*)ctx) == n; }

GMSG_API void gmsg_printer_init_file(gmsg_printer* p, FILE* f, const gmsg_print_options* opts) {
    gmsg_printer_init(p, gmsg_print_file_write_, f, opts);
}
#endif

GMSG_API void gmsg_print_raw(gmsg_printer* p, const char* s, size_t n) {
    if (p->full || n == 0) return;
    if (!p->fn(p->ctx, s, n)) {
        p->full = true;
        return;
    }
    p->written += n;
}

GMSG_PRIV_ void gmsg_print_cstr_(gmsg_printer* p, const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    gmsg_print_raw(p, s, n);
}

GMSG_PRIV_ bool gmsg_print_multi_line_(const gmsg_printer* p) { return p->opts.style == GMSG_PRINT_MULTI_LINE; }

GMSG_PRIV_ void gmsg_print_push_(gmsg_printer* p) {
    if (p->depth < GMSG_PRINT_MAX_DEPTH_) p->first |= (uint64_t)1 << p->depth;
    ++p->depth;
}

GMSG_PRIV_ void gmsg_print_pop_(gmsg_printer* p) {
    if (p->depth) --p->depth;
}

GMSG_PRIV_ bool gmsg_print_first_(const gmsg_printer* p) {
    return p->depth && p->depth - 1 < GMSG_PRINT_MAX_DEPTH_ && ((p->first >> (p->depth - 1)) & 1u);
}

GMSG_PRIV_ void gmsg_print_clear_first_(gmsg_printer* p) {
    if (p->depth && p->depth - 1 < GMSG_PRINT_MAX_DEPTH_) p->first &= ~((uint64_t)1 << (p->depth - 1));
}

GMSG_PRIV_ void gmsg_print_newline_(gmsg_printer* p) {
    unsigned i;
    gmsg_print_raw(p, "\n", 1);
    for (i = 0; i < p->depth * p->opts.indent; ++i) gmsg_print_raw(p, " ", 1);
}

GMSG_PRIV_ void gmsg_print_separate_(gmsg_printer* p) {
    if (gmsg_print_multi_line_(p)) gmsg_print_newline_(p);
    else gmsg_print_cstr_(p, gmsg_print_first_(p) ? " " : ", ");
    gmsg_print_clear_first_(p);
}

GMSG_PRIV_ void gmsg_print_open_note_(gmsg_printer* p) {
    gmsg_print_cstr_(p, p->note_open ? ", " : " (");
    p->note_open = true;
}

GMSG_API void gmsg_print_flush(gmsg_printer* p) {
    if (!p->note_open) return;
    p->note_open = false;
    gmsg_print_raw(p, ")", 1);
}

GMSG_API void gmsg_print_u64(gmsg_printer* p, uint64_t v) {
    char tmp[24];
    int n = 0, i;
    char out[24];
    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    for (i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    gmsg_print_raw(p, out, (size_t)n);
}

GMSG_API void gmsg_print_i64(gmsg_printer* p, int64_t v) {
    if (v < 0) {
        gmsg_print_raw(p, "-", 1);
        gmsg_print_u64(p, (uint64_t)0 - (uint64_t)v);
    } else {
        gmsg_print_u64(p, (uint64_t)v);
    }
}

GMSG_API void gmsg_print_bool(gmsg_printer* p, bool v) { gmsg_print_cstr_(p, v ? "true" : "false"); }

GMSG_PRIV_ void gmsg_print_hex_digits_(gmsg_printer* p, uint64_t v, size_t bytes) {
    static const char digits[] = "0123456789ABCDEF";
    char tmp[16];
    const size_t n = bytes * 2;
    size_t i;
    for (i = 0; i < n; ++i) tmp[n - 1 - i] = digits[(v >> (4 * i)) & 0xFu];
    gmsg_print_raw(p, tmp, n);
}

GMSG_API void gmsg_print_f64(gmsg_printer* p, double v) {
#if GMSG_FLOAT_TEXT != GMSG_FLOAT_TEXT_NONE
    char tmp[40];
    size_t n = 0;
    if (gmsg_format_double(v, -1, tmp, sizeof tmp, &n) == GMSG_OK) gmsg_print_raw(p, tmp, n);
    else gmsg_print_cstr_(p, "<float>");
#else
    (void)v;
    gmsg_print_cstr_(p, "<float>");
#endif
}

GMSG_API void gmsg_print_f32(gmsg_printer* p, float v) {
#if GMSG_FLOAT_TEXT != GMSG_FLOAT_TEXT_NONE
    char tmp[40];
    size_t n = 0;
    if (gmsg_format_float(v, -1, tmp, sizeof tmp, &n) == GMSG_OK) gmsg_print_raw(p, tmp, n);
    else gmsg_print_cstr_(p, "<float>");
#else
    (void)v;
    gmsg_print_cstr_(p, "<float>");
#endif
}

/* `digits` significant digits (as std::to_chars(v, general, digits)); digits <= 0: shortest. */
GMSG_PRIV_ void gmsg_print_f64_digits_(gmsg_printer* p, double v, int digits) {
#if GMSG_FLOAT_TEXT != GMSG_FLOAT_TEXT_NONE
    char tmp[40];
    size_t n = 0;
    if (digits <= 0) {
        gmsg_print_f64(p, v);
        return;
    }
    if (gmsg_format_general(v, digits, tmp, sizeof tmp, &n) == GMSG_OK) gmsg_print_raw(p, tmp, n);
    else gmsg_print_cstr_(p, "<float>");
#else
    (void)v;
    (void)digits;
    gmsg_print_cstr_(p, "<float>");
#endif
}

GMSG_PRIV_ void gmsg_print_escaped_char_(gmsg_printer* p, char c, char quote) {
    const unsigned char u = (unsigned char)c;
    switch (c) {
        case '\n': gmsg_print_raw(p, "\\n", 2); return;
        case '\r': gmsg_print_raw(p, "\\r", 2); return;
        case '\t': gmsg_print_raw(p, "\\t", 2); return;
        case '\\': gmsg_print_raw(p, "\\\\", 2); return;
        default: break;
    }
    if (c == quote) {
        const char e[2] = {'\\', c};
        gmsg_print_raw(p, e, 2);
        return;
    }
    if (u < 0x20 || u >= 0x7F) {
        gmsg_print_raw(p, "\\x", 2);
        gmsg_print_hex_digits_(p, u, 1);
        return;
    }
    gmsg_print_raw(p, &c, 1);
}

GMSG_API void gmsg_print_char(gmsg_printer* p, char c) {
    gmsg_print_raw(p, "'", 1);
    gmsg_print_escaped_char_(p, c, '\'');
    gmsg_print_raw(p, "'", 1);
}

GMSG_API void gmsg_print_str(gmsg_printer* p, const char* s, size_t n) {
    size_t i;
    gmsg_print_raw(p, "\"", 1);
    for (i = 0; i < n; ++i) gmsg_print_escaped_char_(p, s[i], '"');
    gmsg_print_raw(p, "\"", 1);
}

GMSG_PRIV_ void gmsg_print_more_(gmsg_printer* p, size_t k) {
    gmsg_print_cstr_(p, "... (+");
    gmsg_print_u64(p, (uint64_t)k);
    gmsg_print_cstr_(p, " more)");
}

GMSG_API void gmsg_print_hex(gmsg_printer* p, const uint8_t* b, size_t n) {
    const size_t shown = p->opts.max_bytes && n > p->opts.max_bytes ? p->opts.max_bytes : n;
    size_t i;
    gmsg_print_raw(p, "[", 1);
    for (i = 0; i < shown; ++i) {
        if (i) gmsg_print_raw(p, " ", 1);
        gmsg_print_hex_digits_(p, b[i], 1);
    }
    if (shown < n) {
        gmsg_print_raw(p, " ", 1);
        gmsg_print_more_(p, n - shown);
    }
    gmsg_print_raw(p, "]", 1);
}

GMSG_API void gmsg_print_enum(gmsg_printer* p, const char* name, uint64_t raw, size_t bytes) {
    gmsg_print_cstr_(p, name ? name : "<unknown>");
    gmsg_print_cstr_(p, " (0x");
    gmsg_print_hex_digits_(p, bytes >= 8 ? raw : raw & (((uint64_t)1 << (8 * bytes)) - 1u), bytes);
    gmsg_print_raw(p, ")", 1);
}

GMSG_API void gmsg_print_error(gmsg_printer* p, gmsg_error e) {
    gmsg_print_cstr_(p, "<error: ");
    gmsg_print_cstr_(p, gmsg_error_str(e));
    gmsg_print_raw(p, ">", 1);
}

GMSG_API void gmsg_print_begin(gmsg_printer* p, const char* type) {
    gmsg_print_flush(p);
    if (type && type[0]) {
        gmsg_print_cstr_(p, type);
        gmsg_print_raw(p, " ", 1);
    }
    gmsg_print_raw(p, "{", 1);
    gmsg_print_push_(p);
}

GMSG_API void gmsg_print_end(gmsg_printer* p) {
    bool empty;
    gmsg_print_flush(p);
    empty = gmsg_print_first_(p);
    gmsg_print_pop_(p);
    if (empty) {
        gmsg_print_raw(p, "}", 1);
    } else if (gmsg_print_multi_line_(p)) {
        gmsg_print_newline_(p);
        gmsg_print_raw(p, "}", 1);
    } else {
        gmsg_print_raw(p, " }", 2);
    }
}

GMSG_API void gmsg_print_key(gmsg_printer* p, const char* name) {
    gmsg_print_flush(p);
    gmsg_print_separate_(p);
    gmsg_print_cstr_(p, name);
    gmsg_print_raw(p, ": ", 2);
}

GMSG_API void gmsg_print_absent_value(gmsg_printer* p) { gmsg_print_cstr_(p, "<absent>"); }

GMSG_API void gmsg_print_absent(gmsg_printer* p, const char* name) {
    gmsg_print_key(p, name);
    gmsg_print_absent_value(p);
}

GMSG_API void gmsg_print_unit(gmsg_printer* p, const char* unit) {
    if (!unit || !unit[0]) return;
    gmsg_print_raw(p, " ", 1);
    gmsg_print_cstr_(p, unit);
}

GMSG_API void gmsg_print_note(gmsg_printer* p, const char* s) {
    gmsg_print_open_note_(p);
    gmsg_print_cstr_(p, s);
}

GMSG_API void gmsg_print_conversion(gmsg_printer* p, double v, const char* unit) {
    gmsg_print_open_note_(p);
    gmsg_print_f64_digits_(p, v, p->opts.precision);
    gmsg_print_unit(p, unit);
}

GMSG_API void gmsg_print_text(gmsg_printer* p, const char* s, size_t n) { gmsg_print_raw(p, s, n); }

GMSG_API gmsg_print_list gmsg_print_list_begin(gmsg_printer* p, size_t n, bool blocks) {
    gmsg_print_list l;
    l.n = n;
    l.shown = p->opts.max_items && n > p->opts.max_items ? p->opts.max_items : n;
    l.lines = blocks && gmsg_print_multi_line_(p);
    gmsg_print_raw(p, "[", 1);
    if (n == 0) {
        gmsg_print_raw(p, "]", 1);
        return l;
    }
    if (l.lines) gmsg_print_push_(p);
    return l;
}

GMSG_API void gmsg_print_list_item(gmsg_printer* p, const gmsg_print_list* l, size_t i) {
    if (l->lines) gmsg_print_newline_(p);
    else if (i) gmsg_print_raw(p, ", ", 2);
}

GMSG_API void gmsg_print_list_end(gmsg_printer* p, const gmsg_print_list* l) {
    if (l->n == 0) return;
    if (l->shown < l->n) {
        if (l->lines) gmsg_print_newline_(p);
        else gmsg_print_raw(p, ", ", 2);
        gmsg_print_more_(p, l->n - l->shown);
    }
    if (l->lines) {
        gmsg_print_pop_(p);
        gmsg_print_newline_(p);
    }
    gmsg_print_raw(p, "]", 1);
}

GMSG_API gmsg_error gmsg_printer_finish(gmsg_printer* p, bool newline, size_t* written) {
    gmsg_print_flush(p);
    if (newline) gmsg_print_raw(p, "\n", 1);
    if (written) *written = p->buf ? p->len : p->written;
    return p->full ? GMSG_ERR_BUFFER_FULL : GMSG_OK;
}

#endif /* implementation */

GMSG_C_END

#endif /* GMSG_C_PRINT_H */
