"""Tests of runtime-py/gmsg: unit checks, and a cross-check against the C++ runtime using the reference vectors printed
by js_vectors (tests/runtime_js/js_vectors.cpp; the vectors do not depend on the language).

    python tests/runtime_py/test_runtime.py <path to js_vectors executable> [--pure | --native] [-v]

--pure turns the accelerator off, --native requires it (fails when gmsg._native cannot be imported).
"""

from __future__ import annotations

import json
import math
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.environ.get("GMSG_PATH") or os.path.join(HERE, "..", "..", "runtime-py"))

import gmsg  # noqa: E402
import harness as T  # noqa: E402
from gmsg import Err  # noqa: E402

if "--pure" in sys.argv:
    gmsg.use_native(False)
if "--native" in sys.argv and not gmsg.use_native(True):
    print("FAIL: --native given but gmsg._native cannot be imported")
    sys.exit(1)
print(f"gmsg {'with' if gmsg.NATIVE else 'without'} the accelerator ({sys.version.split()[0]})")


def hexb(h: str) -> bytes:
    return bytes.fromhex(h)


def f64(h: str) -> float:
    return struct.unpack(">d", bytes.fromhex(h))[0]


def f32(h: str) -> float:
    return struct.unpack(">f", bytes.fromhex(h))[0]


def bits_hex(x: float, is_f32: bool) -> str:
    if is_f32:
        return struct.pack(">f", x).hex().rjust(16, "0")
    return struct.pack(">d", x).hex()


attempt = gmsg.attempt


# ================================================================================================================
# unit checks
# ================================================================================================================
@T.test("error codes and texts match gmsg::Error")
def _():
    T.eq(int(Err.UNKNOWN_ENUM), 14)
    T.eq(int(Err.VALUE_OUT_OF_LIMITS), 15)
    T.eq(gmsg.error_text(Err.BAD_CHECKSUM), "bad checksum")
    T.eq(gmsg.error_text(Err.UNKNOWN_TYPE), "unknown message type")
    T.eq(str(gmsg.GmpError(Err.TRUNCATED)), "truncated")
    T.eq(str(gmsg.GmpError(Err.OUT_OF_RANGE, "x")), "out of range: x")
    T.eq(gmsg.GmpError(Err.OUT_OF_RANGE).text, "out of range")


@T.test("Reader / Writer round trip every type in both byte orders")
def _():
    for e in ("big", "little"):
        w = gmsg.Writer()
        for t, v in (("u8", 0xAB), ("i8", -2), ("u16", 0x1234), ("i16", -3), ("u32", 0xDEADBEEF), ("i32", -4),
                     ("u64", 0xFFFFFFFFFFFFFFFF), ("i64", -5), ("f32", 0.5), ("f64", math.pi), ("bool", True),
                     ("char", "A")):
            w.write(t, v, e)
        r = gmsg.Reader(w.to_bytes())
        for t, v in (("u8", 0xAB), ("i8", -2), ("u16", 0x1234), ("i16", -3), ("u32", 0xDEADBEEF), ("i32", -4),
                     ("u64", 0xFFFFFFFFFFFFFFFF), ("i64", -5), ("f32", 0.5), ("f64", math.pi), ("bool", True),
                     ("char", "A")):
            T.eq(r.read(t, e), v, f"{t} {e}")
        T.eq(r.remaining, 0)
        T.eq(attempt(lambda: r.read("u8"))[0], Err.TRUNCATED)
    w = gmsg.Writer()
    w.write("u16", 0x0102)
    T.eq(w.to_bytes().hex(), "0102")
    T.eq(attempt(lambda: w.write("u8", 256))[0], Err.OUT_OF_RANGE)
    T.eq(attempt(lambda: w.write("i8", -129))[0], Err.OUT_OF_RANGE)
    T.eq(attempt(lambda: w.write("u16", 1.5))[0], Err.OUT_OF_RANGE)
    T.eq(attempt(lambda: w.write("u64", -1))[0], Err.OUT_OF_RANGE)
    T.eq(attempt(lambda: w.write("char", "ab"))[0], Err.OUT_OF_RANGE)
    w2 = gmsg.Writer()
    w2.write("f32", 1e300)  # beyond the float range: inf, as static_cast<float>
    T.eq(w2.to_bytes().hex(), "7f800000")
    w3 = gmsg.Writer()
    w3.write("f32", 3.4028235e38)  # rounds to FLT_MAX
    T.eq(w3.to_bytes().hex(), "7f7fffff")


@T.test("wrap_int / to_int / llround / idiv follow C++")
def _():
    T.eq(gmsg.wrap_int(300, "u8"), 44)
    T.eq(gmsg.wrap_int(-1, "u16"), 65535)
    T.eq(gmsg.wrap_int(200, "i8"), -56)
    T.eq(gmsg.wrap_int(-1, "u64"), 18446744073709551615)
    T.eq(gmsg.wrap_int(2.9, "i32"), 2)
    T.eq(gmsg.wrap_int(-2.9, "i32"), -2)
    T.eq(gmsg.to_int("\xff"), -1)
    T.eq(gmsg.to_int(True), 1)
    T.eq(gmsg.llround(2.5), 3)
    T.eq(gmsg.llround(-2.5), -3)
    T.eq(gmsg.llround(0.49999999999999994), 0)
    T.eq(gmsg.idiv(-7, 2), -3)
    T.eq(gmsg.imod(-7, 2), -1)
    T.eq(attempt(lambda: gmsg.idiv(1, 0))[0], Err.INVALID_VALUE)


@T.test("checksum check values (reveng catalogue, '123456789')")
def _():
    data = b"123456789"
    check = {
        "crc8": 0xF4, "crc8_maxim": 0xA1, "crc8_autosar": 0xDF, "crc8_nrsc5": 0xF7, "crc15_can": 0x059E,
        "crc16_ccitt": 0x29B1, "crc16_xmodem": 0x31C3, "crc16_kermit": 0x2189, "crc16_x25": 0x906E,
        "crc16_modbus": 0x4B37, "crc16_arc": 0xBB3D, "crc24_openpgp": 0x21CF02, "crc32": 0xCBF43926,
        "crc32c": 0xE3069283, "crc32_mpeg2": 0x0376E6E7, "crc64_ecma": 0x6C40DF5F0B497347,
    }
    for name, v in check.items():
        T.eq(gmsg.checksum.PRESETS[name].compute(data), v, name)
    a = gmsg.checksum.crc32
    T.eq(a.finish(a.update(a.update(a.init(), data, 0, 4), data, 4)), 0xCBF43926, "incremental")
    T.eq(gmsg.checksum.crc32.compute(memoryview(bytearray(data))), 0xCBF43926, "memoryview")


@T.test("float text examples")
def _():
    T.eq(gmsg.format_float(gmsg.to_f32(0.9), -1, True), "0.9")
    T.eq(gmsg.format_float(0.1 + 0.2), "0.30000000000000004")
    T.eq(gmsg.format_float(1e21), "1e+21")
    T.eq(gmsg.format_float(123456.0), "123456")
    T.eq(gmsg.format_float(441382624.0, -1, True), "441382624")
    T.eq(gmsg.format_float(2.5, 0), "2")
    T.eq(gmsg.format_float(-0.0), "-0")
    T.eq(gmsg.format_general(1234567885.0, 9), "1.23456788e+09")
    T.eq(gmsg.format_general(0.0001, 9), "0.0001")
    T.eq(gmsg.parse_float("+1.5"), 1.5)
    T.eq(attempt(lambda: gmsg.parse_float(""))[0], Err.MISSING_FIELD)
    T.eq(attempt(lambda: gmsg.parse_float("1e400"))[0], Err.OUT_OF_RANGE)
    T.eq(attempt(lambda: gmsg.parse_float(" 1"))[0], Err.PARSE_ERROR)


@T.test("printer: enum widths, lists and notes")
def _():
    p = gmsg.Printer(style="single_line")
    p.begin("M")
    p.key("e")
    p.enum_value("", -1, 2)
    p.key("v")
    p.int_value(5)
    p.conversion(0.5, "V")
    p.note("computed")
    p.end()
    T.eq(p.text(), "M { e: <unknown> (0xFFFF), v: 5 (0.5 V, computed) }")


@T.test("stream parser: listeners, pull API, iterators, resync / flush / tick")
def _():
    class View:  # frames: 0x24 <len> <payload...>; payload byte 0 odd = bad checksum
        message_name = "Ping"

        def __init__(self, b):
            self.b = b

        def to_struct(self):
            if self.b[2] == 0xFE:
                gmsg.fail(Err.OUT_OF_RANGE)
            return {"n": self.b[2]}

    cfg = gmsg.LengthPrefixedConfig(sync=b"\x24", length_offset=1, length_size=1, add=2, max_frame=16)

    class Header:
        def __init__(self, n):
            self.len = n

    class Protocol:
        max_frame = 16
        scan = staticmethod(lambda b: gmsg.scan_length_prefixed(b, cfg))

        @staticmethod
        def process(f):
            if f[2] & 1:
                gmsg.fail(Err.BAD_CHECKSUM)
            return gmsg.Received(View(f), Header(f[1]))

    data = bytes([0x24, 1, 2, 0x99, 0x24, 1, 3, 0x24, 2, 4, 4, 0x24, 1, 0xFE])
    s = gmsg.StreamParser(Protocol)
    seen = []
    s.on("Ping", lambda v, h: seen.append(f"P{v.b[2]}/{h.len}"))
    s.on("frame", lambda v, h: seen.append("f"))
    off = s.on("error", lambda e: seen.append(f"E{int(e.code)}:{len(e.data)}"))
    T.eq(s.feed(data), 3)
    T.eq(" ".join(seen), "P2/1 f E2:1 E3:3 E2:2 P4/2 f P254/1 f")
    T.eq(s.stats.frames, 3)
    T.eq(s.stats.frame_errors, 1)
    off()

    p = gmsg.StreamParser(Protocol)
    T.eq([v.b[2] for v in p.messages(data)], [2, 4, 254])
    T.eq(p.last_error, Err.BAD_SYNC)
    q = gmsg.StreamParser(Protocol)
    T.eq([f"E{int(e.code)}" if isinstance(e, gmsg.GmpError) else f"M{e.message.b[2]}" for e in q.events(data)],
         ["M2", "E2", "E3", "E2", "M4", "M254"])
    r = gmsg.StreamParser(Protocol)
    errs = []
    r.on("error", lambda e: errs.append(e.code))
    r.push(data)
    T.eq(r.poll_message(), {"n": 2})
    T.eq(r.poll_message(), {"n": 4})
    T.eq(r.poll_message(), None)
    T.eq(errs, [Err.BAD_SYNC, Err.BAD_CHECKSUM, Err.BAD_SYNC, Err.OUT_OF_RANGE, Err.BAD_SYNC])
    t = gmsg.StreamParser(Protocol)
    other = []
    t.on("frame", lambda v, h: other.append(v.b[2]))
    t.push(data)
    T.eq(t.poll_for("Ping").b[2], 2)
    fr = t.poll_frame()
    T.eq(fr.bytes.hex(), "24020404")
    T.eq(fr.header.len, 2)
    T.eq(fr.offset, 7)
    c = gmsg.StreamParser(Protocol, capacity=8)
    T.eq(c.push(bytes([0x24, 1, 2, 0x24, 1])), 5)
    T.eq(c.poll().b[2], 2)
    T.eq(c.push(bytes([4, 0x24, 2, 6, 6])), 5)  # fits only after compaction
    T.eq(c.poll().b[2], 4)
    T.eq(c.poll().b[2], 6)

    # recovery: a length claiming 9 more bytes than arrive holds back the frame behind it
    stuck = bytes([0x24, 9, 2]) + bytes([0x24, 1, 2])
    u = gmsg.StreamParser(Protocol)
    events = []
    u.on("error", lambda e: events.append((int(e.code), len(e.data))))
    u.set_idle_timeout(10)
    u.feed(stuck)
    T.eq(u.poll(), None)
    T.eq(u.tick(100), False)  # first tick: starts the clock
    T.eq(u.tick(105), False)
    T.eq(u.tick(110), True)  # idle for 10: resync
    T.eq(events, [(int(Err.TRUNCATED), 6)])
    T.eq(u.poll().b[2], 2)
    T.eq(u.stats.resyncs, 1)
    v = gmsg.StreamParser(Protocol)
    v.push(stuck)
    T.eq(v.flush(), True)
    T.eq(v.buffered, 0)
    T.eq(v.stats.flushes, 1)
    T.eq(v.flush(), False)


# ================================================================================================================
# cross-check against the C++ runtime
# ================================================================================================================
exe = next((a for a in sys.argv[1:] if not a.startswith("-")), None)
V = json.loads(subprocess.run([os.path.abspath(exe)], capture_output=True, check=True).stdout.decode("latin-1")) if exe else None


def vectors(name):
    def deco(fn):
        def run():
            if V is None:
                return T.skip("no js_vectors executable given")
            fn()

        T.test(name)(run)
        return fn

    return deco


@vectors("checksum presets match C++")
def _():
    for name, h, value in V["checksums"]:
        T.eq(str(gmsg.checksum.PRESETS[name].compute(hexb(h))), value, f"{name} {h}")


@vectors("generic CRCs with random parameters match the C runtime")
def _():
    for width, poly, init, refin, refout, xorout, h, value in V["crc_generic"]:
        a = gmsg.checksum.crc(width, int(poly), int(init), refin, refout, int(xorout))
        T.eq(str(a.compute(hexb(h))), value, f"width {width} poly {poly} refin {refin} refout {refout}")


@vectors("bit extraction, sign extension and insertion match C++")
def _():
    for h, off, n, order, v, sv, value, written in V["bits"]:
        b = hexb(h)
        T.eq(str(gmsg.extract_bits(b, off, n, order)), v, f"extract {h} {off} {n} {order}")
        T.eq(str(gmsg.get_bits(b, off, n, order, "i64")), sv, f"signed {h} {off} {n}")
        w = bytearray(b)
        gmsg.set_bits(w, off, n, order, int(value))
        T.eq(w.hex(), written, f"insert {h} {off} {n} {order}")


@vectors("float formatting matches std::to_chars")
def _():
    for kind, h, mode, precision, text in V["float_format"]:
        is_f32 = kind == "f32"
        x = f32(h[8:]) if is_f32 else f64(h)
        got = (gmsg.format_float(x, -1, is_f32) if mode == 0 else gmsg.format_float(x, precision, is_f32) if mode == 1
               else gmsg.format_general(x, precision))
        T.eq(got, text, f"{kind} {h} mode {mode} precision {precision}")


@vectors("float parsing matches std::from_chars")
def _():
    for s, de, dbits, fe, fbits in V["float_parse"]:
        dc, dv = attempt(lambda: gmsg.parse_float(s, False))
        T.eq(int(dc), de, f'f64 error for "{s}"')
        if not dc and not math.isnan(dv):
            T.eq(bits_hex(dv, False), dbits, f'f64 bits for "{s}"')
        fc, fv = attempt(lambda: gmsg.parse_float(s, True))
        T.eq(int(fc), fe, f'f32 error for "{s}"')
        if not fc and not math.isnan(fv):
            T.eq(bits_hex(fv, True), fbits, f'f32 bits for "{s}"')


@vectors("integer parsing matches gmsg::text::parse_int")
def _():
    for t, s, base, err, value in V["int_parse"]:
        c, v = attempt(lambda: gmsg.text.parse_int(s, t, base))
        T.eq(int(c), err, f'{t} "{s}" base {base}')
        if not c:
            T.eq(str(v), value, f'{t} "{s}" base {base}')


@vectors("TextWriter tokens match C++")
def _():
    for kind, value, a, width, upper, text in V["text_writer"]:
        w = gmsg.text.TextWriter(",")
        if kind == "int":
            w.int_token(int(value), a, width, upper)
        else:
            w.float_token(f32(value[8:]) if kind == "f32" else f64(value), a, width, kind == "f32")
        T.eq(w.text(), text, f"{kind} {value} {a} {width} {upper}")


X = {"xor": gmsg.xor_escape(0x7D, 0x20, [0x7E]), "index": gmsg.index_escape(0x81, [0x80, 0x81]),
     "map": gmsg.map_escape(0xDB, [0xC0, 0xDB], [0xDC, 0xDD])}
ESC = [X["xor"], X["index"], X["map"]]
SCAN = [
    lambda b, c=gmsg.FixedConfig(sync=b"\xAA\x55", frame_size=6): gmsg.scan_fixed(b, c),
    lambda b, c=gmsg.FixedConfig(frame_size=4): gmsg.scan_fixed(b, c),
    lambda b, c=gmsg.FixedConfig(sync=b"\x80", frame_size=5, escape=X["index"]): gmsg.scan_fixed(b, c),
    lambda b, c=gmsg.LengthPrefixedConfig(sync=b"\xAA\x55", length_offset=2, length_size=1, length_endian="big", add=5,
                                          min_frame=5, max_frame=24): gmsg.scan_length_prefixed(b, c),
    lambda b, c=gmsg.LengthPrefixedConfig(length_offset=0, length_size=2, length_endian="little", add=-1, min_frame=3,
                                          max_frame=20): gmsg.scan_length_prefixed(b, c),
    lambda b, c=gmsg.LengthPrefixedConfig(sync=b"\x7E", length_offset=1, length_size=1, add=4, min_frame=4, max_frame=32,
                                          escape=X["xor"]): gmsg.scan_length_prefixed(b, c),
    lambda b, c=gmsg.LengthPrefixedConfig(sync=b"\x7E", length_offset=1, length_size=1, add=0, min_frame=4, max_frame=32,
                                          escape=X["xor"], counts_wire=True, header_len=2,
                                          trailer_len=1): gmsg.scan_length_prefixed(b, c),
    lambda b, c=gmsg.DelimitedConfig(start=b"\x7E", end=b"\x7E", escape=X["xor"], max_frame=24): gmsg.scan_delimited(b, c),
    lambda b, c=gmsg.DelimitedConfig(start=b"\x24", end=b"\x0D\x0A", max_frame=16): gmsg.scan_delimited(b, c),
]
KIND = {"need_more": 0, "frame": 1, "skip": 2}


@vectors("framing scanners match C++")
def _():
    for which, h, kind, size, consume, on_error, error in V["scan"]:
        s = SCAN[which](hexb(h))
        T.eq([KIND[s.kind], s.size, s.consume, s.consume_on_error, int(s.error)], [kind, size, consume, on_error, error],
             f"config {which} {h}")


@vectors("escaping matches C++")
def _():
    walk = {"done": 0, "need_more": 1, "bad": 2}
    for which, h, uerr, unesc, esc, count, pick_at, wkind, wire, werr, pick in V["escape"]:
        cfg, b = ESC[which], hexb(h)
        c, u = attempt(lambda: gmsg.unescape(b, cfg))
        T.eq(int(c), uerr, f"unescape {which} {h}")
        if not c:
            T.eq(u.hex(), unesc, f"unescape {which} {h}")
        padded = b"\xEE\xEE" + b
        T.eq(gmsg.escape_range(padded, 2, len(b), cfg).hex(), esc, f"escape {which} {h}")
        pk = bytearray(4)
        w = gmsg.walk_escaped(b, 0, count, cfg, pick_at, pk)
        T.eq([walk[w.kind], w.wire, int(w.error)], [wkind, wire, werr], f"walk {which} {h} {count}")
        if w.kind == "done":
            T.eq(pk.hex(), pick, f"walk pick {which} {h}")


@vectors("stream parser matches gmsg::StreamParser (events, resync, statistics)")
def _():
    class Frame:
        message_name = "Frame"

        def __init__(self, b):
            self.b = b

    for which, capacity, chunk, h, events, stats in V["stream"]:
        class Protocol:
            max_frame = capacity
            scan = staticmethod(SCAN[which])

            @staticmethod
            def process(f):
                if not f or f[-1] & 1:
                    gmsg.fail(Err.BAD_CHECKSUM)
                return gmsg.Received(Frame(f))

        s = gmsg.StreamParser(Protocol, capacity=capacity)
        ev = []
        s.on("frame", lambda v, hd: ev.append(f"F{len(v.b)} "))
        s.on("error", lambda e: ev.append(f"E{int(e.code)}:{len(e.data)} "))
        data = hexb(h)
        for pos in range(0, len(data), chunk):
            s.feed(data[pos:pos + chunk])
        T.eq("".join(ev), events, f"config {which} capacity {capacity} chunk {chunk}")
        T.eq([s.stats.frames, s.stats.frame_errors, s.stats.bytes_skipped, s.stats.overflows], stats,
             f"stats {which} {capacity} {chunk}")


@vectors("printer matches gmsg::Printer for every option set")
def _():
    class Inner:
        def __init__(self, i):
            self.i = i

        def print_to(self, q):
            q.begin("Inner")
            q.key("id")
            q.int_value(self.i)
            q.end()

    class Scenario:
        def print_to(self, p):
            p.begin("Scenario")
            p.key("u"); p.int_value(4000000000)
            p.key("i"); p.int_value(-12)
            p.key("big"); p.int_value(18446744073709551615)
            p.key("f32"); p.float_value(gmsg.to_f32(0.9), True)
            p.key("f64"); p.float_value(1e21)
            p.key("tiny"); p.float_value(5e-324)
            p.key("flag"); p.bool_value(True)
            p.key("ch"); p.char_value("'")
            p.key("ch2"); p.char_value(7)
            p.key("mode"); p.enum_value("run", 1, 1)
            p.key("bad_mode"); p.enum_value("", 7, 1)
            p.key("raw"); p.int_value(12600); p.unit("mV")
            p.key("scaled"); p.int_value(12600); p.conversion(12.6, "V"); p.conversion(1234567885, "x")
            p.conversion(0.000012345678901, "")
            p.key("bytes"); p.hex_bytes(bytes([0, 1, 0xAB, 0xFF, 0x10]))
            p.key("text"); p.str_value('a"b\\c\n\x01')
            p.absent("maybe")
            p.key("ints"); p.list_value([1, 2, 3, 4, 5], p.int_value)
            p.key("empty_list"); p.list_value([], p.int_value)
            p.key("nested"); p.list_value([Inner(1), Inner(2), Inner(3)], lambda v: v.print_to(p), True)
            p.key("empty"); p.begin("Empty"); p.end()
            p.key("bits"); p.begin(""); p.key("a"); p.int_value(1); p.key("b"); p.bool_value(False); p.end()
            p.key("total"); p.int_value(42); p.unit("ms"); p.note("computed")
            p.key("err"); p.error_value(Err.PARSE_ERROR)
            p.end()

    for style, indent, max_items, max_bytes, precision, text in V["print"]:
        got = gmsg.to_text(Scenario(), style=style, indent=indent, max_items=max_items, max_bytes=max_bytes,
                           precision=precision)
        T.eq(got, text, f"{style} indent {indent} items {max_items} bytes {max_bytes} precision {precision}")


sys.exit(T.run())
