"""Support for generated code (`gmpc --lang python`): the enum base class, results of views and protocols, and small
helpers the generated modules call."""

from __future__ import annotations

import dataclasses
import enum
from typing import Any, Callable, NamedTuple

from .core import to_bytes
from .errors import Err, fail

_I64 = 1 << 64


class GmpEnum(enum.IntEnum):
    """Base of generated enums. Fields hold the member when the value is declared, else the plain int (non-strict
    enums keep undeclared values), so `msg.mode == Mode.run` works either way."""

    @classmethod
    def of(cls, v: Any) -> Any:
        """The member with value v, or v itself when no member has it."""
        return cls._value2member_map_.get(v, v)

    @classmethod
    def name_of(cls, v: Any) -> str:
        """The schema name of value v; '' when undeclared."""
        m = cls._value2member_map_.get(v)
        if m is None:
            return ""
        names = getattr(cls, "__gmp_names__", None)
        return names.get(m.name, m.name) if names else m.name

    @classmethod
    def is_known(cls, v: Any) -> bool:
        return v in cls._value2member_map_


def known_enum(e: type[GmpEnum], v: Any) -> Any:
    """The member of strict enum e with value v; GmpError UnknownEnum when there is none."""
    m = e._value2member_map_.get(v)
    if m is None:
        fail(Err.UNKNOWN_ENUM)
    return m


class Opened(NamedTuple):
    """A validated frame split by a protocol's open(): the body (bytes after the header, before the checksum; the record
    text for text protocols), the header view (binary protocols with a header) or the type token (text protocols)."""

    body: Any
    header: Any = None
    type_id: str | None = None


class FieldRange(NamedTuple):
    """Bytes of one present field of a view (relative to view.data); key is the attribute (None for padding and
    constants)."""

    name: str
    key: str | None
    start: int
    end: int


class TextField(NamedTuple):
    """One field of a text message: its name, attribute (None for padding / constants), first token and token count."""

    name: str
    key: str | None
    token: int
    tokens: int


def i64(v: int) -> int:
    """v wrapped to a signed 64-bit integer (the int64 arithmetic of schema expressions)."""
    v &= _I64 - 1
    return v - _I64 if v >> 63 else v


def count(v: Any) -> int:
    """Elements of a container field (bytes, string or list); 0 for None."""
    return 0 if v is None else len(v)


def limited(v: Any, fails: Callable[[Any], bool]) -> Any:
    """v, or GmpError ValueOutOfLimits when fails(v)."""
    if fails(v):
        fail(Err.VALUE_OUT_OF_LIMITS)
    return v


def as_view(data: Any) -> memoryview:
    """A byte memoryview over data (bytes, bytearray, memoryview, str with one character per byte, list of ints)."""
    b = to_bytes(data)
    return b if isinstance(b, memoryview) else memoryview(b)


def fixed_array(b: memoryview, size: int, make: Callable[[memoryview], Any]) -> list[Any]:
    """Elements of `size` bytes one after another."""
    return [make(b[i * size:(i + 1) * size]) for i in range(len(b) // size)]


def seq_array(b: memoryview, make: Callable[[memoryview], Any]) -> list[Any]:
    """Variable-size elements one after another: make(rest) returns a view with wire_size."""
    out = []
    pos = 0
    while pos < len(b):
        v = make(b[pos:])
        out.append(v)
        pos += v.wire_size or len(b)
    return out


def build(cls: type, init: Any) -> Any:
    """A new cls (a generated dataclass) with the values of init: None, a dict of fields, or an instance (copied)."""
    if init is None:
        return cls()
    if isinstance(init, dict):
        return cls(**init)
    return dataclasses.replace(init)
