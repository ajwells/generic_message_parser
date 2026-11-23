"""Cross-check of the generated Python (gmpc --lang python) against the generated C++: the reference results come from
js_crossref (tests/roundtrip_js/js_crossref.cpp). Every case must give the same errors, the same gmp::print text and
the same re-encoded bytes.

    python tests/roundtrip_py/test_generated.py <js_crossref executable> <directory with the generated *_gmp.py>
        [--pure | --native] [-v]
"""

from __future__ import annotations

import importlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("-")]
exe, gen_dir = (os.path.abspath(a) for a in args[:2])
sys.path[:0] = [HERE, os.path.join(HERE, "..", "runtime_py"), gen_dir,
                os.environ.get("GMSG_PATH") or os.path.join(HERE, "..", "..", "runtime-py")]

import gmsg  # noqa: E402
import harness as T  # noqa: E402

if "--pure" in sys.argv:
    gmsg.use_native(False)
if "--native" in sys.argv and not gmsg.use_native(True):
    print("FAIL: --native given but gmsg._native cannot be imported")
    sys.exit(1)
print(f"gmsg {'with' if gmsg.NATIVE else 'without'} the accelerator ({sys.version.split()[0]})")

ref = json.loads(subprocess.run([exe], capture_output=True, check=True).stdout.decode("latin-1"))
modules: dict = {}


def load(schema: str):
    if schema not in modules:
        modules[schema] = importlib.import_module(schema + "_gmp")
    return modules[schema]


attempt = gmsg.attempt


def single(v) -> str:
    return gmsg.to_text(v, style="single_line")


def view_of(V, data):
    return V.from_text(data) if hasattr(V, "from_text") else V.from_bytes(data)


NAN = re.compile(r"\bnan\b")


def check_message(msg):
    mod = load(msg["schema"])
    M, V = getattr(mod, msg["name"], None), getattr(mod, msg["name"] + "View")
    for c in msg["cases"]:
        data = bytes.fromhex(c["hex"])
        what = f"{msg['name']} {c['hex']}"
        d = c.get("decode")
        if d is not None:
            code, m = attempt(lambda: M.decode(data))
            T.eq(int(code), d.get("err", 0), f"{what}: decode error")
            if not code and not d.get("err"):
                T.eq(gmsg.to_text(m), d["multi"], f"{what}: print")
                T.eq(single(m), d["single"], f"{what}: single-line print")
                ec, enc = attempt(lambda: m.encode())
                if "encode" in d:
                    T.eq(int(ec), 0, f"{what}: encode error")
                    # a signalling f32 NaN becomes a quiet NaN in Python floats: compare by what they decode to
                    if not ec and enc.hex() != d["encode"] and NAN.search(d["multi"]):
                        T.eq(gmsg.to_text(M.decode(enc)), gmsg.to_text(M.decode(bytes.fromhex(d["encode"]))),
                             f"{what}: re-encoded NaN")
                    elif not ec:
                        T.eq(enc.hex(), d["encode"], f"{what}: re-encoded bytes")
                else:
                    T.eq(int(ec), d["encodeErr"], f"{what}: encode error code")
        vw = c["view"]
        vc, v = attempt(lambda: view_of(V, data))
        T.eq(int(vc), vw.get("err", 0), f"{what}: view error")
        if not vc and not vw.get("err"):
            T.eq(gmsg.to_text(v), vw["multi"], f"{what}: view print")
            if "size" in vw:
                T.eq(v.wire_size, vw["size"], f"{what}: view size")
            if "extra" in vw:
                T.eq(gmsg.extra_of(v), vw["extra"], f"{what}: data after the message")


def check_protocol(proto):
    P = getattr(load(proto["schema"]), proto["name"])
    for c in proto["frames"]:
        data = bytes.fromhex(c["hex"])
        what = f"{proto['name']} {c['hex']}"
        pc, v = attempt(lambda: P.parse(data))
        T.eq(int(pc), c["parse"].get("err", 0), f"{what}: parse error")
        if not pc and not c["parse"].get("err"):
            T.eq(single(v), c["parse"]["single"], f"{what}: parsed view")
        d = c.get("decode")
        if d is not None:
            dc, m = attempt(lambda: P.decode(data))
            T.eq(int(dc), d.get("err", 0), f"{what}: decode error")
            if not dc and not d.get("err"):
                T.eq(single(m), d["single"], f"{what}: decoded message")
                ec, enc = attempt(lambda: P.encode(m))
                if "encode" in d:
                    T.eq(int(ec), 0, f"{what}: encode error")
                    if not ec:
                        T.eq(enc.hex(), d["encode"], f"{what}: encoded frame")
                else:
                    T.eq(int(ec), d["encodeErr"], f"{what}: encode error code")
    for c in proto["streams"]:
        s = P.stream()
        ev = []

        def on_frame(view, header):
            if header is not None:
                ev.append("H:" + single(header) + "\n")
            ev.append("M:" + single(view) + "\n")

        s.on("frame", on_frame)
        s.on("error", lambda e: ev.append(f"E{int(e.code)}:{len(e.data)}\n"))
        wire = bytes.fromhex(c["hex"])
        if c.get("chunk"):
            for pos in range(0, len(wire), c["chunk"]):
                s.feed(wire[pos:pos + c["chunk"]])
        else:
            # recovery run (js_crossref.cpp): chunks of 3, then the recorded resync / flush / tick ops
            s.set_idle_timeout(2)
            ops = c["ops"]
            state = {"now": 0, "k": 0}

            def op(code):
                state["now"] += 1
                did = s.resync() if code == "r" else s.flush() if code == "f" else s.tick(state["now"]) if code == "t" else False
                ev.append(f"R{1 if did else 0}\n")
                while s.next():
                    pass

            for pos in range(0, len(wire), 3):
                s.feed(wire[pos:pos + 3])
                op(ops[state["k"]])
                state["k"] += 1
            while state["k"] < len(ops):
                op(ops[state["k"]])
                state["k"] += 1
        T.eq("".join(ev), c["events"], f"{proto['name']} stream {c['hex']} chunk {c.get('chunk')}")
        st = s.stats
        T.eq([st.frames, st.frame_errors, st.bytes_skipped, st.overflows, st.resyncs, st.flushes, st.frames_with_extra],
             c["stats"], f"{proto['name']} stream stats")


for msg in ref["messages"]:
    T.test(f"message {msg['schema']}.{msg['name']} ({len(msg['cases'])} inputs)")(lambda msg=msg: check_message(msg))
for proto in ref["protocols"]:
    T.test(f"protocol {proto['schema']}.{proto['name']} ({len(proto['frames'])} frames, {len(proto['streams'])} streams)")(
        lambda proto=proto: check_protocol(proto))

sys.exit(T.run())
