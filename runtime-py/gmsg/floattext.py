"""Exact float text: the same results as std::to_chars / std::from_chars (ported from gmp/c/float_text.h).

format_float(x, precision=-1, is_f32=False): precision >= 0 gives fixed notation with that many decimals, otherwise
the shortest text that reads back to the same value of its type. format_general(x, digits) is printf "%.*g".
parse_float(s, is_f32=False) parses strictly (after one leading '+'), raising MissingField / ParseError / OutOfRange.
"""

from __future__ import annotations

import math
import re
import struct

from . import _accel
from .errors import Err, fail


class _Fmt:
    __slots__ = ("p", "emin_lsb", "emax", "bias", "exp_bits")

    def __init__(self, p: int, emin_lsb: int, emax: int, bias: int, exp_bits: int) -> None:
        self.p, self.emin_lsb, self.emax, self.bias, self.exp_bits = p, emin_lsb, emax, bias, exp_bits


F64 = _Fmt(53, -1074, 2047, 1023, 11)
F32 = _Fmt(24, -149, 255, 127, 8)

_P10 = [1]


def _pow10(k: int) -> int:
    while len(_P10) <= k:
        _P10.append(_P10[-1] * 10)
    return _P10[k]


def _bits_of(x: float, is_f32: bool) -> int:
    if is_f32:
        return struct.unpack("<I", struct.pack("<f", x))[0]
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def _from_bits(bits: int, is_f32: bool) -> float:
    if is_f32:
        return struct.unpack("<f", struct.pack("<I", bits))[0]
    return struct.unpack("<d", struct.pack("<Q", bits))[0]


def _split(x: float, is_f32: bool) -> tuple[int, bool, int, int]:
    """(kind 0 finite | 1 inf | 2 nan, negative, significand, exponent of its lowest bit)."""
    f = F32 if is_f32 else F64
    bits = _bits_of(x, is_f32)
    pm = f.p - 1
    frac = bits & ((1 << pm) - 1)
    biased = (bits >> pm) & ((1 << f.exp_bits) - 1)
    neg = (bits >> (pm + f.exp_bits)) & 1 == 1
    if biased == f.emax:
        return (2 if frac else 1), neg, 0, 0
    if biased == 0:
        return 0, neg, frac, f.emin_lsb
    return 0, neg, frac | (1 << pm), biased - f.bias - pm


def _make_bits(q: int, e2: int, sticky: bool, f: _Fmt) -> int | None:
    """Round q * 2^e2 (+ sticky remainder) to the format, ties to even: raw bits, or None on overflow."""
    if q == 0:
        return 0
    lead = e2 + q.bit_length() - 1
    lsb = max(lead - (f.p - 1), f.emin_lsb)
    shift = lsb - e2
    if shift <= 0:
        m = q << -shift
    else:
        rem = q & ((1 << shift) - 1)
        half = 1 << (shift - 1)
        m = q >> shift
        if rem > half or (rem == half and (sticky or m & 1)):
            m += 1
    if m == 1 << f.p:
        m >>= 1
        lsb += 1
    if m >= 1 << (f.p - 1):
        biased = lsb + (f.p - 1) + f.bias
        if biased >= f.emax:
            return None
        return (biased << (f.p - 1)) | (m - (1 << (f.p - 1)))
    return m


_NAN = re.compile(r"nan\([0-9a-z_]*\)\Z")


def parse_float(s: str, is_f32: bool = False) -> float:
    """Parse as std::from_chars (after gmp's leading '+' strip)."""
    nat = _accel.native
    if nat is not None:
        r = nat.parse_float(s, is_f32)
        if isinstance(r, int):
            fail(r)
        return r
    f = F32 if is_f32 else F64
    if not s:
        fail(Err.MISSING_FIELD)
    if s[0] == "+":
        s = s[1:]
    i, neg = 0, False
    if s[:1] == "-":
        neg, i = True, 1

    def signed(x: float) -> float:
        return -x if neg else x

    c0 = s[i:i + 1]
    if c0 in ("i", "I", "n", "N"):
        r = s[i:].lower()
        if r in ("inf", "infinity"):
            return signed(math.inf)
        if r.startswith("nan"):
            if len(r) > 3 and not _NAN.match(r):
                fail(Err.PARSE_ERROR)
            return signed(math.nan)
        fail(Err.PARSE_ERROR)
    num = ndig = exp10 = 0
    any_digit = dropped = point = False
    leading = True
    n = len(s)
    while i < n:
        c = s[i]
        if c == ".":
            if point:
                break
            point = True
            i += 1
            continue
        if not "0" <= c <= "9":
            break
        any_digit = True
        if leading and c == "0":
            if point:
                exp10 -= 1
            i += 1
            continue
        leading = False
        if ndig < 800:
            num = num * 10 + (ord(c) - 48)
            ndig += 1
            if point:
                exp10 -= 1
        else:
            if c != "0":
                dropped = True
            if not point:
                exp10 += 1
        i += 1
    if not any_digit:
        fail(Err.PARSE_ERROR)
    if i < n and s[i] in ("e", "E"):
        j, en, e = i + 1, False, 0
        if j < n and s[j] in ("+", "-"):
            en = s[j] == "-"
            j += 1
        if j < n and "0" <= s[j] <= "9":
            while j < n and "0" <= s[j] <= "9":
                if e < 100000000:
                    e = e * 10 + (ord(s[j]) - 48)
                j += 1
            exp10 += -e if en else e
            i = j
    trailing = i != n
    if dropped:
        num = num * 10 + 1
        ndig += 1
        exp10 -= 1
    if num == 0:
        if trailing:
            fail(Err.PARSE_ERROR)
        return -0.0 if neg else 0.0
    lead10 = exp10 + ndig - 1
    if lead10 > 310 or lead10 < -400:
        fail(Err.OUT_OF_RANGE)
    if exp10 >= 0:
        v = num * _pow10(exp10)
        bl = v.bit_length()
        sticky, q, e2 = False, v, 0
        if bl > 60:
            e2 = bl - 60
            sticky = (v & ((1 << e2) - 1)) != 0
            q = v >> e2
        raw = _make_bits(q, e2, sticky, f)
    else:
        a, b = num, _pow10(-exp10)
        sh = (f.p + 4) + b.bit_length() - a.bit_length()
        if sh > 0:
            a <<= sh
        else:
            b <<= -sh
        raw = _make_bits(a // b, -sh, a % b != 0, f)
    if raw is None or raw == 0:
        fail(Err.OUT_OF_RANGE)
    if trailing:
        fail(Err.PARSE_ERROR)
    return signed(_from_bits(raw, is_f32))


def _fixed_digits(m: int, e2: int, precision: int) -> str:
    """Fixed notation with `precision` decimals of m * 2^e2, rounded half to even."""
    frac, fb = 0, 0
    if e2 >= 0:
        ip = m << e2
    else:
        fb = -e2
        ip = m >> fb
        frac = m & ((1 << fb) - 1)
    one = 1 << fb
    num = frac * _pow10(precision)
    q = num >> fb
    rem = num - (q << fb)
    whole = ip * _pow10(precision) + q
    if rem:
        c = rem * 2 - one
        if c > 0 or (c == 0 and whole & 1):
            whole += 1
    t = str(whole)
    if precision > 0:
        t = t.rjust(precision + 1, "0")
        t = t[:-precision] + "." + t[-precision:]
    return t


def _shortest_digits(m: int, e2: int, f: _Fmt) -> tuple[str, int]:
    """Shortest round-trip digits (Burger & Dybvig): (digits, k) with value = 0.digits * 10^k."""
    even = m & 1 == 0
    unequal = m == 1 << (f.p - 1) and e2 > f.emin_lsb
    if e2 >= 0:
        r = m << (e2 + (2 if unequal else 1))
        s = 4 if unequal else 2
        mp = 1 << (e2 + (1 if unequal else 0))
        mm = 1 << e2
    else:
        r = m << (2 if unequal else 1)
        s = 1 << (-e2 + (2 if unequal else 1))
        mp = 2 if unequal else 1
        mm = 1
    lead = e2 + m.bit_length() - 1
    k = (lead * 78913) // 262144 + 1
    if k >= 0:
        s *= _pow10(k)
    else:
        p = _pow10(-k)
        r, mp, mm = r * p, mp * p, mm * p
    while True:
        c = r + mp
        if (c >= s) if even else (c > s):
            s *= 10
            k += 1
            continue
        c10 = c * 10
        if (c10 < s) if even else (c10 <= s):
            r, mp, mm = r * 10, mp * 10, mm * 10
            k -= 1
            continue
        break
    digits: list[int] = []
    while True:
        r, mp, mm = r * 10, mp * 10, mm * 10
        d, r = divmod(r, s)
        tc1 = (r <= mm) if even else (r < mm)
        tc2 = (r + mp >= s) if even else (r + mp > s)
        if not tc1 and not tc2:
            digits.append(d)
            if len(digits) >= 40:
                break
            continue
        if tc1 and tc2:
            c = r * 2
            if c > s or (c == s and d & 1):
                d += 1
        elif tc2:
            d += 1
        digits.append(d)
        break
    i = len(digits) - 1
    while i > 0 and digits[i] > 9:
        digits[i] = 0
        i -= 1
        digits[i] += 1
    if digits[0] > 9:
        digits = [1] + [0] * (len(digits) - 1)
        k += 1
    while len(digits) > 1 and digits[-1] == 0:
        digits.pop()
    return "".join(map(str, digits)), k


def _exp_text(x: int) -> str:
    return "e" + ("-" if x < 0 else "+") + str(abs(x)).rjust(2, "0")


def _shortest_text(m: int, e2: int, f: _Fmt) -> str:
    d, k = _shortest_digits(m, e2, f)
    n, x = len(d), k - 1
    sci_len = n + (1 if n > 1 else 0) + 2 + (3 if abs(x) >= 100 else 2)
    fixed_len = 2 - k + n if k <= 0 else (n + 1 if k < n else k)
    if fixed_len <= sci_len:
        if k <= 0:
            return "0." + "0" * -k + d
        if k < n:
            return d[:k] + "." + d[k:]
        if e2 > 0:
            return _fixed_digits(m, e2, 0)  # an integer with gaps of 2+: its exact value, as to_chars
        return d + "0" * (k - n)
    return d[0] + ("." + d[1:] if n > 1 else "") + _exp_text(x)


def _general_text(m: int, e2: int, sig: int) -> str:
    """printf "%.*g" / to_chars(general, sig): `sig` significant digits, ties to even."""
    sig = max(sig, 1)
    if m == 0:
        d, k = "0", 1
    else:
        a, scale = m, 0
        if e2 >= 0:
            a <<= e2
        else:
            scale = -e2
            a *= 5 ** scale
        d = str(a)
        k = len(d) - scale
        d = d.rstrip("0") or "0"
    if len(d) > sig:
        nxt = d[sig]
        up = nxt > "5" if nxt != "5" else (len(d) > sig + 1 or int(d[sig - 1]) % 2 == 1)
        ds = [int(c) for c in d[:sig]]
        if up:
            i = sig - 1
            while i >= 0 and ds[i] == 9:
                ds[i] = 0
                i -= 1
            if i >= 0:
                ds[i] += 1
            else:
                ds = [1] + ds[1:]
                k += 1
        d = "".join(map(str, ds)).rstrip("0") or "0"
    x = k - 1
    if -4 <= x < sig:
        if k <= 0:
            return "0." + "0" * -k + d
        out = []
        i = 0
        while i < len(d) or i < k:
            if i == k:
                out.append(".")
            out.append(d[i] if i < len(d) else "0")
            i += 1
        return "".join(out)
    return d[0] + ("." + d[1:] if len(d) > 1 else "") + _exp_text(x)


def format_float(x: float, precision: int = -1, is_f32: bool = False) -> str:
    """As std::to_chars: precision >= 0 gives fixed notation with that many decimals; otherwise the shortest text
    that reads back to the same value of its type (f32 when is_f32)."""
    nat = _accel.native
    if nat is not None:
        r = nat.format_float(float(x), precision, is_f32)
        if isinstance(r, int):
            fail(r)
        return r
    kind, neg, m, e2 = _split(float(x), is_f32 and precision < 0)
    sign = "-" if neg else ""
    if kind == 1:
        return sign + "inf"
    if kind == 2:
        return sign + "nan"
    if precision >= 0:
        return sign + _fixed_digits(m, e2, precision)
    if m == 0:
        return sign + "0"
    return sign + _shortest_text(m, e2, F32 if is_f32 else F64)


def format_general(x: float, digits: int) -> str:
    """std::to_chars(x, chars_format::general, digits), as printf "%.*g"."""
    nat = _accel.native
    if nat is not None:
        return nat.format_general(float(x), digits)
    kind, neg, m, e2 = _split(float(x), False)
    sign = "-" if neg else ""
    if kind == 1:
        return sign + "inf"
    if kind == 2:
        return sign + "nan"
    return sign + _general_text(m, e2, max(digits, 1))
