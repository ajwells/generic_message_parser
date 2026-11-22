/* The C version of main.cpp: encode a few frames, add line noise, and recover the messages with the stream parser
 * and callbacks. Same schema (telemetry.yaml), generated with `gmpc --lang c` (C11, no heap). */
#include <stdio.h>
#include <string.h>

#include "telemetry.gmp.h"

static uint8_t wire[512];
static size_t wire_len = 0;

static void append(const uint8_t* data, size_t n) {
    if (wire_len + n > sizeof wire) return;
    memcpy(wire + wire_len, data, n);
    wire_len += n;
}

static void append_status(const demo_status* m) {
    uint8_t buf[DEMO_LINK_MAX_FRAME];
    size_t n = 0;
    if (demo_link_encode_status(m, buf, sizeof buf, &n) == GMSG_OK) append(buf, n);
}

static void append_samples(const demo_samples* m) {
    uint8_t buf[DEMO_LINK_MAX_FRAME];
    size_t n = 0;
    if (demo_link_encode_samples(m, buf, sizeof buf, &n) == GMSG_OK) append(buf, n);
}

/* ---- callbacks: one per message, plus errors; ctx is passed to each ---- */

static void on_status(void* ctx, const demo_link_header_view* h, const demo_status_view* v) {
    const demo_status_state st = demo_status_view_state(v);
    const char* mode = demo_mode_to_string(st.mode);
    (void)ctx;
    (void)h;
    printf("Status  uptime=%lus voltage=%.3fV mode=%s", (unsigned long)demo_status_view_uptime_s(v),
           demo_status_view_voltage_scaled(v), mode ? mode : "?");
    if (demo_status_view_has_fault(v)) {
        const demo_fault_view f = demo_status_view_fault(v);
        printf(" fault=0x%04X temp=%.1fC", (unsigned)demo_fault_view_code(&f), demo_fault_view_temperature_scaled(&f));
    }
    printf("\n");
}

static void on_samples(void* ctx, const demo_link_header_view* h, const demo_samples_view* v) {
    /* print dumps every field (enum names, units, nested messages); see gmp/c/print.h */
    gmsg_print_options o = gmsg_print_defaults();
    gmsg_printer p;
    (void)ctx;
    (void)h;
    o.style = GMSG_PRINT_SINGLE_LINE;
    gmsg_printer_init_file(&p, stdout, &o);
    demo_samples_view_print(v, &p);
    (void)gmsg_printer_finish(&p, true, NULL);
}

static void on_error(void* ctx, gmsg_error e, const uint8_t* data, size_t size) {
    (void)ctx;
    (void)data;
    printf("dropped %lu byte(s): %s\n", (unsigned long)size, gmsg_error_str(e));
}

int main(void) {
    static uint8_t buffer[DEMO_LINK_MAX_FRAME]; /* the stream's frame buffer (caller owned, no heap) */
    demo_link_stream parser;
    demo_status ok, bad;
    demo_samples s;
    const gmsg_stream_stats* st;
    size_t off;

    memset(&ok, 0, sizeof ok);
    ok.uptime_s = 3600;
    demo_status_set_voltage_scaled(&ok, 12.6);
    ok.state.armed = true;
    ok.state.mode = DEMO_MODE_RUN;
    append_status(&ok);

    {
        static const uint8_t noise[] = {0x13, 0x37, 0xAA}; /* line noise */
        append(noise, sizeof noise);
    }

    memset(&s, 0, sizeof s);
    s.channel = 3;
    s.values[0] = 100;
    s.values[1] = -100;
    s.values[2] = 250;
    s.count = 3; /* `count` is the length field of `values`: it is the array's size */
    append_samples(&s);

    bad = ok;
    bad.state.mode = DEMO_MODE_FAULT;
    bad.has_fault = true; /* required now that mode == fault (`when`); encode fails with MISSING_FIELD without it */
    bad.fault.code = 0x0042;
    bad.fault.temperature = 875;
    append_status(&bad);

    if (demo_link_stream_init(&parser, buffer, sizeof buffer) != GMSG_OK) return 1;
    parser.cb.on_status = on_status;
    parser.cb.on_samples = on_samples;
    parser.cb.on_error = on_error;

    /* Deliver the stream in awkward 5-byte chunks, as a UART driver might. */
    for (off = 0; off < wire_len; off += 5)
        (void)demo_link_stream_feed(&parser, wire + off, wire_len - off < 5 ? wire_len - off : 5);

    st = demo_link_stream_stats(&parser);
    printf("frames=%u errors=%u skipped=%u\n", (unsigned)st->frames, (unsigned)st->frame_errors,
           (unsigned)st->bytes_skipped);
    return 0;
}
