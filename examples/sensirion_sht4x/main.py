"""The Python version of main.cpp: talk to a Sensirion SHT4x over I2C. Encode commands, decode the 6-byte responses
(with per-word CRC-8). The I2C transfers are simulated with captured bytes.
    python main.py   (next to the generated sht4x_gmp.py, with gmsg on the path)"""

import sys

import gmsg
import sht4x_gmp as sht


def print_bytes(what: str, b: bytes) -> None:
    print(f"{what:<8}" + "".join(f" {x:02X}" for x in b))


# 1. Command: measure T & RH with high precision (write 1 byte to 0x44, then wait 8.3 ms).
print_bytes("write", sht.MeasureHighPrecision().encode())

# 2. Response: 6 bytes read back = T word, CRC, RH word, CRC.
rx = bytes([0x66, 0x66, 0x93, 0x7A, 0xE1, 0xA4])
print_bytes("read", rx)
try:
    meas = sht.MeasurementResponse.decode(rx)  # a Measurement (the protocol's only message)
except gmsg.GmpError as e:
    print(f"bad response: {gmsg.error_text(e.code)}")
    sys.exit(1)
print(f"T = {meas.temperature_scaled:.2f} degC ({meas.temperature_degf:.2f} degF), RH = {meas.humidity_scaled:.2f} %RH")

# A corrupted byte is caught by the word CRC.
bad = bytearray(rx)
bad[1] ^= 0x01
code, _ = gmsg.attempt(lambda: sht.MeasurementResponse.parse(bytes(bad)))
print(f"corrupted: {gmsg.error_text(code)}")

# 3. Serial number (command 0x89, then read 6 bytes), using the zero-copy view.
s = sht.SerialNumberView.from_bytes(bytes([0x0F, 0x4F, 0x0B, 0x5A, 0x3C, 0x30]))
# serial is a computed value: (serial_high << 16) | serial_low, declared in the schema.
print(f"serial = 0x{s.serial:08X}")

# The struct can also set it: the setter splits the value back into serial_high / serial_low.
sn = sht.SerialNumber()
sn.serial = 0x0F4F5A3C
print_bytes("serial", sn.encode())

# Encoding a response (e.g. for a sensor simulator) writes the CRCs automatically.
sim = sht.Measurement()
sim.temperature_scaled = 25.0
sim.humidity_scaled = 54.0
print_bytes("encoded", sim.encode())
