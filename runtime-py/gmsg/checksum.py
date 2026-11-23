"""Checksums and CRCs (gmp/checksum.hpp). Every algorithm has: name, size (bytes), width (bits), init(),
update(state, data, start=0, end=None), finish(state) and compute(data, start=0, end=None). States are ints in the
same representation as the C runtime (a reflected register for reflected CRCs), so the accelerator can take over
any step.
"""

from __future__ import annotations

import functools
import operator
from typing import Callable

from . import _accel
from .core import BytesLike


def _reflect(v: int, w: int) -> int:
    r = 0
    for _ in range(w):
        r = (r << 1) | (v & 1)
        v >>= 1
    return r


class Checksum:
    """Interface shared by all algorithms."""

    name: str = ""
    size: int = 0
    width: int = 0

    def init(self) -> int:
        return 0

    def update(self, state: int, data: BytesLike, start: int = 0, end: int | None = None) -> int:  # pragma: no cover
        raise NotImplementedError

    def finish(self, state: int) -> int:
        return state

    def compute(self, data: BytesLike, start: int = 0, end: int | None = None) -> int:
        return self.finish(self.update(self.init(), data, start, end))

    def __repr__(self) -> str:
        return f"<gmsg checksum {self.name or self.__class__.__name__}>"


class Crc(Checksum):
    """Generic CRC in the Rocksoft model (gmp::checksum::Crc): width 1..64, any parameters."""

    def __init__(self, width: int, poly: int, init: int = 0, refin: bool = False, refout: bool = False,
                 xorout: int = 0, name: str = "") -> None:
        if not 1 <= width <= 64:
            raise ValueError("CRC width must be 1..64")
        self.name, self.width, self.size = name, width, (width + 7) // 8
        self.poly, self.init_value, self.refin, self.refout, self.xorout = poly, init, refin, refout, xorout
        self.mask = (1 << width) - 1
        self._rpoly = _reflect(poly, width)
        self._table: list[int] | None = None
        if width >= 8:
            t = []
            for i in range(256):
                if refin:
                    c = i
                    for _ in range(8):
                        c = (c >> 1) ^ self._rpoly if c & 1 else c >> 1
                else:
                    c = i << (width - 8)
                    top = 1 << (width - 1)
                    for _ in range(8):
                        c = ((c << 1) ^ poly) & self.mask if c & top else (c << 1) & self.mask
                t.append(c)
            self._table = t

    def init(self) -> int:
        return _reflect(self.init_value, self.width) if self.refin else self.init_value

    def update(self, state: int, data: BytesLike, start: int = 0, end: int | None = None) -> int:
        n = _accel.native
        if n is not None:
            return n.crc_update(self.width, self.poly, self.refin, state, data, start, -1 if end is None else end)
        d = data[start:end]
        c = state
        t, w, mask = self._table, self.width, self.mask
        if t is not None:
            if self.refin:
                for b in d:
                    c = t[(c ^ b) & 0xFF] ^ (c >> 8)
            else:
                s = w - 8
                for b in d:
                    c = (t[((c >> s) ^ b) & 0xFF] ^ (c << 8)) & mask
            return c
        for b in d:  # bitwise for widths below 8
            if self.refin:
                c ^= b
                for _ in range(8):
                    c = (c >> 1) ^ self._rpoly if c & 1 else c >> 1
                c &= mask
            else:
                for k in range(7, -1, -1):
                    bit = ((c >> (w - 1)) & 1) ^ ((b >> k) & 1)
                    c = (c << 1) & mask
                    if bit:
                        c ^= self.poly
        return c

    def finish(self, state: int) -> int:
        if self.refin != self.refout:
            state = _reflect(state, self.width)
        return (state ^ self.xorout) & self.mask


class _Simple(Checksum):
    def __init__(self, name: str, size: int, kind: int, step: Callable[[int, BytesLike], int],
                 finish: Callable[[int], int] = lambda s: s) -> None:
        self.name, self.size, self.width, self._kind, self._step, self._finish = name, size, size * 8, kind, step, finish

    def update(self, state: int, data: BytesLike, start: int = 0, end: int | None = None) -> int:
        n = _accel.native
        if n is not None:
            return n.simple_update(self._kind, state, data, start, -1 if end is None else end)
        return self._step(state, data[start:end])

    def finish(self, state: int) -> int:
        return self._finish(state)


def _fletcher16(s: int, d: BytesLike) -> int:
    s1, s2 = s & 0xFF, (s >> 8) & 0xFF
    for b in d:
        s1 = (s1 + b) % 255
        s2 = (s2 + s1) % 255
    return (s2 << 8) | s1


def _fletcher8_ubx(s: int, d: BytesLike) -> int:
    a, b = (s >> 8) & 0xFF, s & 0xFF
    for x in d:
        a = (a + x) & 0xFF
        b = (b + a) & 0xFF
    return (a << 8) | b


crc8 = Crc(8, 0x07, name="crc8")
crc8_maxim = Crc(8, 0x31, refin=True, refout=True, name="crc8_maxim")
crc8_autosar = Crc(8, 0x2F, init=0xFF, xorout=0xFF, name="crc8_autosar")
crc8_nrsc5 = Crc(8, 0x31, init=0xFF, name="crc8_nrsc5")
crc15_can = Crc(15, 0x4599, name="crc15_can")
crc16_ccitt = Crc(16, 0x1021, init=0xFFFF, name="crc16_ccitt")
crc16_xmodem = Crc(16, 0x1021, name="crc16_xmodem")
crc16_kermit = Crc(16, 0x1021, refin=True, refout=True, name="crc16_kermit")
crc16_x25 = Crc(16, 0x1021, init=0xFFFF, refin=True, refout=True, xorout=0xFFFF, name="crc16_x25")
crc16_modbus = Crc(16, 0x8005, init=0xFFFF, refin=True, refout=True, name="crc16_modbus")
crc16_arc = Crc(16, 0x8005, refin=True, refout=True, name="crc16_arc")
crc24_openpgp = Crc(24, 0x864CFB, init=0xB704CE, name="crc24_openpgp")
crc32 = Crc(32, 0x04C11DB7, init=0xFFFFFFFF, refin=True, refout=True, xorout=0xFFFFFFFF, name="crc32")
crc32c = Crc(32, 0x1EDC6F41, init=0xFFFFFFFF, refin=True, refout=True, xorout=0xFFFFFFFF, name="crc32c")
crc32_mpeg2 = Crc(32, 0x04C11DB7, init=0xFFFFFFFF, name="crc32_mpeg2")
crc64_ecma = Crc(64, 0x42F0E1EBA9EA3693, name="crc64_ecma")
xor8 = _Simple("xor8", 1, 0, lambda s, d: functools.reduce(operator.xor, d, s))
sum8 = _Simple("sum8", 1, 1, lambda s, d: (s + sum(d)) & 0xFF)
lrc8 = _Simple("lrc8", 1, 2, lambda s, d: (s + sum(d)) & 0xFF, lambda s: (-s) & 0xFF)
sum16 = _Simple("sum16", 2, 3, lambda s, d: (s + sum(d)) & 0xFFFF)
fletcher16 = _Simple("fletcher16", 2, 4, _fletcher16)
fletcher8_ubx = _Simple("fletcher8_ubx", 2, 5, _fletcher8_ubx)

#: the built-in algorithms by schema name
PRESETS: dict[str, Checksum] = {a.name: a for a in (
    crc8, crc8_maxim, crc8_autosar, crc8_nrsc5, crc15_can, crc16_ccitt, crc16_xmodem, crc16_kermit, crc16_x25,
    crc16_modbus, crc16_arc, crc24_openpgp, crc32, crc32c, crc32_mpeg2, crc64_ecma, xor8, sum8, lrc8, sum16,
    fletcher16, fletcher8_ubx)}


def crc(width: int, poly: int, init: int = 0, refin: bool = False, refout: bool = False, xorout: int = 0) -> Crc:
    """A CRC with explicit parameters (schema `{ crc: { width, poly, ... } }`)."""
    return Crc(width, poly, init, refin, refout, xorout)
