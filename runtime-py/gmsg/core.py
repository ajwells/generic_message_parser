"""Bytes, integer types, Reader / Writer and bit fields (gmp/endian.hpp, byte_reader.hpp, byte_writer.hpp, bits.hpp).

Byte orders are the strings 'big' and 'little' (as int.from_bytes takes them); bit orders 'msb_first' / 'lsb_first'.
Integers are Python ints of any width, checked against their wire type when written. A char is a one-character
string (one byte, latin-1); strings hold one character per byte, so any byte sequence round-trips.
"""

from __future__ import annotations

import math
import struct
from typing import Any, Callable

from .errors import Err, fail

BIG = "big"
LITTLE = "little"
MSB_FIRST = "msb_first"
LSB_FIRST = "lsb_first"

BytesLike = bytes | bytearray | memoryview

#: bytes on the wire per primitive type
SIZE = {"u8": 1, "u16": 2, "u32": 4, "u64": 8, "i8": 1, "i16": 2, "i32": 4, "i64": 8,
        "f32": 4, "f64": 8, "bool": 1, "char": 1}
#: (bits, signed) per integer type
INT_TYPES = {"u8": (8, False), "u16": (16, False), "u32": (32, False), "u64": (64, False),
             "i8": (8, True), "i16": (16, True), "i32": (32, True), "i64": (64, True)}
_FMT = {"u8": "B", "i8": "b", "u16": "H", "i16": "h", "u32": "I", "i32": "i", "u64": "Q", "i64": "q",
        "f32": "f", "f64": "d"}
_STRUCTS = {(t, e): struct.Struct(("<" if e == LITTLE else ">") + f) for t, f in _FMT.items() for e in (BIG, LITTLE)}


def to_bytes(b: Any) -> bytes | bytearray | memoryview:
    """Accept bytes, bytearray, memoryview, a str (one character per byte) or a list of byte values."""
    if isinstance(b, (bytes, bytearray)):
        return b
    if isinstance(b, memoryview):
        return b if b.format == "B" and b.ndim == 1 else b.cast("B")
    if isinstance(b, str):
        return latin1_bytes(b)
    if isinstance(b, (list, tuple)):
        return bytes(b)
    fail(Err.INVALID_VALUE, "expected bytes")


def latin1_bytes(s: str) -> bytes:
    """A string with one character per byte -> bytes; characters above U+00FF are InvalidValue."""
    try:
        return s.encode("latin-1")
    except UnicodeEncodeError as e:
        fail(Err.INVALID_VALUE, f"character U+{ord(s[e.start]):04X} is not a byte")


def latin1_string(b: BytesLike, start: int = 0, end: int | None = None) -> str:
    return bytes(b[start:end]).decode("latin-1")


# ---- integers ---------------------------------------------------------------------------------------------------
def int_range(t: str) -> tuple[int, int]:
    bits, signed = INT_TYPES[t]
    return (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)


def check_int(v: Any, t: str, what: str | None = None) -> int:
    """`v` when it is an integer that fits type t; otherwise GmpError(OutOfRange) (what C++ types guarantee)."""
    if isinstance(v, bool) or not isinstance(v, int):
        if isinstance(v, float) and v.is_integer():
            v = int(v)
        else:
            fail(Err.OUT_OF_RANGE, f"{what or 'value'} must be an integer ({t})")
    lo, hi = int_range(t)
    if v < lo or v > hi:
        fail(Err.OUT_OF_RANGE, f"{what or 'value'} {v} does not fit {t}")
    return int(v)


def wrap_int(v: Any, t: str) -> int:
    """Convert to integer type t as C++ static_cast does (floats truncate toward zero, then wrap)."""
    bits, signed = INT_TYPES[t]
    if isinstance(v, float):
        v = math.trunc(v) if math.isfinite(v) else 0
    v = int(v) & ((1 << bits) - 1)
    return v - (1 << bits) if signed and v >> (bits - 1) else v


def to_int(v: Any) -> int:
    """A value as gmsg::to_int sees it in generated expressions (int64): bool -> 0 / 1, char -> its signed byte,
    floats truncated toward zero."""
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, int):
        return wrap_int(v, "i64")
    if isinstance(v, str):
        c = ord(v[0]) & 0xFF if v else 0
        return c - 256 if c > 127 else c
    if isinstance(v, float):
        return wrap_int(math.trunc(v), "i64") if math.isfinite(v) else 0
    return 0


def llround(x: float) -> int:
    """Round half away from zero (std::llround); non-finite values give 0."""
    if not math.isfinite(x):
        return 0
    a = abs(x)
    f = math.floor(a)
    r = f + 1 if a - f >= 0.5 else f  # exact: a - f is computed without rounding for doubles
    return -int(r) if x < 0 else int(r)


def idiv(a: int, b: int) -> int:
    """Integer division of generated expressions (int64, truncating toward zero)."""
    if b == 0:
        fail(Err.INVALID_VALUE, "division by zero")
    q = abs(a) // abs(b)
    return wrap_int(q if (a < 0) == (b < 0) else -q, "i64")


def imod(a: int, b: int) -> int:
    """Remainder with the sign of the dividend (C++ %)."""
    if b == 0:
        fail(Err.INVALID_VALUE, "division by zero")
    return wrap_int(a - b * idiv(a, b), "i64")


def strip_padding(s: str, pad: int) -> str:
    """Trailing pad characters removed (gmsg::strip_padding)."""
    return s.rstrip(chr(pad))


# ---- floats -----------------------------------------------------------------------------------------------------
_F32_OVER = (1 << 128) - (1 << 103)  # values at or above this magnitude round to infinity as float


def to_f32(x: float) -> float:
    """Round a double to the nearest float (C++ static_cast<float>): values beyond the float range become inf."""
    x = float(x)
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        return math.copysign(math.inf if abs(x) >= _F32_OVER else 3.4028234663852886e38, x)


def _pack(t: str, v: Any, e: str) -> bytes:
    if t == "f32":
        v = to_f32(v)
    return _STRUCTS[(t, e)].pack(v)


# ---- loads and stores -------------------------------------------------------------------------------------------
def load(p: BytesLike, at: int, t: str, endian: str = BIG) -> Any:
    """The value of primitive type t at p[at] (gmsg::load): int, float, bool, or a one-character str for char."""
    if at < 0 or len(p) - at < SIZE[t]:
        fail(Err.TRUNCATED)
    if t == "bool":
        return p[at] != 0
    if t == "char":
        return chr(p[at])
    return _STRUCTS[(t, endian)].unpack_from(p, at)[0]


def load_uint(p: BytesLike, at: int, n: int, endian: str = BIG) -> int:
    """Unsigned integer of n bytes."""
    return int.from_bytes(p[at:at + n], endian)


def store_uint(p: bytearray, at: int, n: int, v: int, endian: str = BIG) -> None:
    """Store the low n bytes of v."""
    p[at:at + n] = (int(v) & ((1 << (8 * n)) - 1)).to_bytes(n, endian)


class Reader:
    """Sequential reader over bytes[start, end). Reading past the end raises GmpError(Truncated)."""

    __slots__ = ("bytes", "pos", "end")

    def __init__(self, data: Any, start: int = 0, end: int | None = None) -> None:
        self.bytes = to_bytes(data)
        self.pos = start
        self.end = len(self.bytes) if end is None else end

    @property
    def remaining(self) -> int:
        return self.end - self.pos

    def need(self, n: int) -> None:
        if self.end - self.pos < n:
            fail(Err.TRUNCATED)

    def skip(self, n: int) -> None:
        self.need(n)
        self.pos += n

    def read(self, t: str, endian: str = BIG) -> Any:
        """A value of primitive type t ('u16', 'f32', 'bool', 'char', ...)."""
        n = SIZE[t]
        self.need(n)
        at = self.pos
        self.pos += n
        if t == "bool":
            return self.bytes[at] != 0
        if t == "char":
            return chr(self.bytes[at])
        return _STRUCTS[(t, endian)].unpack_from(self.bytes, at)[0]

    def read_struct(self, s: struct.Struct) -> tuple:
        """Unpack a precompiled struct.Struct (fixed layouts of generated code)."""
        self.need(s.size)
        v = s.unpack_from(self.bytes, self.pos)
        self.pos += s.size
        return v

    def take(self, n: int) -> bytes:
        """n bytes (a copy)."""
        self.need(n)
        b = bytes(self.bytes[self.pos:self.pos + n])
        self.pos += n
        return b

    def view(self, n: int) -> memoryview:
        """n bytes without copying."""
        self.need(n)
        b = memoryview(self.bytes)[self.pos:self.pos + n]
        self.pos += n
        return b

    def read_until(self, terminator: int) -> bytes:
        """The bytes before the next `terminator` (a copy); the terminator is consumed. Truncated when there is none."""
        i = bytes(self.bytes[self.pos:self.end]).find(terminator)
        if i < 0:
            fail(Err.TRUNCATED)
        b = bytes(self.bytes[self.pos:self.pos + i])
        self.pos += i + 1
        return b


class Writer:
    """Growable writer. Integers are range-checked against their type (GmpError OutOfRange)."""

    __slots__ = ("buf",)

    def __init__(self) -> None:
        self.buf = bytearray()

    @property
    def pos(self) -> int:
        return len(self.buf)

    def write(self, t: str, v: Any, endian: str = BIG) -> None:
        """A value of primitive type t."""
        if t == "bool":
            self.buf.append(1 if v else 0)
        elif t == "char":
            self.buf.append(char_code(v))
        elif t in INT_TYPES:
            self.buf += _STRUCTS[(t, endian)].pack(check_int(v, t))
        else:
            self.buf += _pack(t, v, endian)

    def write_struct(self, s: struct.Struct, *values: Any) -> None:
        self.buf += s.pack(*values)

    def put(self, data: Any) -> None:
        self.buf += to_bytes(data)

    def zeros(self, n: int) -> None:
        self.buf += bytes(n)

    def fill(self, value: int, n: int) -> None:
        self.buf += bytes((value,)) * n

    def uint(self, v: int, n: int, endian: str = BIG) -> None:
        """The low n bytes of an unsigned integer."""
        self.buf += (int(v) & ((1 << (8 * n)) - 1)).to_bytes(n, endian)

    def patch_uint(self, at: int, v: int, n: int, endian: str = BIG) -> None:
        """Overwrite n bytes at absolute offset `at` (back-patching a length field)."""
        store_uint(self.buf, at, n, v, endian)

    def to_bytes(self) -> bytes:
        return bytes(self.buf)


def char_code(v: Any) -> int:
    """The byte of a char value: a one-character string (U+0000..U+00FF) or a byte value."""
    if isinstance(v, int) and not isinstance(v, bool):
        return check_int(v, "u8", "char")
    if not isinstance(v, str) or len(v) != 1 or ord(v) > 255:
        fail(Err.OUT_OF_RANGE, "char must be one character")
    return ord(v)


def check_char(v: Any) -> str:
    """A char value as a one-character string."""
    return chr(char_code(v))


# ---- bit fields -------------------------------------------------------------------------------------------------
def extract_bits(p: BytesLike, off: int, n: int, order: str = MSB_FIRST) -> int:
    """n bits starting at bit `off` (gmsg::extract_bits)."""
    if order == LSB_FIRST:
        first, last = off >> 3, (off + n - 1) >> 3
        word = int.from_bytes(p[first:last + 1], "little")
        return (word >> (off & 7)) & ((1 << n) - 1)
    first, last = off >> 3, (off + n - 1) >> 3
    word = int.from_bytes(p[first:last + 1], "big")
    total = (last - first + 1) * 8
    return (word >> (total - (off & 7) - n)) & ((1 << n) - 1)


def insert_bits(p: bytearray, off: int, n: int, value: int, order: str = MSB_FIRST) -> None:
    """Write the low n bits of value at bit `off`."""
    v = int(value) & ((1 << n) - 1)
    first, last = off >> 3, (off + n - 1) >> 3
    size = last - first + 1
    if order == LSB_FIRST:
        word = int.from_bytes(p[first:last + 1], "little")
        shift = off & 7
        word = (word & ~(((1 << n) - 1) << shift)) | (v << shift)
        p[first:last + 1] = word.to_bytes(size, "little")
    else:
        word = int.from_bytes(p[first:last + 1], "big")
        shift = size * 8 - (off & 7) - n
        word = (word & ~(((1 << n) - 1) << shift)) | (v << shift)
        p[first:last + 1] = word.to_bytes(size, "big")


def sign_extend(v: int, n: int) -> int:
    return v - (1 << n) if v >> (n - 1) & 1 else v


def get_bits(p: BytesLike, off: int, n: int, order: str, t: str) -> Any:
    """A bit field converted to type t: an integer type, 'bool', or an enum's base type."""
    raw = extract_bits(p, off, n, order)
    if t == "bool":
        return raw != 0
    return sign_extend(raw, n) if INT_TYPES.get(t, (0, False))[1] else raw


def set_bits(p: bytearray, off: int, n: int, order: str, value: Any) -> None:
    """Write a value into a bit field (truncated to n bits, as gmsg::set_bits)."""
    insert_bits(p, off, n, (1 if value else 0) if isinstance(value, bool) else int(value), order)


def attempt(fn: Callable[[], Any]) -> tuple[int, Any]:
    """Run fn: (Err.NONE, result), or (code, None) when it raises a GmpError."""
    from .errors import GmpError

    try:
        return Err.NONE, fn()
    except GmpError as e:
        return e.code, None
