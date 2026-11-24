/* The C version of main.cpp: 0x80 is reserved as the sync byte, so every 0x80 / 0x81 inside a frame is sent as
 * 81 00 / 81 01. Encode a few frames, show the escaping, then cut one frame short and let the stream parser
 * resynchronise on the next 0x80. Same schema (motor_link.yaml), generated with `gmpc --lang c`. */
#include <stdio.h>
#include <string.h>

#include "motor_link.gmp.h"

typedef struct frame {
    uint8_t b[MOTOR_MOTOR_LINK_MAX_FRAME];
    size_t n;
} frame;

static void check(gmsg_error e) {
    if (e != GMSG_OK) printf("encode failed: %s\n", gmsg_error_str(e));
}

/* Print a wire frame with escape pairs in brackets. */
static void print_wire(const char* label, const frame* f) {
    size_t i;
    printf("%-10s", label);
    for (i = 0; i < f->n; ++i) {
        if (i > 0 && f->b[i] == 0x81 && i + 1 < f->n) {
            printf(" [%02X %02X]", f->b[i], f->b[i + 1]);
            ++i;
        } else {
            printf(" %02X", f->b[i]);
        }
    }
    printf("   (%lu bytes)\n", (unsigned long)f->n);
}

static void on_telemetry(void* ctx, const motor_motor_link_header_view* h, const motor_telemetry_view* v) {
    (void)ctx;
    (void)h;
    printf("  Telemetry seq=%u rpm=%u current=%.2fA temperature=%.1fC\n", (unsigned)motor_telemetry_view_seq(v),
           (unsigned)motor_telemetry_view_rpm(v), motor_telemetry_view_current_scaled(v),
           motor_telemetry_view_temperature_scaled(v));
}

static void on_log(void* ctx, const motor_motor_link_header_view* h, const motor_log_view* v) {
    const gmsg_str t = motor_log_view_text(v);
    (void)ctx;
    (void)h;
    printf("  Log \"%.*s\"\n", (int)t.size, t.data);
}

static void on_error(void* ctx, gmsg_error e, const uint8_t* data, size_t size) {
    (void)ctx;
    (void)data;
    printf("  dropped %lu byte(s): %s\n", (unsigned long)size, gmsg_error_str(e));
}

int main(void) {
    static uint8_t wire[256];
    static uint8_t buffer[MOTOR_MOTOR_LINK_MAX_FRAME];
    motor_set_speed cmd;
    motor_telemetry t;
    motor_log log;
    frame set_speed, telemetry, log_frame, telemetry2, copy;
    motor_motor_link_any_view v;
    motor_motor_link_stream parser;
    const gmsg_stream_stats* st;
    size_t wire_len = 0, off;
    static const uint8_t noise[] = {0x13, 0x37};

    /* --- encoding: reserved bytes in the payload are escaped ---------------------------------------- */
    memset(&cmd, 0, sizeof cmd);
    cmd.rpm = 1920; /* 0x0780 */
    cmd.direction = MOTOR_DIRECTION_FORWARD;
    cmd.ramp_ms = 385; /* 0x0181 */
    check(motor_motor_link_encode_set_speed(&cmd, set_speed.b, sizeof set_speed.b, &set_speed.n));

    memset(&t, 0, sizeof t);
    t.seq = 128; /* 0x0080 */
    t.rpm = 1920;
    motor_telemetry_set_current_scaled(&t, 3.21);
    motor_telemetry_set_temperature_scaled(&t, 38.5); /* 385 = 0x0181 */
    check(motor_motor_link_encode_telemetry(&t, telemetry.b, sizeof telemetry.b, &telemetry.n));

    memset(&log, 0, sizeof log);
    strcpy(log.text, "rpm limit reached");
    log.len = (uint8_t)strlen(log.text); /* `len` is the length field of `text`: set it to the text size */
    check(motor_motor_link_encode_log(&log, log_frame.b, sizeof log_frame.b, &log_frame.n));

    t.seq = 129;
    check(motor_motor_link_encode_telemetry(&t, telemetry2.b, sizeof telemetry2.b, &telemetry2.n));

    printf("Escaped frames (escape pairs in brackets: [81 00] = 0x80, [81 01] = 0x81):\n");
    print_wire("SetSpeed", &set_speed);
    print_wire("Telemetry", &telemetry);
    print_wire("Log", &log_frame);

    /* --- whole-buffer decoding: the frame is unescaped in place, so parse() takes a mutable buffer ---- */
    copy = set_speed;
    if (motor_motor_link_parse(copy.b, copy.n, &v) == GMSG_OK && v.kind == MOTOR_MOTOR_LINK_SET_SPEED) {
        const char* dir = motor_direction_to_string(motor_set_speed_view_direction(&v.as.set_speed));
        printf("\nparse(): SetSpeed rpm=%u direction=%s ramp=%ums\n", (unsigned)motor_set_speed_view_rpm(&v.as.set_speed),
               dir ? dir : "?", (unsigned)motor_set_speed_view_ramp_ms(&v.as.set_speed));
    }

    /* --- streaming: a frame cut short, then line noise ----------------------------------------------- */
    memcpy(wire + wire_len, telemetry.b, telemetry.n);
    wire_len += telemetry.n;
    memcpy(wire + wire_len, log_frame.b, 9); /* cut short: the rest never arrives */
    wire_len += 9;
    memcpy(wire + wire_len, telemetry2.b, telemetry2.n);
    wire_len += telemetry2.n;
    memcpy(wire + wire_len, noise, sizeof noise);
    wire_len += sizeof noise;
    memcpy(wire + wire_len, log_frame.b, log_frame.n);
    wire_len += log_frame.n;

    printf("\nStream of %lu bytes, fed in 3-byte chunks:\n", (unsigned long)wire_len);
    if (motor_motor_link_stream_init(&parser, buffer, sizeof buffer) != GMSG_OK) return 1;
    parser.cb.on_telemetry = on_telemetry;
    parser.cb.on_log = on_log;
    parser.cb.on_error = on_error;
    for (off = 0; off < wire_len; off += 3)
        (void)motor_motor_link_stream_feed(&parser, wire + off, wire_len - off < 3 ? wire_len - off : 3);

    st = motor_motor_link_stream_stats(&parser);
    printf("frames=%u errors=%u skipped=%u\n", (unsigned)st->frames, (unsigned)st->frame_errors,
           (unsigned)st->bytes_skipped);
    return 0;
}
