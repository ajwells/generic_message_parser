/* The C version of main_enum.cpp: SHT4x with commands as an enum (sht4x_enum.yaml). Build a command sequence as
 * data, encode each command, pick the response decoder from the command, and decode bytes written by someone else
 * back to a command. The I2C transfers are simulated with captured bytes. */
#include <stdio.h>
#include <string.h>

#include "sht4x_enum.gmp.h"

/* What to read back after a command, derived from the enum. */
typedef enum reply { REPLY_NONE, REPLY_MEASUREMENT, REPLY_SERIAL } reply;

static reply reply_for(sensirion_sht4x_enum_command c) {
    if (c == SENSIRION_SHT4X_ENUM_COMMAND_SOFT_RESET) return REPLY_NONE;
    if (c == SENSIRION_SHT4X_ENUM_COMMAND_READ_SERIAL_NUMBER) return REPLY_SERIAL;
    return REPLY_MEASUREMENT; /* measure_* and heater_* commands */
}

/* Simulated sensor: the 6 bytes it would return for a command. */
static const uint8_t* simulated_read(sensirion_sht4x_enum_command c) {
    static const uint8_t serial[6] = {0x0F, 0x4F, 0x0B, 0x5A, 0x3C, 0x30};
    static const uint8_t measurement[6] = {0x66, 0x66, 0x93, 0x7A, 0xE1, 0xA4}; /* 25.00 degC, 54.00 %RH */
    return c == SENSIRION_SHT4X_ENUM_COMMAND_READ_SERIAL_NUMBER ? serial : measurement;
}

int main(void) {
    static const sensirion_sht4x_enum_command sequence[] = {
        SENSIRION_SHT4X_ENUM_COMMAND_SOFT_RESET, SENSIRION_SHT4X_ENUM_COMMAND_READ_SERIAL_NUMBER,
        SENSIRION_SHT4X_ENUM_COMMAND_MEASURE_HIGH_PRECISION, SENSIRION_SHT4X_ENUM_COMMAND_HEATER_20MW_100MS};
    static const uint8_t sniffed[] = {0xE0, 0x42};
    size_t i;

    for (i = 0; i < sizeof sequence / sizeof sequence[0]; ++i) {
        const sensirion_sht4x_enum_command c = sequence[i];
        sensirion_sht4x_enum_command_frame f;
        uint8_t tx[1];
        size_t n = 0;
        f.command = c;
        if (sensirion_sht4x_enum_command_frame_encode(&f, tx, sizeof tx, &n) != GMSG_OK) return 1;
        printf("write %02X  %-24s", tx[0], sensirion_sht4x_enum_command_to_string(c));

        switch (reply_for(c)) {
            case REPLY_NONE: printf("(no response)\n"); break;
            case REPLY_SERIAL: {
                sensirion_sht4x_enum_serial_number_view s;
                const gmsg_error e = sensirion_sht4x_enum_serial_number_view_from(simulated_read(c), 6, &s);
                if (e != GMSG_OK) {
                    printf("bad response: %s\n", gmsg_error_str(e));
                    return 1;
                }
                printf("serial = 0x%08lX\n", (unsigned long)sensirion_sht4x_enum_serial_number_view_serial(&s));
                break;
            }
            case REPLY_MEASUREMENT: {
                sensirion_sht4x_enum_measurement_response_any_message m;
                const gmsg_error e = sensirion_sht4x_enum_measurement_response_decode(simulated_read(c), 6, &m);
                if (e != GMSG_OK) {
                    printf("bad response: %s\n", gmsg_error_str(e));
                    return 1;
                }
                printf("T = %.2f degC (%.2f degF), RH = %.2f %%RH\n",
                       sensirion_sht4x_enum_measurement_temperature_scaled(&m.as.measurement),
                       sensirion_sht4x_enum_measurement_temperature_degf(&m.as.measurement),
                       sensirion_sht4x_enum_measurement_humidity_scaled(&m.as.measurement));
                break;
            }
        }
    }

    /* Decoding a command byte (e.g. in a bus sniffer). Command is `strict: true` in the schema, so a byte that is
     * not a known command fails with GMSG_ERR_UNKNOWN_ENUM. */
    for (i = 0; i < sizeof sniffed; ++i) {
        sensirion_sht4x_enum_command_frame f;
        const gmsg_error e = sensirion_sht4x_enum_command_frame_decode(&sniffed[i], 1, &f);
        if (e == GMSG_OK) printf("sniffed %02X -> %s\n", sniffed[i], sensirion_sht4x_enum_command_to_string(f.command));
        else printf("sniffed %02X -> error: %s\n", sniffed[i], gmsg_error_str(e));
    }
    return 0;
}
