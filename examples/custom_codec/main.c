/* The C version of main.cpp: a custom field codec and a custom checksum, wired in from the schema's `c:` blocks
 * (meter.yaml -> meter_codecs.h). */
#include <stdio.h>
#include <string.h>

#include "meter.gmp.h"

int main(void) {
    meter_reading r;
    uint8_t frame[METER_METER_BUS_MAX_FRAME];
    size_t n = 0, i;
    meter_meter_bus_any_view any;
    meter_datetime ts;

    memset(&r, 0, sizeof r);
    r.timestamp.year = 2026;
    r.timestamp.month = 11;
    r.timestamp.day = 20;
    r.timestamp.hour = 14;
    r.timestamp.minute = 30;
    r.timestamp.second = 5;
    meter_reading_set_kwh_scaled(&r, 12345.67);

    if (meter_meter_bus_encode_reading(&r, frame, sizeof frame, &n) != GMSG_OK) return 1;
    printf("frame:");
    for (i = 0; i < n; ++i) printf(" %02X", frame[i]);
    printf("\n");

    {
        const gmsg_error e = meter_meter_bus_parse(frame, n, &any);
        if (e != GMSG_OK) {
            printf("parse failed: %s\n", gmsg_error_str(e));
            return 1;
        }
    }
    /* custom-type accessors decode on access: gmsg_error + out parameter */
    if (meter_reading_view_timestamp(&any.as.reading, &ts) != GMSG_OK) return 1;
    printf("%04d-%02d-%02d %02d:%02d:%02d  %.2f kWh\n", ts.year, ts.month, ts.day, ts.hour, ts.minute, ts.second,
           meter_reading_view_kwh_scaled(&any.as.reading));

    {
        /* print uses the `print` hook of the custom type */
        gmsg_printer p;
        gmsg_printer_init_file(&p, stdout, NULL);
        meter_meter_bus_any_view_print(&any, &p);
        (void)gmsg_printer_finish(&p, true, NULL);
    }
    return 0;
}
