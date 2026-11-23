"""Text tokens (gmp/text.hpp): splitting, strict number parsing as std::from_chars, and the token writer."""

from __future__ import annotations

from typing import Any

from . import _accel
from .core import INT_TYPES, int_range
from .errors import Err, fail
from .floattext import format_float, parse_float

__all__ = ["split", "parse_int", "parse_float", "parse_char", "parse_hex_byte", "hex_value", "hex_text", "TextWriter"]

_DIGITS = "0123456789abcdefghijklmnopqrstuvwxyz"


def split(s: str, sep: str) -> list[str]:
    """Split on sep, keeping empty tokens ("a,,b" -> ["a", "", "b"])."""
    return s.split(sep)


def parse_int(s: str, t: str, base: int = 10) -> int:
    """Parse an integer of type t as std::from_chars (after one leading '+'): MissingField (empty), ParseError,
    OutOfRange."""
    nat = _accel.native
    if nat is not None:
        lo, hi = int_range(t)
        r = nat.parse_int(s, base, INT_TYPES[t][1], lo, hi)
        if isinstance(r, tuple):
            fail(r[0])
        return r
    if not s:
        fail(Err.MISSING_FIELD)
    if s[0] == "+":
        s = s[1:]
    bits, signed = INT_TYPES[t]
    i, neg = 0, False
    if signed and s[:1] == "-":
        neg, i = True, 1
    v, any_digit = 0, False
    while i < len(s):
        c = ord(s[i])
        d = c - 48 if 48 <= c <= 57 else c - 87 if 97 <= c <= 122 else c - 55 if 65 <= c <= 90 else 99
        if d >= base:
            break
        any_digit = True
        v = v * base + d
        i += 1
    if not any_digit:
        fail(Err.PARSE_ERROR)
    if neg:
        v = -v
    lo, hi = int_range(t)
    if v < lo or v > hi:
        fail(Err.OUT_OF_RANGE)
    if i != len(s):
        fail(Err.PARSE_ERROR)
    return v


def parse_char(s: str) -> str:
    if not s:
        fail(Err.MISSING_FIELD)
    if len(s) != 1:
        fail(Err.PARSE_ERROR)
    return s


def parse_hex_byte(s: str) -> int:
    if len(s) != 2:
        fail(Err.PARSE_ERROR)
    return parse_int(s, "u8", 16)


def hex_value(s: str, n: int) -> int | None:
    """n bytes written as 2n hex digits (a text checksum); None when malformed."""
    if len(s) != 2 * n:
        return None
    v = 0
    for i in range(n):
        try:
            v = (v << 8) | parse_hex_byte(s[2 * i:2 * i + 2])
        except Exception:  # GmpError
            return None
    return v


def hex_text(v: int, n: int) -> str:
    """The low n bytes of v as 2n uppercase hex digits."""
    return format(int(v) & ((1 << (8 * n)) - 1), f"0{2 * n}X")


def _to_base(v: int, base: int) -> str:
    if base == 10:
        return str(v)
    neg, v = v < 0, abs(v)
    out = []
    while True:
        v, d = divmod(v, base)
        out.append(_DIGITS[d])
        if not v:
            break
    return ("-" if neg else "") + "".join(reversed(out))


class TextWriter:
    """Builds a text message, inserting separators between tokens (gmp::text::TextWriter)."""

    def __init__(self, sep: str) -> None:
        self.sep = sep
        self._out: list[str] = []
        self._first = True

    def begin_token(self) -> None:
        if not self._first:
            self._out.append(self.sep)
        self._first = False

    def raw(self, s: str) -> None:
        self._out.append(s)

    def empty_token(self) -> None:
        self.begin_token()

    def token(self, s: str) -> None:
        self.begin_token()
        self._out.append(s)

    def int_token(self, v: Any, base: int = 10, width: int = 0, upper: bool = True) -> None:
        """Integer token in `base`, zero-padded to `width` digits."""
        self.begin_token()
        d = _to_base(int(v), base)
        if d[0] == "-":
            self._out.append("-")
            d = d[1:]
        if len(d) < width:
            self._out.append("0" * (width - len(d)))
        self._out.append(d.upper() if upper and base > 10 else d)

    def float_token(self, v: float, precision: int = -1, width: int = 0, is_f32: bool = False) -> None:
        """Float token: precision >= 0 gives fixed notation; width zero-pads after the sign (NMEA "0123.45")."""
        self.begin_token()
        s = format_float(v, precision, is_f32)
        neg = s[0] == "-"
        if neg:
            self._out.append("-")
            s = s[1:]
        n = len(s) + (1 if neg else 0)
        if n < width:
            self._out.append("0" * (width - n))
        self._out.append(s)

    def text(self) -> str:
        return "".join(self._out)

    __str__ = text
