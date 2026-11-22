/* C side of the text / float-text cross-check (compiled as C11); see test_text_c_vs_cpp.cpp. */
#include "crosscheck_c.h"

#include "gmp/c/runtime.h"

int gmpct_format_double(double v, int precision, char* out, size_t cap, size_t* n) {
    return (int)gmsg_format_double(v, precision, out, cap, n);
}

int gmpct_format_float(float v, int precision, char* out, size_t cap, size_t* n) {
    return (int)gmsg_format_float(v, precision, out, cap, n);
}

int gmpct_format_general(double v, int digits, char* out, size_t cap, size_t* n) {
    return (int)gmsg_format_general(v, digits, out, cap, n);
}

static void print_block(gmsg_printer* p, int64_t v) {
    gmsg_print_begin(p, "Blk");
    gmsg_print_key(p, "v");
    gmsg_print_i64(p, v);
    gmsg_print_end(p);
}

int gmpct_print_scenario(int style, unsigned indent, size_t max_items, size_t max_bytes, int precision, char* out,
                         size_t cap, size_t* n) {
    static const uint8_t bytes[6] = {0x00, 0x0A, 0x7F, 0x80, 0xFE, 0xFF};
    static const char text[] = "a\"b'c\\\n\r\t\x01\x7F\x80 z";
    gmsg_print_options o = gmsg_print_defaults();
    gmsg_printer p;
    gmsg_print_list l;
    size_t i;
    o.style = style ? GMSG_PRINT_SINGLE_LINE : GMSG_PRINT_MULTI_LINE;
    o.indent = (uint8_t)indent;
    o.max_items = max_items;
    o.max_bytes = max_bytes;
    o.precision = precision;
    gmsg_printer_init_buffer(&p, out, cap, &o);

    gmsg_print_begin(&p, "Scenario");
    gmsg_print_key(&p, "empty");
    gmsg_print_begin(&p, "Empty");
    gmsg_print_end(&p);
    gmsg_print_key(&p, "group");
    gmsg_print_begin(&p, NULL);
    gmsg_print_end(&p);
    gmsg_print_key(&p, "flag");
    gmsg_print_bool(&p, true);
    gmsg_print_key(&p, "off");
    gmsg_print_bool(&p, false);
    gmsg_print_key(&p, "chars");
    gmsg_print_char(&p, 'x');
    gmsg_print_char(&p, '\'');
    gmsg_print_char(&p, '"');
    gmsg_print_char(&p, '\n');
    gmsg_print_char(&p, (char)0x01);
    gmsg_print_char(&p, (char)0x7F);
    gmsg_print_char(&p, (char)0xC3);
    gmsg_print_key(&p, "text");
    gmsg_print_str(&p, text, sizeof text - 1);
    gmsg_print_key(&p, "min");
    gmsg_print_i64(&p, INT64_MIN);
    gmsg_print_key(&p, "max");
    gmsg_print_u64(&p, UINT64_MAX);
    gmsg_print_key(&p, "zero");
    gmsg_print_u64(&p, 0);
    gmsg_print_key(&p, "e8");
    gmsg_print_enum(&p, "run", 1, 1);
    gmsg_print_key(&p, "e16");
    gmsg_print_enum(&p, NULL, (uint64_t)(int64_t)-2, 2);
    gmsg_print_key(&p, "e32");
    gmsg_print_enum(&p, "big", 0xDEADBEEFu, 4);
    gmsg_print_key(&p, "e64");
    gmsg_print_enum(&p, NULL, (uint64_t)(int64_t)-2, 8);
    gmsg_print_key(&p, "raw");
    gmsg_print_hex(&p, bytes, sizeof bytes);
    gmsg_print_key(&p, "none");
    gmsg_print_hex(&p, bytes, 0);
    gmsg_print_key(&p, "list");
    l = gmsg_print_list_begin(&p, 5, false);
    for (i = 0; i < l.shown; ++i) {
        gmsg_print_list_item(&p, &l, i);
        gmsg_print_i64(&p, (int64_t)i - 2);
    }
    gmsg_print_list_end(&p, &l);
    gmsg_print_key(&p, "nothing");
    l = gmsg_print_list_begin(&p, 0, false);
    gmsg_print_list_end(&p, &l);
    gmsg_print_key(&p, "blocks");
    l = gmsg_print_list_begin(&p, 3, true);
    for (i = 0; i < l.shown; ++i) {
        gmsg_print_list_item(&p, &l, i);
        print_block(&p, (int64_t)i * 10);
    }
    gmsg_print_list_end(&p, &l);
    gmsg_print_key(&p, "no_blocks");
    l = gmsg_print_list_begin(&p, 0, true);
    gmsg_print_list_end(&p, &l);
    gmsg_print_key(&p, "volts");
    gmsg_print_u64(&p, 12600);
    gmsg_print_conversion(&p, 12.6, "V");
    gmsg_print_conversion(&p, 1.0 / 3.0, NULL);
    gmsg_print_conversion(&p, 123456789.0, "mV");
    gmsg_print_key(&p, "temp");
    gmsg_print_f32(&p, 0.9f);
    gmsg_print_unit(&p, "degC");
    gmsg_print_key(&p, "sum");
    gmsg_print_f64(&p, 0.1 + 0.2);
    gmsg_print_unit(&p, "m");
    gmsg_print_note(&p, "computed");
    gmsg_print_absent(&p, "gone");
    gmsg_print_key(&p, "bad");
    gmsg_print_error(&p, GMSG_ERR_PARSE);
    gmsg_print_key(&p, "custom");
    gmsg_print_text(&p, "12:34:56", 8);
    gmsg_print_key(&p, "nested");
    gmsg_print_begin(&p, "Outer");
    gmsg_print_key(&p, "inner");
    print_block(&p, -7);
    gmsg_print_key(&p, "last");
    gmsg_print_u64(&p, 1);
    gmsg_print_conversion(&p, 1e-7, "s");
    gmsg_print_end(&p);
    gmsg_print_end(&p);
    gmsg_print_u64(&p, 5); /* a top-level value with a note, closed by finish */
    gmsg_print_note(&p, "tail");
    return (int)gmsg_printer_finish(&p, true, n);
}

int gmpct_parse_double(const char* s, size_t n, double* out) { return (int)gmsg_parse_double(s, n, out); }

int gmpct_parse_float(const char* s, size_t n, float* out) { return (int)gmsg_parse_float(s, n, out); }

int gmpct_parse_i64(const char* s, size_t n, int base, int64_t min, int64_t max, int64_t* out) {
    return (int)gmsg_parse_i64(s, n, base, min, max, out);
}

int gmpct_parse_u64(const char* s, size_t n, int base, uint64_t max, uint64_t* out) {
    return (int)gmsg_parse_u64(s, n, base, max, out);
}

int gmpct_text_tokens(int which, int64_t iv, uint64_t uv, double dv, float fv, int base, int precision, int width,
                      char* out, size_t cap, size_t* n) {
    gmsg_text_writer w = gmsg_text_writer_init((uint8_t*)out, cap, ',');
    gmsg_error e = GMSG_OK;
    switch (which) {
        case 0: e = gmsg_text_i64_token(&w, iv, base, width); break;
        case 1: e = gmsg_text_u64_token(&w, uv, base, width); break;
        case 2: e = gmsg_text_f64_token(&w, dv, precision, width); break;
        case 3: e = gmsg_text_f32_token(&w, fv, precision, width); break;
        default: break;
    }
    if (e == GMSG_OK) e = gmsg_text_char_token(&w, 'x');
    *n = w.pos;
    return (int)e;
}
