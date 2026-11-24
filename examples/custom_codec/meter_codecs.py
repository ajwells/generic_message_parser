"""User-supplied Python codecs referenced from the `python:` blocks of meter.yaml (the Python twin of meter_codecs.hpp).
Imported by the generated meter_gmp.py (module meter_codecs, next to it or on sys.path)."""

from __future__ import annotations

from dataclasses import dataclass

import gmsg
from gmsg import Err


@dataclass
class DateTime:
    year: int = 2000
    month: int = 1
    day: int = 1
    hour: int = 0
    minute: int = 0
    second: int = 0


def _bcd(b: int) -> int:
    return (b >> 4) * 10 + (b & 0x0F)


def _to_bcd(v: int) -> int:
    return ((v // 10) << 4) | (v % 10)


def decode_bcd(r: gmsg.Reader) -> DateTime:
    """YY MM DD hh mm ss, BCD encoded."""
    d = [r.read("u8") for _ in range(6)]
    t = DateTime(2000 + _bcd(d[0]), _bcd(d[1]), _bcd(d[2]), _bcd(d[3]), _bcd(d[4]), _bcd(d[5]))
    if not 1 <= t.month <= 12 or not 1 <= t.day <= 31:
        gmsg.fail(Err.INVALID_VALUE)
    return t


def encode_bcd(w: gmsg.Writer, t: DateTime) -> None:
    for v in (t.year - 2000, t.month, t.day, t.hour, t.minute, t.second):
        w.write("u8", _to_bcd(v))


def print_datetime(p: gmsg.Printer, t: DateTime) -> None:
    """Print hook: 2026-11-20 14:30:05. Without it the field prints as <?>."""
    p.raw_text(f"{t.year:04d}-{t.month:02d}-{t.day:02d} {t.hour:02d}:{t.minute:02d}:{t.second:02d}")


def mod256(d) -> int:
    """Custom checksum: modulo-256 sum."""
    return sum(d) & 0xFF
