"""Python versions of the user extensions in tests/roundtrip/test_codecs.hpp, referenced from the `python:` blocks of
tests/schemas/hdlc.yaml and custom.yaml (module test_codecs, imported by the generated modules)."""

from __future__ import annotations

import math
from dataclasses import dataclass

import gmsg
from gmsg import Err


@dataclass
class Time:
    h: int = 0
    m: int = 0
    s: int = 0


def _from_bcd(b: int) -> int:
    if (b >> 4) > 9 or (b & 0xF) > 9:
        gmsg.fail(Err.INVALID_VALUE)
    return (b >> 4) * 10 + (b & 0xF)


def decode_bcd_time(r: gmsg.Reader) -> Time:
    """Three BCD bytes: HH MM SS."""
    return Time(_from_bcd(r.read("u8")), _from_bcd(r.read("u8")), _from_bcd(r.read("u8")))


def encode_bcd_time(w: gmsg.Writer, t: Time) -> None:
    for v in (t.h, t.m, t.s):
        if v > 99:
            gmsg.fail(Err.OUT_OF_RANGE)
        w.write("u8", ((v // 10) << 4) | (v % 10))


def print_time(p: gmsg.Printer, t: Time) -> None:
    """Print hook: HH:MM:SS."""
    p.raw_text(f"{t.h % 100:02d}:{t.m % 100:02d}:{t.s % 100:02d}")


def add16(d) -> int:
    """Custom checksum: 16-bit sum of all bytes."""
    return sum(d) & 0xFFFF


def decode_coord(s: str) -> float:
    """NMEA "ddmm.mmmm" -> decimal degrees."""
    v = gmsg.text.parse_float(s)
    deg = float(math.floor(v / 100.0))
    return deg + (v - deg * 100.0) / 60.0


def encode_coord(w: gmsg.text.TextWriter, degrees: float) -> None:
    deg = float(math.floor(degrees))
    ddmm = deg * 100.0 + (degrees - deg) * 60.0
    w.raw(gmsg.format_float(ddmm, 4))


def len_byte_scan(b) -> gmsg.FrameScan:
    """Frame = [n][n bytes]; a zero length byte is treated as garbage."""
    if len(b) == 0:
        return gmsg.FrameScan.need_more(1)
    if b[0] == 0:
        return gmsg.FrameScan.skip(1, Err.BAD_LENGTH)
    n = 1 + b[0]
    if len(b) < n:
        return gmsg.FrameScan.need_more(n)
    return gmsg.FrameScan.frame(n, n, n)
