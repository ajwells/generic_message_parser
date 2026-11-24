"""The Python version of main.cpp: encode a few frames, add line noise, and recover the messages with the stream parser.
Same schema (telemetry.yaml), generated with `gmpc --lang python`; run next to the generated module, with the runtime
package gmsg (runtime-py) on the path:  python main.py"""

import gmsg
import telemetry_gmp as demo

wire = bytearray()


def append(msg) -> None:
    wire.extend(demo.Link.encode(msg))


ok = demo.Status(uptime_s=3600)
ok.voltage_scaled = 12.6
ok.state.armed = True
ok.state.mode = demo.Mode.run
append(ok)

wire += bytes([0x13, 0x37, 0xAA])  # line noise

append(demo.Samples(channel=3, values=[100, -100, 250]))  # `count` is filled in automatically

bad = demo.Status(uptime_s=ok.uptime_s, voltage=ok.voltage)
bad.state.armed = True
bad.state.mode = demo.Mode.fault
bad.fault = demo.Fault(code=0x0042, temperature=875)
append(bad)


def on_status(v, header) -> None:
    line = f"Status  uptime={v.uptime_s}s voltage={v.voltage_scaled:.3f}V mode={demo.Mode.name_of(v.state.mode)}"
    if (f := v.fault) is not None:
        line += f" fault=0x{f.code:04X} temp={f.temperature_scaled:.1f}C"
    print(line)


# Listeners per message class (or name); errors are GmpError objects with .code and .data.
parser = demo.Link.stream()
parser.on(demo.Status, on_status)
# to_text dumps every field (enum names, units, nested messages); see README "Printing"
parser.on(demo.Samples, lambda v, header: print(v.to_text(style="single_line")))
parser.on("error", lambda e: print(f"dropped {len(e.data)} byte(s): {gmsg.error_text(e.code)}"))

# Deliver the stream in awkward 5-byte chunks, as a UART driver might.
data = bytes(wire)
for off in range(0, len(data), 5):
    parser.feed(data[off:off + 5])

st = parser.stats
print(f"frames={st.frames} errors={st.frame_errors} skipped={st.bytes_skipped}")
