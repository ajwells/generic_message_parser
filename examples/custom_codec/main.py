"""The Python version of main.cpp: a custom field codec and a custom checksum, wired in from the schema's `python:`
blocks (meter.yaml -> meter_codecs.py).
    python main.py   (next to the generated meter_gmp.py and meter_codecs.py, with gmsg on the path)"""

import sys

import gmsg
import meter_gmp as meter
from meter_codecs import DateTime

r = meter.Reading(timestamp=DateTime(2026, 11, 20, 14, 30, 5))
r.kwh_scaled = 12345.67

frame = meter.MeterBus.encode(r)
print("frame:" + "".join(f" {b:02X}" for b in frame))

try:
    v = meter.MeterBus.parse(frame)
except gmsg.GmpError as e:
    print(f"parse failed: {gmsg.error_text(e.code)}")
    sys.exit(1)
ts = v.timestamp  # custom-type accessors decode on access (and raise GmpError)
print(f"{ts.year:04d}-{ts.month:02d}-{ts.day:02d} {ts.hour:02d}:{ts.minute:02d}:{ts.second:02d}  {v.kwh_scaled:.2f} kWh")

# print uses the `print` hook of the custom type (meter_codecs.py)
print(v.to_text())
