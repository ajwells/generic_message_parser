"""The Python version of main_enum.cpp: SHT4x with commands as an enum (sht4x_enum.yaml). Build a command sequence as
data, encode each command, pick the response decoder from the command, and decode bytes written by someone else back to
a command.
    python main_enum.py   (next to the generated sht4x_enum_gmp.py, with gmsg on the path)"""

import sys

import gmsg
import sht4x_enum_gmp as sht

Command = sht.Command


def reply_for(c: Command) -> str:
    """What to read back after a command, derived from the enum."""
    if c == Command.soft_reset:
        return "none"
    if c == Command.read_serial_number:
        return "serial"
    return "measurement"  # measure_* and heater_* commands


def simulated_read(c: Command) -> bytes:
    """Simulated sensor: the 6 bytes it would return for a command."""
    if c == Command.read_serial_number:
        return bytes([0x0F, 0x4F, 0x0B, 0x5A, 0x3C, 0x30])
    return bytes([0x66, 0x66, 0x93, 0x7A, 0xE1, 0xA4])  # 25.00 degC, 54.00 %RH


# A command sequence is plain data.
for c in (Command.soft_reset, Command.read_serial_number, Command.measure_high_precision, Command.heater_20mw_100ms):
    tx = sht.CommandFrame(command=c).encode()
    line = f"write {tx[0]:02X}  {Command.name_of(c):<24}"
    try:
        kind = reply_for(c)
        if kind == "none":
            line += "(no response)"
        elif kind == "serial":
            line += f"serial = 0x{sht.SerialNumberView.from_bytes(simulated_read(c)).serial:08X}"  # computed value
        else:
            m = sht.MeasurementResponse.decode(simulated_read(c))
            line += (f"T = {m.temperature_scaled:.2f} degC ({m.temperature_degf:.2f} degF), "
                     f"RH = {m.humidity_scaled:.2f} %RH")
    except gmsg.GmpError as e:
        print(line + f"bad response: {gmsg.error_text(e.code)}")
        sys.exit(1)
    print(line)

# Decoding a command byte (e.g. in a bus sniffer). Command is `strict: true` in the schema, so a byte that is not a
# known command fails with UNKNOWN_ENUM.
for b in (0xE0, 0x42):
    code, f = gmsg.attempt(lambda: sht.CommandFrame.decode(bytes([b])))
    print(f"sniffed {b:02X} -> error: {gmsg.error_text(code)}" if code else f"sniffed {b:02X} -> {Command.name_of(f.command)}")
