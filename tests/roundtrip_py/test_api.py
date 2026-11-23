"""The Python API of generated code (api.yaml): what the cross-check against C++ does not reach - logical conditions,
an f32 computed value, container size checks on encode, keyword names, enum members, text extra tokens, protocol
headers given as a dict, and the accelerator switch.

    python tests/roundtrip_py/test_api.py <directory with api_gmp.py> [--pure | --native] [-v]
"""

from __future__ import annotations

import dataclasses
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
gen_dir = os.path.abspath(next(a for a in sys.argv[1:] if not a.startswith("-")))
sys.path[:0] = [HERE, os.path.join(HERE, "..", "runtime_py"), gen_dir,
                os.environ.get("GMSG_PATH") or os.path.join(HERE, "..", "..", "runtime-py")]

import gmsg  # noqa: E402
import harness as T  # noqa: E402
from gmsg import Err  # noqa: E402

if "--pure" in sys.argv:
    gmsg.use_native(False)
if "--native" in sys.argv and not gmsg.use_native(True):
    print("FAIL: --native given but gmsg._native cannot be imported")
    sys.exit(1)

import api_gmp as api  # noqa: E402
from api_gmp import Flags, Kind, Link, Loose, Rec  # noqa: E402


def code_of(fn) -> int:
    return int(gmsg.attempt(fn)[0])


def frame(a: int, b: int) -> bytes:
    """Flags with the given a / b, conditional fields set as the conditions require."""
    m = Flags(a=a, b=b, kind=Kind.class_, from_=7, blob=b"xyz", pair=[1, 2], wide=5, tail=[9])
    if a > 0 and b > 0:
        m.both = 0x1111
    if a == 1 or not b < 5:
        m.either = 0x2222
    return m.encode()


@T.test("logical conditions (&&, ||, !)")
def _():
    for a, b, both, either in ((0, 0, False, False), (1, 0, False, True), (2, 3, True, False), (2, 7, True, True),
                               (0, 9, False, True), (1, 1, True, True)):
        data = frame(a, b)
        m = Flags.decode(data)
        T.eq(m.both is not None, both, f"both a={a} b={b}")
        T.eq(m.either is not None, either, f"either a={a} b={b}")
        v = Flags.view(data)
        T.eq(v.both is not None, both, f"view both a={a} b={b}")
        T.eq(v.either is not None, either, f"view either a={a} b={b}")
        T.eq(v.wire_size, len(data), f"view size a={a} b={b}")
        T.eq(m, v.to_struct(), f"to_struct a={a} b={b}")
    # a condition that holds needs the field
    T.eq(code_of(lambda: Flags(a=1, b=1, both=1, kind=Kind.class_).encode()), Err.MISSING_FIELD)


@T.test("computed f32 value rounds as float")
def _():
    m = Flags(wide=16777217)
    T.eq(m.wide_f32, 16777216.0)
    T.eq(Flags.view(dataclasses.replace(m, kind=Kind.class_).encode()).wide_f32, 16777216.0)


@T.test("container sizes and value ranges are checked on encode")
def _():
    ok = Flags(kind=Kind.name_)
    T.eq(code_of(ok.encode), Err.NONE)
    T.eq(code_of(dataclasses.replace(ok, blob=b"ab").encode), Err.OUT_OF_RANGE)
    T.eq(code_of(dataclasses.replace(ok, blob=b"abcd").encode), Err.OUT_OF_RANGE)
    T.eq(code_of(dataclasses.replace(ok, pair=[1]).encode), Err.OUT_OF_RANGE)
    T.eq(code_of(dataclasses.replace(ok, pair=[1, 2, 3]).encode), Err.OUT_OF_RANGE)
    T.eq(code_of(dataclasses.replace(ok, tail=[1, 2, 3, 4]).encode), Err.OUT_OF_RANGE)
    T.eq(code_of(dataclasses.replace(ok, a=256).encode), Err.OUT_OF_RANGE)
    T.eq(code_of(dataclasses.replace(ok, wide=-1).encode), Err.OUT_OF_RANGE)
    T.eq(code_of(dataclasses.replace(ok, kind=9).encode), Err.UNKNOWN_ENUM)
    T.eq(code_of(dataclasses.replace(ok, loose=9).encode), Err.NONE)  # not strict
    T.eq(code_of(lambda: Flags.decode(bytes(4))), Err.TRUNCATED)


@T.test("keyword names, enum members and printing")
def _():
    T.eq(Kind.None_.value, 0)
    T.eq(Kind.name_of(1), "class")
    T.eq(Kind.name_of(Kind.name_), "name")
    T.eq(Kind.name_of(7), "")
    T.eq(Kind.is_known(2), True)
    m = Flags.decode(Flags(kind=Kind.class_, loose=2, from_=4).encode())
    T.eq(m.kind is Kind.class_, True)
    T.eq(m.loose is Loose.b, True)
    T.eq(m.from_, 4)
    T.ok("kind: class (0x01)" in str(m), "print uses the schema name")
    T.ok("from: 4" in str(m), "print uses the field name")
    u = Flags.decode(Flags(kind=Kind.class_, loose=9).encode())
    T.eq(type(u.loose), int)  # undeclared values of non-strict enums stay ints
    T.ok("loose: <unknown> (0x09)" in str(u), "unknown enum value")
    v = Flags.view(m.encode())
    T.eq([r.name for r in v.ranges()][:3], ["a", "b", "kind"])
    T.eq(v.ranges()[2], gmsg.FieldRange("kind", "kind", 2, 3))
    T.ok(repr(v).startswith("<FlagsView Flags { a: 0"), "view repr")
    T.eq(api.__all__[:3], ["Kind", "Loose", "Flags"])


@T.test("text messages: decode_with_extra and limits of strings")
def _():
    m, extra = Rec.decode_with_extra("5,ab,c,d")
    T.eq((m.x, m.label, extra), (5, "ab", 2))
    m, extra = Rec.decode_with_extra(b"-3,xy")
    T.eq((m.x, m.label, extra), (-3, "xy", 0))
    T.eq(Rec.view("1,a,b").extra_tokens, 1)
    T.eq(Rec(x=-7, label="hi").encode_text(), "-7,hi")
    T.eq(code_of(lambda: Rec(label="123456789").encode()), Err.OUT_OF_RANGE)
    T.eq(code_of(lambda: Rec.decode("1,123456789")), Err.OUT_OF_RANGE)


@T.test("protocol: header values from a dict or an instance, parse / decode / stream")
def _():
    msg = Flags(a=1, kind=Kind.class_, either=3)
    wire = Link.encode(msg, {"seq": 9})
    T.eq(wire[0], 0xA5)
    v = Link.parse(wire)
    T.eq(type(v).__name__, "FlagsView")
    T.eq(Link.decode(wire), msg)
    f = Link.open(wire)
    T.eq((f.header.id, f.header.seq), (1, 9))
    T.eq(Link.encode(msg, Link.Header(seq=4))[2], 4)
    T.eq(Link.encode(msg)[2], 0)
    T.eq(code_of(lambda: Link.encode(Rec())), Err.INVALID_VALUE)
    s = Link.stream()
    got = []
    s.on(Flags, lambda view, header: got.append((header.seq, view.a)))
    T.eq(s.feed(wire + b"\x00" + Link.encode(msg, {"seq": 2})), 2)
    T.eq(got, [(9, 1), (2, 1)])
    T.eq(s.stats.bytes_skipped, 1)
    T.eq([m.either for m in Link.stream().messages(wire * 3)], [3, 3, 3])
    T.eq(Link.stream().poll_message(), None)


@T.test("accelerator switch")
def _():
    native = gmsg.NATIVE
    T.eq(gmsg.use_native(False), False)
    T.eq(gmsg.NATIVE, False)
    T.eq(Flags.decode(frame(2, 7)).both, 0x1111)
    gmsg.use_native(native)
    T.eq(gmsg.NATIVE, native)


sys.exit(T.run())
