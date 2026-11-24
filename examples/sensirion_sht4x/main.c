/* The C version of main.cpp: talk to a Sensirion SHT4x over I2C. Encode commands, decode the 6-byte responses
 * (with per-word CRC-8). The I2C transfers are simulated with captured bytes; replace them with your bus driver.
 * Same schema (sht4x.yaml), generated with `gmpc --lang c`. */
#include <stdio.h>
#include <string.h>

#include "sht4x.gmp.h"

static void print_bytes(const char* what, const uint8_t* b, size_t n) {
    size_t i;
    printf("%-8s", what);
    for (i = 0; i < n; ++i) printf(" %02X", b[i]);
    printf("\n");
}

int main(void) {
    uint8_t tx[6];
    size_t n = 0;
    static const uint8_t rx[6] = {0x66, 0x66, 0x93, 0x7A, 0xE1, 0xA4};
    static const uint8_t serial_rx[6] = {0x0F, 0x4F, 0x0B, 0x5A, 0x3C, 0x30};
    uint8_t bad[6];
    sensirion_sht4x_measure_high_precision cmd;
    sensirion_sht4x_measurement_response_any_message m;
    sensirion_sht4x_measurement_response_any_view mv;
    sensirion_sht4x_serial_number_view s;
    sensirion_sht4x_serial_number sn;
    sensirion_sht4x_measurement sim;
    gmsg_error e;

    /* 1. Command: measure T & RH with high precision (write 1 byte to 0x44, then wait 8.3 ms). */
    memset(&cmd, 0, sizeof cmd);
    if (sensirion_sht4x_measure_high_precision_encode(&cmd, tx, sizeof tx, &n) != GMSG_OK) return 1;
    print_bytes("write", tx, n);

    /* 2. Response: 6 bytes read back = T word, CRC, RH word, CRC. */
    print_bytes("read", rx, sizeof rx);
    e = sensirion_sht4x_measurement_response_decode(rx, sizeof rx, &m);
    if (e != GMSG_OK) {
        printf("bad response: %s\n", gmsg_error_str(e));
        return 1;
    }
    printf("T = %.2f degC (%.2f degF), RH = %.2f %%RH\n",
           sensirion_sht4x_measurement_temperature_scaled(&m.as.measurement),
           sensirion_sht4x_measurement_temperature_degf(&m.as.measurement),
           sensirion_sht4x_measurement_humidity_scaled(&m.as.measurement));

    /* A corrupted byte is caught by the word CRC. */
    memcpy(bad, rx, sizeof bad);
    bad[1] ^= 0x01;
    printf("corrupted: %s\n", gmsg_error_str(sensirion_sht4x_measurement_response_parse(bad, sizeof bad, &mv)));

    /* 3. Serial number (command 0x89, then read 6 bytes), using the zero-copy view. */
    if (sensirion_sht4x_serial_number_view_from(serial_rx, sizeof serial_rx, &s) == GMSG_OK) {
        /* serial is a computed value: (serial_high << 16) | serial_low, declared in the schema. */
        printf("serial = 0x%08lX\n", (unsigned long)sensirion_sht4x_serial_number_view_serial(&s));
    }

    /* The struct can also set it: set_serial splits the value back into serial_high / serial_low. */
    memset(&sn, 0, sizeof sn);
    sensirion_sht4x_serial_number_set_serial(&sn, 0x0F4F5A3Cu);
    if (sensirion_sht4x_serial_number_encode(&sn, tx, sizeof tx, &n) == GMSG_OK) print_bytes("serial", tx, n);

    /* Encoding a response (e.g. for a sensor simulator) writes the CRCs automatically. */
    memset(&sim, 0, sizeof sim);
    sensirion_sht4x_measurement_set_temperature_scaled(&sim, 25.0);
    sensirion_sht4x_measurement_set_humidity_scaled(&sim, 54.0);
    if (sensirion_sht4x_measurement_encode(&sim, tx, sizeof tx, &n) == GMSG_OK) print_bytes("encoded", tx, n);
    return 0;
}
