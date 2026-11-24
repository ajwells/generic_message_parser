"""The Python version of main.cpp: parse NMEA 0183 sentences (text protocol, XOR checksum, optional fields), print one,
and encode one. Uses the iterator form of the pull API.
    python main.py   (next to the generated nmea_gmp.py, with gmsg on the path)"""

import sys

import gmsg
import nmea_gmp as nm


def value_or(get, fallback):
    """The value of a text field, or `fallback` when it fails to parse (text views parse on access and raise)."""
    code, v = gmsg.attempt(get)
    return fallback if code else v


log = (
    b"$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"
    b"$GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n"  # bad checksum
    b"$GPTXT,01,01,02,ANTSTATUS=OK*3B\r\n"
    b"$GPGGA,000000,,,,,0,00,,,,,,,*66\r\n"
)

parser = nm.Nmea.stream()
printed = False
for ev in parser.events(log):
    if isinstance(ev, gmsg.GmpError):
        print(f"rejected: {gmsg.error_text(ev.code)}")
    elif isinstance(ev.message, nm.GGAView):
        g = ev.message
        line = (f"GGA time={value_or(lambda: g.utc_time, 0.0):.0f}"
                f" quality={nm.FixQuality.name_of(value_or(lambda: g.quality, nm.FixQuality.invalid))}"
                f" sats={value_or(lambda: g.satellites, 0)}")
        lat = value_or(lambda: g.latitude, None)  # optional field: None when the token is empty
        line += f" lat={lat:.3f}" if lat is not None else " (no fix)"
        print(line)
        if not printed:
            printed = True
            print(g.to_text())  # the whole sentence, every field
    elif isinstance(ev.message, nm.TXTView):
        print(f'TXT "{ev.message.text}"')

txt = nm.TXT(total=1, number=1, severity=2, text="HELLO")
sys.stdout.write("encoded: " + nm.Nmea.encode(txt).decode("latin-1"))
