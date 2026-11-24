"""The Python version of main.cpp: 0x80 is reserved as the sync byte, so every 0x80 / 0x81 inside a frame is sent as
81 00 / 81 01. Encode a few frames, show the escaping, then cut one frame short and let the stream parser resynchronise
on the next 0x80.
    python main.py   (next to the generated motor_link_gmp.py, with gmsg on the path)"""

import gmsg
import motor_link_gmp as motor

Link = motor.MotorLink


def encode(msg) -> bytes:
    try:
        return Link.encode(msg)
    except gmsg.GmpError as e:
        print(f"encode failed: {gmsg.error_text(e.code)}")
        return b""


def print_wire(label: str, f: bytes) -> None:
    """Print a wire frame with escape pairs in brackets."""
    s = f"{label:<10}"
    i = 0
    while i < len(f):
        if i > 0 and f[i] == 0x81 and i + 1 < len(f):
            s += f" [{f[i]:02X} {f[i + 1]:02X}]"
            i += 1
        else:
            s += f" {f[i]:02X}"
        i += 1
    print(f"{s}   ({len(f)} bytes)")


# --- encoding: reserved bytes in the payload are escaped -----------------------------------------------------------
set_speed = encode(motor.SetSpeed(rpm=1920, direction=motor.Direction.forward, ramp_ms=385))

t = motor.Telemetry(seq=128, rpm=1920)  # seq 0x0080
t.current_scaled = 3.21
t.temperature_scaled = 38.5  # 385 = 0x0181
telemetry = encode(t)

log_frame = encode(motor.Log(text="rpm limit reached"))  # `len` is filled in automatically

t.seq = 129
telemetry2 = encode(t)

print("Escaped frames (escape pairs in brackets: [81 00] = 0x80, [81 01] = 0x81):")
print_wire("SetSpeed", set_speed)
print_wire("Telemetry", telemetry)
print_wire("Log", log_frame)

# --- whole-buffer decoding (Python unescapes into a copy, so the frame is left as it was) --------------------------
v = Link.parse(set_speed)
if isinstance(v, motor.SetSpeedView):
    print(f"\nparse(): SetSpeed rpm={v.rpm} direction={motor.Direction.name_of(v.direction)} ramp={v.ramp_ms}ms")

# --- streaming: a frame cut short, then line noise -----------------------------------------------------------------
wire = telemetry + log_frame[:9] + telemetry2 + bytes([0x13, 0x37]) + log_frame

print(f"\nStream of {len(wire)} bytes, fed in 3-byte chunks:")
parser = Link.stream()
parser.on(motor.Telemetry, lambda m, h: print(
    f"  Telemetry seq={m.seq} rpm={m.rpm} current={m.current_scaled:.2f}A temperature={m.temperature_scaled:.1f}C"))
parser.on(motor.Log, lambda m, h: print(f'  Log "{m.text}"'))
parser.on("error", lambda e: print(f"  dropped {len(e.data)} byte(s): {gmsg.error_text(e.code)}"))
for off in range(0, len(wire), 3):
    parser.feed(wire[off:off + 3])

st = parser.stats
print(f"frames={st.frames} errors={st.frame_errors} skipped={st.bytes_skipped}")
