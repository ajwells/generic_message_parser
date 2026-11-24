"""Byte stuffing and frame scanners (gmp/framing/*.hpp).

A scan returns a FrameScan: kind 'need_more' | 'frame' | 'skip', with size, consume, consume_on_error and error, the
same fields as gmsg::FrameScan. Configs are immutable; the accelerator compiles each one once (cached on the object).
"""

from __future__ import annotations

from typing import Any, Callable, NamedTuple

from . import _accel
from .core import BIG, BytesLike, to_bytes
from .errors import Err, fail

MAX_RESERVED = 16


class EscapeConfig:
    """Reserved bytes and how each is sent as {esc, code} (gmsg::EscapeConfig). Use xor_escape / map_escape /
    index_escape; NO_ESCAPE disables stuffing."""

    __slots__ = ("enabled", "esc", "style", "xor_mask", "raw", "code", "_set", "_n")

    def __init__(self, enabled: bool, esc: int = 0, style: str = "xor", xor_mask: int = 0x20,
                 raw: tuple[int, ...] = (), code: tuple[int, ...] = ()) -> None:
        self.enabled, self.esc, self.style, self.xor_mask = enabled, esc, style, xor_mask
        self.raw, self.code = tuple(raw[:MAX_RESERVED]), tuple(code[:MAX_RESERVED])
        s = bytearray(256)
        if enabled:
            s[esc] = 1
            for b in raw:
                s[b] = 1
        self._set = bytes(s)
        self._n = None

    def needs_escape(self, b: int) -> bool:
        return self._set[b] == 1

    def encode(self, b: int) -> int:
        if self.style == "table" and b in self.raw:
            return self.code[self.raw.index(b)]
        return (b ^ self.xor_mask) & 0xFF

    def decode(self, c: int) -> int:
        """The raw byte for an escape code, or -1 when the code is not valid."""
        if self.style == "table":
            return self.raw[self.code.index(c)] if c in self.code else -1
        return (c ^ self.xor_mask) & 0xFF

    def native_handle(self):  # pragma: no cover - accelerator only
        if self._n is None and _accel.native is not None:
            self._n = _accel.native.escape_config(self.enabled, self.esc, self.style == "table", self.xor_mask,
                                                  bytes(self.raw), bytes(self.code))
        return self._n


NO_ESCAPE = EscapeConfig(False)


def xor_escape(esc: int, xor_mask: int, reserved: list[int] | tuple[int, ...]) -> EscapeConfig:
    """XOR style (HDLC / PPP): each reserved byte (and esc) is sent as {esc, b ^ xor_mask}."""
    return EscapeConfig(True, esc, "xor", xor_mask, tuple(reserved))


def map_escape(esc: int, raw: list[int] | tuple[int, ...], code: list[int] | tuple[int, ...]) -> EscapeConfig:
    """Table style: raw[i] is sent as {esc, code[i]}. `raw` should include esc."""
    return EscapeConfig(True, esc, "table", 0x20, tuple(raw), tuple(code))


def index_escape(esc: int, reserved: list[int] | tuple[int, ...], base: int = 0) -> EscapeConfig:
    """Index style: reserved[i] is sent as {esc, base + i}. `reserved` should include esc."""
    return EscapeConfig(True, esc, "table", 0x20, tuple(reserved), tuple((base + i) & 0xFF for i in range(len(reserved))))


class EscapeWalk(NamedTuple):
    kind: str  # 'done' | 'need_more' | 'bad'
    wire: int
    error: Err


def walk_escaped(buf: BytesLike, at: int, n: int, cfg: EscapeConfig, pick_at: int = 0,
                 pick: bytearray | None = None) -> EscapeWalk:
    """Walk escaped bytes from `at` until n unescaped bytes are produced (gmsg::walk_escaped). Unescaped bytes with
    index in [pick_at, pick_at + len(pick)) are copied into `pick`."""
    nat = _accel.native
    if nat is not None:
        k, wire, err = nat.walk_escaped(cfg.native_handle(), buf, at, n, pick_at, pick)
        return EscapeWalk(("done", "need_more", "bad")[k], wire, Err(err))
    i, size, esc = at, len(buf), cfg.esc
    for u in range(n):
        if i >= size:
            return EscapeWalk("need_more", i, Err.NONE)
        b = buf[i]
        if b == esc:
            if i + 1 >= size:
                return EscapeWalk("need_more", i, Err.NONE)
            d = cfg.decode(buf[i + 1])
            if d < 0:
                return EscapeWalk("bad", i, Err.INVALID_VALUE)
            b = d
            i += 2
        elif cfg._set[b]:
            return EscapeWalk("bad", i, Err.BAD_DELIMITER)
        else:
            i += 1
        if pick is not None and pick_at <= u < pick_at + len(pick):
            pick[u - pick_at] = b
    return EscapeWalk("done", i, Err.NONE)


def read_frame_header(buf: BytesLike, prefix: int, escape: EscapeConfig | None, n: int) -> bytes | None:
    """The n header bytes after the sync pattern (`prefix` bytes) at the start of buf, unescaped when `escape` is
    enabled, or None while they have not all arrived / the escaping is invalid (gmsg::read_frame_header)."""
    if escape is None or not escape.enabled:
        return None if len(buf) < prefix + n else bytes(buf[prefix:prefix + n])
    out = bytearray(n)
    return bytes(out) if walk_escaped(buf, prefix, n, escape, 0, out).kind == "done" else None


def unescape(wire: BytesLike, cfg: EscapeConfig) -> bytes:
    """The unescaped bytes (gmsg::unescape_in_place on a copy). Truncated / InvalidValue on a bad escape pair."""
    nat = _accel.native
    if nat is not None:
        r = nat.unescape(cfg.native_handle(), wire)
        if isinstance(r, int):
            fail(r)
        return r
    out = bytearray()
    esc, i, n = cfg.esc, 0, len(wire)
    while i < n:
        b = wire[i]
        if b == esc:
            i += 1
            if i >= n:
                fail(Err.TRUNCATED)
            d = cfg.decode(wire[i])
            if d < 0:
                fail(Err.INVALID_VALUE)
            b = d
        out.append(b)
        i += 1
    return bytes(out)


def unescaped_size(wire: BytesLike, cfg: EscapeConfig) -> int:
    """Number of bytes produced by unescaping `wire` (Truncated when it ends inside an escape pair)."""
    n = i = 0
    size = len(wire)
    while i < size:
        if wire[i] == cfg.esc:
            i += 1
            if i >= size:
                fail(Err.TRUNCATED)
        i += 1
        n += 1
    return n


def check_escaped(wire: BytesLike, cfg: EscapeConfig) -> int:
    """Check the escape pairs of `wire` without changing it (the errors of unescape, in the same order); returns the
    unescaped size."""
    n = i = 0
    size = len(wire)
    while i < size:
        if wire[i] == cfg.esc:
            i += 1
            if i >= size:
                fail(Err.TRUNCATED)
            if cfg.decode(wire[i]) < 0:
                fail(Err.INVALID_VALUE)
        i += 1
        n += 1
    return n


def escape_overhead(raw: BytesLike, cfg: EscapeConfig, start: int = 0, end: int | None = None) -> int:
    """Number of escape pairs needed for raw[start:end]."""
    s = cfg._set
    return sum(1 for b in raw[start:end] if s[b])


def escape_range(data: BytesLike, at: int, length: int, cfg: EscapeConfig) -> bytes:
    """A copy of `data` with data[at:at + length] escaped."""
    nat = _accel.native
    if nat is not None:
        return nat.escape_range(cfg.native_handle(), data, at, length)
    out = bytearray(data[:at])
    s, esc = cfg._set, cfg.esc
    for b in data[at:at + length]:
        if s[b]:
            out.append(esc)
            out.append(cfg.encode(b))
        else:
            out.append(b)
    out += data[at + length:]
    return bytes(out)


def escape(raw: BytesLike, cfg: EscapeConfig) -> bytes:
    raw = to_bytes(raw)
    return escape_range(raw, 0, len(raw), cfg)


# ================================================================================================================
# scanners
# ================================================================================================================
class FrameScan(NamedTuple):
    """The result of a scan (gmsg::FrameScan)."""

    kind: str  # 'need_more' | 'frame' | 'skip'
    size: int = 0
    consume: int = 0
    consume_on_error: int = 0
    error: Err = Err.NONE

    @staticmethod
    def need_more(hint: int = 0) -> FrameScan:
        return FrameScan("need_more", hint)

    @staticmethod
    def frame(n: int, consume: int, consume_on_error: int) -> FrameScan:
        return FrameScan("frame", n, consume, consume_on_error)

    @staticmethod
    def skip(n: int, error: int = Err.NONE) -> FrameScan:
        return FrameScan("skip", n, 0, 0, Err(error))


_KINDS = ("need_more", "frame", "skip")


def _from_native(t: tuple) -> FrameScan:
    return FrameScan(_KINDS[t[0]], t[1], t[2], t[3], Err(t[4]))


def find_pattern_at_start(buf: BytesLike, pat: bytes) -> FrameScan | None:
    """None when `pat` is at the start of buf, otherwise the scan to return (skip garbage / need more)."""
    if not pat:
        return None
    b = bytes(buf)
    n = len(pat)
    i = b.find(pat[:1])
    while i >= 0:
        avail = min(n, len(b) - i)
        if b[i:i + avail] == pat[:avail]:
            if i > 0:
                return FrameScan.skip(i, Err.BAD_SYNC)
            if avail < n:
                return FrameScan.need_more(n)
            return None
        i = b.find(pat[:1], i + 1)
    return FrameScan.need_more(n) if not b else FrameScan.skip(len(b), Err.BAD_SYNC)


def matches_at(buf: BytesLike, at: int, pat: bytes) -> bool:
    return at + len(pat) <= len(buf) and bytes(buf[at:at + len(pat)]) == pat


class FixedConfig:
    """Fixed-size frames (gmsg::FixedFrameConfig)."""

    __slots__ = ("sync", "frame_size", "escape", "suffix_len", "_n")

    def __init__(self, sync: bytes = b"", frame_size: int = 0, escape: EscapeConfig = NO_ESCAPE, suffix_len: int = 0):
        self.sync, self.frame_size, self.escape, self.suffix_len, self._n = bytes(sync), frame_size, escape, suffix_len, None


class LengthPrefixedConfig:
    """Length-prefixed frames (gmsg::LengthPrefixedConfig): total frame size = length value + add."""

    __slots__ = ("sync", "length_offset", "length_size", "length_endian", "add", "min_frame", "max_frame", "escape",
                 "suffix_len", "counts_wire", "header_len", "trailer_len", "wire_includes_trailer", "_n")

    def __init__(self, sync: bytes = b"", length_offset: int = 0, length_size: int = 1, length_endian: str = BIG,
                 add: int = 0, min_frame: int = 0, max_frame: int = 0, escape: EscapeConfig = NO_ESCAPE,
                 suffix_len: int = 0, counts_wire: bool = False, header_len: int = 0, trailer_len: int = 0,
                 wire_includes_trailer: bool = False) -> None:
        self.sync, self.length_offset, self.length_size, self.length_endian = bytes(sync), length_offset, length_size, length_endian
        self.add, self.min_frame, self.max_frame, self.escape, self.suffix_len = add, min_frame, max_frame, escape, suffix_len
        self.counts_wire, self.header_len, self.trailer_len = counts_wire, header_len, trailer_len
        self.wire_includes_trailer = wire_includes_trailer
        self._n = None


class DelimitedConfig:
    """Frames bounded by start / end delimiters (gmsg::DelimitedConfig)."""

    __slots__ = ("start", "end", "escape", "max_frame", "_n")

    def __init__(self, start: bytes = b"", end: bytes = b"", escape: EscapeConfig = NO_ESCAPE, max_frame: int = 0):
        self.start, self.end, self.escape, self.max_frame, self._n = bytes(start), bytes(end), escape, max_frame, None


def _native_cfg(cfg: Any, make: Callable[[], Any]) -> bool:
    """Build cfg's accelerator configuration once; False when the C runtime cannot represent it (patterns longer
    than 8 bytes): the pure-Python scanner is used then."""
    if cfg._n is None:
        try:
            cfg._n = make()
        except ValueError:
            cfg._n = False
    return cfg._n is not False


def scan_fixed(buf: BytesLike, cfg: FixedConfig) -> FrameScan:
    nat = _accel.native
    if nat is not None and _native_cfg(cfg, lambda: nat.fixed_config(cfg.sync, cfg.frame_size,
                                                                     cfg.escape.native_handle(), cfg.suffix_len)):
        return _from_native(nat.scan_fixed(cfg._n, buf))
    r = find_pattern_at_start(buf, cfg.sync)
    if r is not None:
        return r
    esc = cfg.escape
    if not esc.enabled:
        if len(buf) < cfg.frame_size:
            return FrameScan.need_more(cfg.frame_size)
        return FrameScan.frame(cfg.frame_size, cfg.frame_size, 1 if cfg.sync else cfg.frame_size)
    s = len(cfg.sync)
    wk = walk_escaped(buf, s, cfg.frame_size - s - cfg.suffix_len, esc)
    if wk.kind == "need_more":
        return FrameScan.need_more()
    if wk.kind == "bad":
        return FrameScan.skip(wk.wire if wk.wire > 0 else 1, wk.error)
    n = wk.wire + cfg.suffix_len
    if len(buf) < n:
        return FrameScan.need_more(n)
    return FrameScan.frame(n, n, 1 if s else n)


def scan_length_prefixed(buf: BytesLike, cfg: LengthPrefixedConfig) -> FrameScan:
    nat = _accel.native
    if nat is not None and _native_cfg(cfg, lambda: nat.length_prefixed_config(
            cfg.sync, cfg.length_offset, cfg.length_size, cfg.length_endian == "little", cfg.add, cfg.min_frame,
            cfg.max_frame, cfg.escape.native_handle(), cfg.suffix_len, cfg.counts_wire, cfg.header_len,
            cfg.trailer_len, cfg.wire_includes_trailer)):
        return _from_native(nat.scan_length_prefixed(cfg._n, buf))
    r = find_pattern_at_start(buf, cfg.sync)
    if r is not None:
        return r
    esc = cfg.escape
    if esc.enabled and cfg.counts_wire:
        return _scan_length_prefixed_wire(buf, cfg)
    ls = cfg.length_size
    need = cfg.length_offset + ls
    if not esc.enabled:
        if len(buf) < need:
            return FrameScan.need_more(need)
        value = int.from_bytes(buf[cfg.length_offset:need], cfg.length_endian)
    else:
        s = len(cfg.sync)
        lb = bytearray(ls)
        wk = walk_escaped(buf, s, need - s, esc, cfg.length_offset - s, lb)
        if wk.kind == "need_more":
            return FrameScan.need_more()
        if wk.kind == "bad":
            return FrameScan.skip(wk.wire if wk.wire > 0 else 1, wk.error)
        value = int.from_bytes(lb, cfg.length_endian)
    total = value + cfg.add
    if total < cfg.min_frame or total > cfg.max_frame or total <= 0:
        return FrameScan.skip(1, Err.BAD_LENGTH)
    n = total
    if esc.enabled:
        s = len(cfg.sync)
        if n < s + cfg.suffix_len:
            return FrameScan.skip(1, Err.BAD_LENGTH)
        wk = walk_escaped(buf, s, n - s - cfg.suffix_len, esc)
        if wk.kind == "need_more":
            if wk.wire > cfg.max_frame:
                return FrameScan.skip(1, Err.BAD_LENGTH)
            return FrameScan.need_more()
        if wk.kind == "bad":
            return FrameScan.skip(wk.wire if wk.wire > 0 else 1, wk.error)
        n = wk.wire + cfg.suffix_len
        if n > cfg.max_frame:
            return FrameScan.skip(1, Err.BAD_LENGTH)
    if len(buf) < n:
        return FrameScan.need_more(n)
    return FrameScan.frame(n, n, 1)


def _scan_length_prefixed_wire(buf: BytesLike, cfg: LengthPrefixedConfig) -> FrameScan:
    esc = cfg.escape
    s = len(cfg.sync)
    lb = bytearray(cfg.length_size)
    hw = walk_escaped(buf, s, cfg.header_len, esc, cfg.length_offset - s, lb)
    if hw.kind == "need_more":
        return FrameScan.need_more()
    if hw.kind == "bad":
        return FrameScan.skip(hw.wire if hw.wire > 0 else 1, hw.error)
    body = int.from_bytes(lb, cfg.length_endian) + cfg.add
    if body < 0 or hw.wire + body + cfg.suffix_len > cfg.max_frame:
        return FrameScan.skip(1, Err.BAD_LENGTH)
    body_end = hw.wire + body
    i = hw.wire
    while i < body_end:
        if i >= len(buf):
            return FrameScan.need_more(body_end)
        if buf[i] == esc.esc:
            if i + 1 >= body_end:
                return FrameScan.skip(1, Err.BAD_LENGTH)
            i += 1
        elif esc.needs_escape(buf[i]):
            return FrameScan.skip(i, Err.BAD_DELIMITER)
        i += 1
    end = body_end
    if not cfg.wire_includes_trailer:
        tw = walk_escaped(buf, body_end, cfg.trailer_len, esc)
        if tw.kind == "need_more":
            return FrameScan.need_more()
        if tw.kind == "bad":
            return FrameScan.skip(tw.wire, tw.error)
        end = tw.wire
    n = end + cfg.suffix_len
    if n < cfg.min_frame or n > cfg.max_frame:
        return FrameScan.skip(1, Err.BAD_LENGTH)
    if len(buf) < n:
        return FrameScan.need_more(n)
    return FrameScan.frame(n, n, 1)


def scan_delimited(buf: BytesLike, cfg: DelimitedConfig) -> FrameScan:
    nat = _accel.native
    if nat is not None and _native_cfg(cfg, lambda: nat.delimited_config(cfg.start, cfg.end, cfg.escape.native_handle(),
                                                                         cfg.max_frame)):
        return _from_native(nat.scan_delimited(cfg._n, buf))
    start, end, esc = cfg.start, cfg.end, cfg.escape
    r = find_pattern_at_start(buf, start)
    if r is not None:
        return r
    s = len(start)
    shared = s > 0 and s == len(end) and end == start
    b = bytes(buf)
    i = s
    while i < len(b):
        if i + 1 > cfg.max_frame:
            return FrameScan.skip(s if s else i, Err.BAD_LENGTH)
        if esc.enabled and b[i] == esc.esc:
            i += 2
            continue
        if b.startswith(end, i):
            n = i + len(end)
            if shared:
                if i == s:
                    return FrameScan.skip(s)
                return FrameScan.frame(n, n - len(end), n - len(end))
            return FrameScan.frame(n, n, n)
        if not shared and s and b.startswith(start, i):
            return FrameScan.skip(i, Err.BAD_DELIMITER)
        i += 1
    if len(b) >= cfg.max_frame:
        return FrameScan.skip(s if s else len(b), Err.BAD_LENGTH)
    return FrameScan.need_more()
