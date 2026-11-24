/* The C version of main.cpp: decode a bit-packed (Intel / lsb_first) CAN payload in place, and build one from
 * values. Same schema (can.yaml), generated with `gmpc --lang c`. */
#include <stdio.h>
#include <string.h>

#include "can.gmp.h"

int main(void) {
    /* As received from a CAN controller: 8 data bytes. */
    static const uint8_t payload[8] = {0xE8, 0xC3, 0x82, 0x00, 0x02, 0x19, 0xEF, 0xBE};
    vehicle_can_engine_data_view view;
    vehicle_can_engine_data_signals s;
    vehicle_can_engine_data out;
    uint8_t frame[8];
    size_t n = 0, i;
    const char* gear;
    gmsg_error e = vehicle_can_engine_data_view_from(payload, sizeof payload, &view);
    if (e != GMSG_OK) {
        printf("invalid frame: %s\n", gmsg_error_str(e));
        return 1;
    }
    s = vehicle_can_engine_data_view_signals(&view); /* the bit group, unpacked */
    gear = vehicle_can_gear_to_string(s.gear);
    printf("rpm=%.1f gear=%s coolant=%.0fC throttle=%.1f%% counter=%u flags=0x%04X\n",
           vehicle_can_engine_data_signals_rpm_scaled(&s), gear ? gear : "?",
           vehicle_can_engine_data_signals_coolant_scaled(&s), vehicle_can_engine_data_signals_throttle_scaled(&s),
           (unsigned)s.counter, (unsigned)s.flags);

    memset(&out, 0, sizeof out);
    out.signals.rpm = (uint16_t)(3000 / 0.5);
    out.signals.gear = VEHICLE_CAN_GEAR_DRIVE;
    out.signals.coolant = (uint8_t)(95 + 40);
    if (vehicle_can_engine_data_encode(&out, frame, sizeof frame, &n) == GMSG_OK) {
        printf("encoded:");
        for (i = 0; i < n; ++i) printf(" %02X", frame[i]);
        printf("\n");
    }
    return 0;
}
