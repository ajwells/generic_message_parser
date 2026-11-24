"""Human-readable text of messages (gmp/print.hpp). Generated classes have print_to(p) and __str__ (= to_text(self));
the text is the same as gmsg::print in C++, gmsg_print in C and gmsg.print in JavaScript."""

from __future__ import annotations

from typing import Any, Callable, Iterable

from .errors import error_text
from .floattext import format_float, format_general

_HEX = "0123456789ABCDEF"


def _hex_byte(b: int) -> str:
    return _HEX[(b >> 4) & 15] + _HEX[b & 15]


def _esc_char(c: int, quote: int) -> str:
    if c == 10:
        return "\\n"
    if c == 13:
        return "\\r"
    if c == 9:
        return "\\t"
    if c == 92:
        return "\\\\"
    if c == quote:
        return "\\" + chr(c)
    if c < 0x20 or c >= 0x7F:
        return "\\x" + _hex_byte(c)
    return chr(c)


class Printer:
    """The printer of gmsg::Printer, writing into a string. Options: style ('multi_line' | 'single_line'), indent,
    max_items, max_bytes (0 = all), precision (significant digits of converted values)."""

    def __init__(self, style: str = "multi_line", indent: int = 2, max_items: int = 64, max_bytes: int = 64,
                 precision: int = 9) -> None:
        self.style, self.indent, self.max_items, self.max_bytes, self.precision = style, indent, max_items, max_bytes, precision
        self._out: list[str] = []
        self._depth = 0
        self._firsts: list[bool] = []
        self._note_open = False

    @property
    def multi_line(self) -> bool:
        return self.style != "single_line"

    def text(self) -> str:
        return "".join(self._out)

    def _put(self, t: str) -> None:
        self._out.append(t)

    def _push(self) -> None:
        if len(self._firsts) <= self._depth:
            self._firsts.append(True)
        else:
            self._firsts[self._depth] = True
        self._depth += 1

    def _pop(self) -> None:
        if self._depth:
            self._depth -= 1

    def _is_first(self) -> bool:
        return self._depth > 0 and self._firsts[self._depth - 1]

    def _clear_first(self) -> None:
        if self._depth > 0:
            self._firsts[self._depth - 1] = False

    def _newline(self) -> None:
        self._put("\n" + " " * (self._depth * self.indent))

    def _separate(self) -> None:
        if self.multi_line:
            self._newline()
        else:
            self._put(" " if self._is_first() else ", ")
        self._clear_first()

    def _open_note(self) -> None:
        self._put(", " if self._note_open else " (")
        self._note_open = True

    def _close_note(self) -> None:
        if self._note_open:
            self._note_open = False
            self._put(")")

    def _more(self, k: int) -> None:
        self._put(f"... (+{k} more)")

    # ---- structure ----
    def begin(self, type_name: str = "") -> None:
        self._close_note()
        if type_name:
            self._put(type_name + " ")
        self._put("{")
        self._push()

    def end(self) -> None:
        self._close_note()
        empty = self._is_first()
        self._pop()
        if empty:
            self._put("}")
        elif self.multi_line:
            self._newline()
            self._put("}")
        else:
            self._put(" }")

    def key(self, name: str) -> None:
        self._close_note()
        self._separate()
        self._put(name + ": ")

    def absent(self, name: str) -> None:
        self.key(name)
        self._put("<absent>")

    def unit(self, u: str | None) -> None:
        if u:
            self._put(" " + u)

    def conversion(self, x: float, unit: str | None = None) -> None:
        """A converted value in the note after a value: `12600 (12.6 V)`."""
        self._open_note()
        self._put(format_general(float(x), self.precision))
        self.unit(unit)

    def note(self, t: str) -> None:
        self._open_note()
        self._put(t)

    def raw_text(self, t: str) -> None:
        self._put(t)

    def flush(self) -> None:
        self._close_note()

    # ---- values ----
    def int_value(self, v: int) -> None:
        self._put(str(int(v)))

    def float_value(self, v: float, is_f32: bool = False) -> None:
        self._put(format_float(float(v), -1, is_f32))

    def bool_value(self, v: bool) -> None:
        self._put("true" if v else "false")

    def char_value(self, c: str | int) -> None:
        """A char: a one-character string or a byte value."""
        self._put("'" + _esc_char(ord(c) if isinstance(c, str) else c, 39) + "'")

    def enum_value(self, name: str, raw: int, size: int) -> None:
        """An enum value: `name (0xNN)`, `<unknown> (0xNN)` when name is empty; size = bytes of the base type."""
        u = int(raw) & ((1 << (8 * size)) - 1)
        self._put(f"{name or '<unknown>'} (0x{u:0{2 * size}X})")

    def hex_bytes(self, data: bytes | bytearray | memoryview) -> None:
        n = len(data)
        shown = self.max_bytes if self.max_bytes and n > self.max_bytes else n
        self._put("[" + " ".join(_hex_byte(b) for b in data[:shown]))
        if shown < n:
            self._put(" ")
            self._more(n - shown)
        self._put("]")

    def str_value(self, s: str) -> None:
        """A quoted string (one character per byte)."""
        self._put('"' + "".join(_esc_char(ord(c), 34) for c in s) + '"')

    def error_value(self, code: int) -> None:
        self._put(f"<error: {error_text(code)}>")

    def list_value(self, items: Iterable[Any], each: Callable[[Any], None], blocks: bool = False) -> None:
        """`[a, b, c]`; `each(item)` prints one element; blocks = elements are messages (one per line)."""
        items = list(items)
        n = len(items)
        shown = self.max_items if self.max_items and n > self.max_items else n
        self._put("[")
        if not n:
            self._put("]")
            return
        lines = blocks and self.multi_line
        if lines:
            self._push()
        for i in range(shown):
            if lines:
                self._newline()
            elif i:
                self._put(", ")
            each(items[i])
        if shown < n:
            if lines:
                self._newline()
            else:
                self._put(", ")
            self._more(n - shown)
        if lines:
            self._pop()
            self._newline()
        self._put("]")

    def value(self, v: Any) -> None:
        """A value with its own print_to (generated classes), or a plain value."""
        if hasattr(v, "print_to"):
            v.print_to(self)
        elif isinstance(v, bool):
            self.bool_value(v)
        elif isinstance(v, int):
            self.int_value(v)
        elif isinstance(v, float):
            self.float_value(v)
        elif isinstance(v, str):
            self.str_value(v)
        elif isinstance(v, (bytes, bytearray, memoryview)):
            self.hex_bytes(v)
        elif v is None:
            self._put("<absent>")
        else:
            self._put("<?>")


def to_text(v: Any, **options: Any) -> str:
    """The text of a generated struct or view (gmsg::print). Options as Printer: style, indent, max_items, max_bytes,
    precision."""
    p = Printer(**options)
    p.value(v)
    p.flush()
    return p.text()
