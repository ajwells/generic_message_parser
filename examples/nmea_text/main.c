/* The C version of main.cpp, written in the pull style: push received bytes, then poll for events (no callbacks).
 * Parses NMEA 0183 sentences (text protocol, XOR checksum, optional fields), prints one, and encodes one. */
#include <stdio.h>
#include <string.h>

#include "nmea.gmp.h"

static const char log_text[] =
    "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"
    "$GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n" /* bad checksum */
    "$GPTXT,01,01,02,ANTSTATUS=OK*3B\r\n"
    "$GPGGA,000000,,,,,0,00,,,,,,,*66\r\n";

static void show_gga(const nav_nmea_gga_view* g) {
    /* Text views parse on access: each accessor returns a gmsg_error and writes the value. */
    double t = 0, lat = 0;
    nav_nmea_fix_quality q = NAV_NMEA_FIX_QUALITY_INVALID;
    uint8_t sats = 0;
    const char* qs;
    (void)nav_nmea_gga_view_utc_time(g, &t);
    (void)nav_nmea_gga_view_quality(g, &q);
    (void)nav_nmea_gga_view_satellites(g, &sats);
    qs = nav_nmea_fix_quality_to_string(q);
    printf("GGA time=%.0f quality=%s sats=%u", t, qs ? qs : "?", (unsigned)sats);
    /* optional field: has_<field> tells whether the token is present */
    if (nav_nmea_gga_view_has_latitude(g) && nav_nmea_gga_view_latitude(g, &lat) == GMSG_OK) printf(" lat=%.3f", lat);
    else printf(" (no fix)");
    printf("\n");
}

int main(void) {
    static uint8_t buffer[NAV_NMEA_NMEA_MAX_FRAME];
    nav_nmea_nmea_stream parser;
    nav_nmea_nmea_event ev;
    const uint8_t* data = (const uint8_t*)log_text;
    const size_t total = sizeof log_text - 1;
    size_t off = 0;
    int printed = 0;

    if (nav_nmea_nmea_stream_init(&parser, buffer, sizeof buffer) != GMSG_OK) return 1;
    while (off < total) {
        /* push what fits (a UART ISR would push a few bytes at a time), then drain the events */
        const size_t want = total - off < 16 ? total - off : 16;
        off += nav_nmea_nmea_stream_push(&parser, data + off, want);
        for (;;) {
            const gmsg_poll r = nav_nmea_nmea_stream_poll_event(&parser, &ev);
            if (r == GMSG_POLL_NEED_MORE) break;
            if (r == GMSG_POLL_ERROR) {
                printf("rejected: %s\n", gmsg_error_str(ev.error));
                continue;
            }
            if (ev.message.kind == NAV_NMEA_NMEA_GGA) {
                show_gga(&ev.message.as.gga);
                if (!printed++) {
                    /* the whole sentence, every field (see gmp/c/print.h) */
                    gmsg_printer p;
                    gmsg_printer_init_file(&p, stdout, NULL);
                    nav_nmea_gga_view_print(&ev.message.as.gga, &p);
                    (void)gmsg_printer_finish(&p, true, NULL);
                }
            } else if (ev.message.kind == NAV_NMEA_NMEA_TXT) {
                const gmsg_str t = nav_nmea_txt_view_text(&ev.message.as.txt);
                printf("TXT \"%.*s\"\n", (int)t.size, t.data);
            }
        }
    }

    {
        nav_nmea_txt txt;
        uint8_t out[NAV_NMEA_NMEA_MAX_FRAME];
        size_t n = 0;
        memset(&txt, 0, sizeof txt);
        txt.total = 1;
        txt.number = 1;
        txt.severity = 2;
        memcpy(txt.text, "HELLO", 5);
        txt.text_len = 5;
        if (nav_nmea_nmea_encode_txt(&txt, out, sizeof out, &n) == GMSG_OK) printf("encoded: %.*s", (int)n, (const char*)out);
    }
    return 0;
}
