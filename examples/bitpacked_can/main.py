"""The Python version of main.cpp: decode a bit-packed (Intel / lsb_first) CAN payload, and build one from values.
    python main.py   (next to the generated can_gmp.py, with gmsg on the path)"""

import sys

import gmsg
import can_gmp as vc

# As received from a CAN controller: 8 data bytes.
payload = bytes([0xE8, 0xC3, 0x82, 0x00, 0x02, 0x19, 0xEF, 0xBE])

try:
    view = vc.EngineDataView.from_bytes(payload)
except gmsg.GmpError as e:
    print(f"invalid frame: {gmsg.error_text(e.code)}")
    sys.exit(1)
s = view.signals
print(f"rpm={s.rpm_scaled:.1f} gear={vc.Gear.name_of(s.gear)} coolant={s.coolant_scaled:.0f}C "
      f"throttle={s.throttle_scaled:.1f}% counter={s.counter} flags=0x{s.flags:04X}")

out = vc.EngineData()
out.signals.rpm = int(3000 / 0.5)
out.signals.gear = vc.Gear.drive
out.signals.coolant = 95 + 40
frame = out.encode()
print("encoded:" + "".join(f" {b:02X}" for b in frame))
