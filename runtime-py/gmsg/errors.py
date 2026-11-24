"""Error codes and the exception raised by decoders, encoders and parsers (gmp/error.hpp)."""

from __future__ import annotations

import enum
from typing import NoReturn


class Err(enum.IntEnum):
    """Error codes: the same values and order as gmsg::Error (C++), gmsg_error (C) and gmsg.Err (JavaScript)."""

    NONE = 0
    TRUNCATED = 1
    BAD_SYNC = 2
    BAD_CHECKSUM = 3
    BAD_LENGTH = 4
    BAD_DELIMITER = 5
    BAD_CONST = 6
    OUT_OF_RANGE = 7
    UNKNOWN_TYPE = 8
    BUFFER_FULL = 9
    PARSE_ERROR = 10
    MISSING_FIELD = 11
    INVALID_VALUE = 12
    CUSTOM = 13
    UNKNOWN_ENUM = 14
    VALUE_OUT_OF_LIMITS = 15
    OVERRUN = 16
    TIMEOUT = 17


_TEXT = (
    "none", "truncated", "bad sync", "bad checksum", "bad length", "bad delimiter", "bad constant", "out of range",
    "unknown message type", "buffer full", "parse error", "missing field", "invalid value", "custom error",
    "unknown enum value", "value out of limits", "overrun", "timeout",
)


def error_text(code: int) -> str:
    """Text of an error code, as gmsg::to_string(Error): 'bad checksum'."""
    return _TEXT[code] if 0 <= code < len(_TEXT) else "unknown"


class GmpError(Exception):
    """Raised by decoders, encoders and parsers. `code` is an Err; stream errors also carry `data` (the dropped
    bytes) and `offset` (their position in the stream)."""

    def __init__(self, code: int, detail: str | None = None) -> None:
        try:
            code = Err(code)
        except ValueError:
            pass
        super().__init__(f"{error_text(code)}: {detail}" if detail else error_text(code))
        self.code = code
        self.data: bytes | None = None
        self.offset: int | None = None

    @property
    def text(self) -> str:
        """The gmsg::to_string text of the code, without the detail."""
        return error_text(self.code)


def fail(code: int, detail: str | None = None) -> NoReturn:
    raise GmpError(code, detail)
