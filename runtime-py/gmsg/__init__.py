"""gmsg - the GMP runtime for Python (used by `gmpc --lang python` output).

A port of the C++ runtime (runtime/include/gmp) with the same behaviour and the same text output, written the way
Python code is normally written:
  - errors are raised as GmpError (`.code` is an Err, `str(e)` the same text as gmsg::to_string)
  - bytes are bytes / bytearray / memoryview; strings hold one character per byte (latin-1), so they round-trip
  - integers are Python ints of any width, checked against their wire type when encoded
  - encoders return bytes; printing returns a string
  - the stream parser takes listeners (`on(name, fn)`) and has a pull API with iterators

The optional accelerator `gmsg._native` (built from the C runtime) speeds up checksums, escaping, frame scanning,
number parsing and float text. It is used automatically when installed (set GMSG_PURE=1 to disable it); results and
API are the same either way. `gmsg.NATIVE` tells which is in use.
"""

from __future__ import annotations

from . import _accel
from . import checksum
from . import text
from .core import (BIG, LITTLE, LSB_FIRST, MSB_FIRST, SIZE, Reader, Writer, attempt, char_code, check_char,
                   check_int, extract_bits, get_bits, idiv, imod, insert_bits, latin1_bytes, latin1_string, llround,
                   load, load_uint, set_bits, sign_extend, store_uint, strip_padding, to_bytes, to_f32, to_int,
                   wrap_int)
from .errors import Err, GmpError, error_text, fail
from .gen import (FieldRange, GmpEnum, Opened, TextField, as_view, build, count, fixed_array, i64,
                  known_enum, limited, seq_array)
from .floattext import format_float, format_general, parse_float
from .framing import (NO_ESCAPE, DelimitedConfig, EscapeConfig, FixedConfig, FrameScan, LengthPrefixedConfig,
                      check_escaped, escape, escape_overhead, escape_range, find_pattern_at_start, index_escape,
                      map_escape, matches_at, read_frame_header, scan_delimited, scan_fixed, scan_length_prefixed,
                      unescape, unescaped_size, walk_escaped, xor_escape)
from .printer import Printer, to_text
from .stream import Received, StreamParser, StreamStats, extra_of

__version__ = "0.1.0"


def __getattr__(name: str):  # NATIVE reflects use_native() switches
    if name == "NATIVE":
        return _accel.native is not None
    raise AttributeError(name)


use_native = _accel.use_native

__all__ = [
    "BIG", "LITTLE", "MSB_FIRST", "LSB_FIRST", "SIZE", "Err", "GmpError", "error_text", "fail",
    "Reader", "Writer", "load", "load_uint", "store_uint", "check_int", "wrap_int", "to_int", "to_f32", "llround",
    "idiv", "imod", "strip_padding", "to_bytes", "latin1_bytes", "latin1_string", "char_code", "check_char",
    "extract_bits", "insert_bits", "sign_extend", "get_bits", "set_bits", "attempt",
    "checksum", "text",
    "EscapeConfig", "NO_ESCAPE", "xor_escape", "map_escape", "index_escape", "walk_escaped", "read_frame_header",
    "unescape", "unescaped_size", "check_escaped", "escape_overhead", "escape_range", "escape",
    "FrameScan", "FixedConfig", "LengthPrefixedConfig", "DelimitedConfig", "find_pattern_at_start", "matches_at",
    "scan_fixed", "scan_length_prefixed", "scan_delimited",
    "StreamParser", "StreamStats", "Received", "extra_of",
    "format_float", "format_general", "parse_float",
    "Printer", "to_text", "use_native", "NATIVE",
    "GmpEnum", "known_enum", "Opened", "FieldRange", "TextField", "i64", "count", "limited", "as_view", "fixed_array",
    "seq_array", "build",
]
