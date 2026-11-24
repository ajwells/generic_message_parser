/* gmp/c/float_text.h - float <-> text, matching C++ std::to_chars / std::from_chars (used by text messages).
 *
 *   gmsg_format_double(v, precision, out, cap, &n)   precision >= 0: fixed with that many decimals (correctly
 *   gmsg_format_float (v, precision, out, cap, &n)   rounded, ties to even); precision < 0: the shortest text that
 *                                                    reads back to v (as to_chars(v): fixed or scientific,
 *                                                    whichever is shorter, fixed on a tie)
 *   gmsg_format_general(v, digits, out, cap, &n)    `digits` significant digits as to_chars(v, general, digits),
 *                                                    i.e. printf "%.*g" (used by print for converted values)
 *   gmsg_parse_double(s, n, &v) / gmsg_parse_float(s, n, &v)   strict, locale-free, correctly rounded
 *       GMSG_ERR_MISSING_FIELD for an empty token, GMSG_ERR_PARSE for bad syntax, GMSG_ERR_OUT_OF_RANGE when the
 *       value overflows or underflows to zero. A leading '+' is accepted (as gmsg::text::parse_float).
 *
 * GMSG_FLOAT_TEXT (gmp/c/config.h) selects the implementation:
 *   GMSG_FLOAT_TEXT_BUILTIN (default)  exact algorithms on a fixed-size big integer: no heap, no <math.h>, same
 *                                      results as the C++ runtime. Uses about 2 KB of stack.
 *   GMSG_FLOAT_TEXT_LIBC               snprintf / strtod: smaller code, may differ from C++ in the last digit or in
 *                                      the choice of notation; uses the C locale's decimal point.
 *   GMSG_FLOAT_TEXT_NONE               not available (generated code with float text fields fails to compile).
 */
#ifndef GMSG_C_FLOAT_TEXT_H
#define GMSG_C_FLOAT_TEXT_H

#include "gmp/c/config.h"
#include "gmp/c/endian.h"
#include "gmp/c/error.h"

#if GMSG_FLOAT_TEXT == GMSG_FLOAT_TEXT_LIBC
#include <stdio.h>
#include <stdlib.h>
#endif

GMSG_C_BEGIN

#if GMSG_FLOAT_TEXT != GMSG_FLOAT_TEXT_NONE
GMSG_API gmsg_error gmsg_format_double(double v, int precision, char* out, size_t cap, size_t* written);
GMSG_API gmsg_error gmsg_format_float(float v, int precision, char* out, size_t cap, size_t* written);
GMSG_API gmsg_error gmsg_format_general(double v, int digits, char* out, size_t cap, size_t* written);
GMSG_API gmsg_error gmsg_parse_double(const char* s, size_t n, double* out);
GMSG_API gmsg_error gmsg_parse_float(const char* s, size_t n, float* out);
#endif

/* ---- implementation (header-only build, or runtime-c/src/float_text.c of the library build) ---------------------------- */

#if GMSG_DEFINE_FUNCTIONS_ || defined(GMSG_IMPL_FLOAT_TEXT_)

#if GMSG_FLOAT_TEXT == GMSG_FLOAT_TEXT_BUILTIN

/* ---- big unsigned integers (little-endian 32-bit words) ------------------------------------------------- */

#define GMSG_BIG_WORDS 136 /* 4352 bits: enough for every intermediate value below */

typedef struct gmsg_big {
    uint32_t w[GMSG_BIG_WORDS];
    int n; /* words in use (no leading zero words) */
} gmsg_big;

GMSG_PRIV_ void gmsg_big_trim_(gmsg_big* a) {
    while (a->n > 0 && a->w[a->n - 1] == 0) --a->n;
}

GMSG_PRIV_ void gmsg_big_set_(gmsg_big* a, uint64_t v) {
    a->w[0] = (uint32_t)v;
    a->w[1] = (uint32_t)(v >> 32);
    a->n = 2;
    gmsg_big_trim_(a);
}

GMSG_PRIV_ void gmsg_big_copy_(gmsg_big* dst, const gmsg_big* src) {
    int i;
    for (i = 0; i < src->n; ++i) dst->w[i] = src->w[i];
    dst->n = src->n;
}

GMSG_PRIV_ bool gmsg_big_is_zero_(const gmsg_big* a) { return a->n == 0; }

GMSG_PRIV_ void gmsg_big_mul_small_(gmsg_big* a, uint32_t m) {
    uint64_t carry = 0;
    int i;
    for (i = 0; i < a->n; ++i) {
        const uint64_t t = (uint64_t)a->w[i] * m + carry;
        a->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && a->n < GMSG_BIG_WORDS) a->w[a->n++] = (uint32_t)carry;
}

GMSG_PRIV_ void gmsg_big_add_small_(gmsg_big* a, uint32_t v) {
    uint64_t carry = v;
    int i;
    for (i = 0; i < a->n && carry; ++i) {
        const uint64_t t = (uint64_t)a->w[i] + carry;
        a->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && a->n < GMSG_BIG_WORDS) a->w[a->n++] = (uint32_t)carry;
}

GMSG_PRIV_ void gmsg_big_mul_pow10_(gmsg_big* a, int k) {
    while (k >= 9) {
        gmsg_big_mul_small_(a, 1000000000u);
        k -= 9;
    }
    {
        static const uint32_t p10[9] = {1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u};
        if (k > 0) gmsg_big_mul_small_(a, p10[k]);
    }
}

GMSG_PRIV_ void gmsg_big_shl_(gmsg_big* a, int bits) {
    const int words = bits / 32, b = bits % 32;
    int i;
    if (a->n == 0 || bits <= 0) return;
    if (a->n + words + 1 > GMSG_BIG_WORDS) return; /* cannot happen with the bounds used below */
    a->w[a->n + words] = 0;
    for (i = a->n - 1; i >= 0; --i) {
        const uint32_t v = a->w[i];
        if (b) {
            a->w[i + words + 1] |= v >> (32 - b);
            a->w[i + words] = v << b;
        } else {
            a->w[i + words] = v;
        }
    }
    for (i = 0; i < words; ++i) a->w[i] = 0;
    a->n += words + 1;
    gmsg_big_trim_(a);
}

GMSG_PRIV_ int gmsg_big_bitlen_(const gmsg_big* a) {
    uint32_t top;
    int bits = 0;
    if (a->n == 0) return 0;
    top = a->w[a->n - 1];
    while (top) {
        ++bits;
        top >>= 1;
    }
    return (a->n - 1) * 32 + bits;
}

GMSG_PRIV_ int gmsg_big_cmp_(const gmsg_big* a, const gmsg_big* b) {
    int i;
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; --i)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

/* a -= b (requires a >= b) */
GMSG_PRIV_ void gmsg_big_sub_(gmsg_big* a, const gmsg_big* b) {
    int64_t borrow = 0;
    int i;
    for (i = 0; i < a->n; ++i) {
        int64_t t = (int64_t)a->w[i] - borrow - (i < b->n ? (int64_t)b->w[i] : 0);
        borrow = t < 0 ? 1 : 0;
        if (t < 0) t += (int64_t)1 << 32;
        a->w[i] = (uint32_t)t;
    }
    gmsg_big_trim_(a);
}

/* a += b */
GMSG_PRIV_ void gmsg_big_add_(gmsg_big* a, const gmsg_big* b) {
    uint64_t carry = 0;
    int i;
    const int n = a->n > b->n ? a->n : b->n;
    for (i = 0; i < n; ++i) {
        const uint64_t t = (uint64_t)(i < a->n ? a->w[i] : 0) + (i < b->n ? b->w[i] : 0) + carry;
        a->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    a->n = n;
    if (carry && a->n < GMSG_BIG_WORDS) a->w[a->n++] = (uint32_t)carry;
}

/* compare a + b with c */
GMSG_PRIV_ int gmsg_big_cmp_sum_(const gmsg_big* a, const gmsg_big* b, const gmsg_big* c) {
    gmsg_big t;
    gmsg_big_copy_(&t, a);
    gmsg_big_add_(&t, b);
    return gmsg_big_cmp_(&t, c);
}

/* the low 64 bits */
GMSG_PRIV_ uint64_t gmsg_big_low64_(const gmsg_big* a) {
    return (a->n > 0 ? (uint64_t)a->w[0] : 0u) | (a->n > 1 ? (uint64_t)a->w[1] << 32 : 0u);
}

/* a >>= bits; *sticky |= any bit shifted out */
GMSG_PRIV_ void gmsg_big_shr_(gmsg_big* a, int bits, bool* sticky) {
    const int words = bits / 32, b = bits % 32;
    int i;
    if (bits <= 0) return;
    for (i = 0; i < words && i < a->n; ++i)
        if (a->w[i]) *sticky = true;
    if (b && words < a->n && (a->w[words] & ((1u << b) - 1u))) *sticky = true;
    if (words >= a->n) {
        a->n = 0;
        return;
    }
    for (i = 0; i + words < a->n; ++i) {
        const uint32_t lo = a->w[i + words];
        const uint32_t hi = i + words + 1 < a->n ? a->w[i + words + 1] : 0;
        a->w[i] = b ? (lo >> b) | (hi << (32 - b)) : lo;
    }
    a->n -= words;
    gmsg_big_trim_(a);
}

/* ---- binary floating point helpers ------------------------------------------------------------------------ */

/* Precision and exponent range of the two formats. */
typedef struct gmsg_fmt_ {
    int p;        /* significand bits (with the hidden bit) */
    int emin_lsb; /* exponent of the least significant bit of the smallest subnormal */
    int emax;     /* biased exponent of inf / nan */
    int bias;
} gmsg_fmt_;

GMSG_PRIV_ gmsg_fmt_ gmsg_fmt_of_(bool is_float) {
    gmsg_fmt_ f;
    f.p = is_float ? 24 : 53;
    f.emin_lsb = is_float ? -149 : -1074;
    f.emax = is_float ? 255 : 2047;
    f.bias = is_float ? 127 : 1023;
    return f;
}

/* Round q * 2^e2 (plus a nonzero remainder below q's lsb if sticky) to the format, ties to even. Returns the
 * raw bits without sign; *overflow when the result is infinite. */
GMSG_PRIV_ uint64_t gmsg_make_bits_(uint64_t q, int e2, bool sticky, gmsg_fmt_ f, bool* overflow) {
    int bl = 0, lead, lsb, shift;
    uint64_t m, t = q;
    *overflow = false;
    if (q == 0) return 0;
    while (t) {
        ++bl;
        t >>= 1;
    }
    lead = e2 + bl - 1;
    lsb = lead - (f.p - 1);
    if (lsb < f.emin_lsb) lsb = f.emin_lsb;
    shift = lsb - e2;
    if (shift <= 0) {
        m = q << -shift;
    } else if (shift >= 64) {
        /* everything is below the half point: rounds to zero */
        m = 0;
        (void)sticky;
    } else {
        const uint64_t rem = q & (((uint64_t)1 << shift) - 1u);
        const uint64_t half = (uint64_t)1 << (shift - 1);
        m = q >> shift;
        if (rem > half || (rem == half && (sticky || (m & 1u)))) ++m;
    }
    if (m == (uint64_t)1 << f.p) {
        m >>= 1;
        ++lsb;
    }
    if (m >= (uint64_t)1 << (f.p - 1)) {
        const int biased = lsb + (f.p - 1) + f.bias;
        if (biased >= f.emax) {
            *overflow = true;
            return (uint64_t)f.emax << (f.p - 1);
        }
        return ((uint64_t)biased << (f.p - 1)) | (m - ((uint64_t)1 << (f.p - 1)));
    }
    return m; /* subnormal (or zero) */
}

/* ---- parsing ------------------------------------------------------------------------------------------- */

GMSG_PRIV_ bool gmsg_ieq_(const char* s, size_t n, const char* word) {
    size_t i;
    for (i = 0; i < n; ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (word[i] == '\0' || c != word[i]) return false;
    }
    return word[n] == '\0';
}

/* Parse s[0..n) into the raw bits of a double (is_float = false) or float. */
GMSG_PRIV_ gmsg_error gmsg_parse_bits_(const char* s, size_t n, bool is_float, uint64_t* bits) {
    const gmsg_fmt_ f = gmsg_fmt_of_(is_float);
    const uint64_t sign_bit = (uint64_t)1 << (is_float ? 31 : 63);
    uint64_t sign = 0;
    size_t i = 0, ndig = 0;
    long long exp10 = 0;
    bool any_digit = false, dropped_nonzero = false, trailing = false;
    static const int max_digits = 800;
    gmsg_big num;
    num.n = 0;
    if (n == 0) return GMSG_ERR_MISSING_FIELD;
    if (s[0] == '+') {
        ++s;
        --n;
    }
    if (n > 0 && s[0] == '-') {
        sign = sign_bit;
        ++i;
    }
    /* inf / infinity / nan / nan(chars) */
    if (i < n && (s[i] == 'i' || s[i] == 'I' || s[i] == 'n' || s[i] == 'N')) {
        const char* r = s + i;
        const size_t rn = n - i;
        if (gmsg_ieq_(r, rn, "inf") || gmsg_ieq_(r, rn, "infinity")) {
            *bits = sign | ((uint64_t)f.emax << (f.p - 1));
            return GMSG_OK;
        }
        if (rn >= 3 && gmsg_ieq_(r, 3, "nan")) {
            size_t k = 3;
            if (rn > 3) {
                if (r[3] != '(' || r[rn - 1] != ')') return GMSG_ERR_PARSE;
                for (k = 4; k + 1 < rn; ++k) {
                    const char c = r[k];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) return GMSG_ERR_PARSE;
                }
            }
            *bits = sign | ((uint64_t)f.emax << (f.p - 1)) | ((uint64_t)1 << (f.p - 2));
            return GMSG_OK;
        }
        return GMSG_ERR_PARSE;
    }
    /* digits [. digits] */
    {
        bool seen_point = false, leading = true;
        for (; i < n; ++i) {
            const char c = s[i];
            if (c == '.') {
                if (seen_point) break;
                seen_point = true;
                continue;
            }
            if (c < '0' || c > '9') break;
            any_digit = true;
            if (leading && c == '0') {
                if (seen_point) --exp10;
                continue;
            }
            leading = false;
            if ((int)ndig < max_digits) {
                gmsg_big_mul_small_(&num, 10);
                gmsg_big_add_small_(&num, (uint32_t)(c - '0'));
                ++ndig;
                if (seen_point) --exp10;
            } else {
                if (c != '0') dropped_nonzero = true;
                if (!seen_point) ++exp10;
            }
        }
    }
    if (!any_digit) return GMSG_ERR_PARSE;
    /* exponent */
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1;
        bool neg = false;
        long long e = 0;
        if (j < n && (s[j] == '+' || s[j] == '-')) {
            neg = s[j] == '-';
            ++j;
        }
        if (j < n && s[j] >= '0' && s[j] <= '9') {
            for (; j < n && s[j] >= '0' && s[j] <= '9'; ++j)
                if (e < 100000000) e = e * 10 + (s[j] - '0');
            exp10 += neg ? -e : e;
            i = j;
        }
    }
    /* as std::from_chars: the valid prefix is converted first, so out of range wins over trailing characters */
    trailing = i != n;
    if (dropped_nonzero) {
        /* digits beyond max_digits only matter as a sticky bit: append a 1 */
        gmsg_big_mul_small_(&num, 10);
        gmsg_big_add_small_(&num, 1);
        ++ndig;
        --exp10;
    }
    if (gmsg_big_is_zero_(&num)) {
        if (trailing) return GMSG_ERR_PARSE;
        *bits = sign;
        return GMSG_OK;
    }
    {
        const long long lead10 = exp10 + (long long)ndig - 1; /* decimal exponent of the leading digit */
        bool overflow = false, sticky = false;
        uint64_t raw;
        if (lead10 > 310) return GMSG_ERR_OUT_OF_RANGE;
        if (lead10 < -400) return GMSG_ERR_OUT_OF_RANGE;
        if (exp10 >= 0) {
            int bl, e2 = 0;
            gmsg_big_mul_pow10_(&num, (int)exp10);
            bl = gmsg_big_bitlen_(&num);
            if (bl > 60) {
                e2 = bl - 60;
                gmsg_big_shr_(&num, e2, &sticky);
            }
            raw = gmsg_make_bits_(gmsg_big_low64_(&num), e2, sticky, f, &overflow);
        } else {
            /* q = floor(num * 2^sh / 10^-exp10) with p + 3 .. p + 5 bits */
            gmsg_big den;
            int sh, bit;
            uint64_t q = 0;
            gmsg_big_set_(&den, 1);
            gmsg_big_mul_pow10_(&den, (int)-exp10);
            sh = (f.p + 4) + gmsg_big_bitlen_(&den) - gmsg_big_bitlen_(&num);
            if (sh > 0) gmsg_big_shl_(&num, sh);
            else gmsg_big_shl_(&den, -sh);
            for (bit = f.p + 6; bit >= 0; --bit) {
                gmsg_big t;
                gmsg_big_copy_(&t, &den);
                gmsg_big_shl_(&t, bit);
                if (gmsg_big_cmp_(&num, &t) >= 0) {
                    gmsg_big_sub_(&num, &t);
                    q |= (uint64_t)1 << bit;
                }
            }
            sticky = !gmsg_big_is_zero_(&num);
            raw = gmsg_make_bits_(q, -sh, sticky, f, &overflow);
        }
        if (overflow || raw == 0) return GMSG_ERR_OUT_OF_RANGE;
        if (trailing) return GMSG_ERR_PARSE;
        *bits = sign | raw;
        return GMSG_OK;
    }
}

GMSG_API gmsg_error gmsg_parse_double(const char* s, size_t n, double* out) {
    uint64_t bits = 0;
    const gmsg_error e = gmsg_parse_bits_(s, n, false, &bits);
    if (e == GMSG_OK) *out = gmsg_f64_from_bits(bits);
    return e;
}

GMSG_API gmsg_error gmsg_parse_float(const char* s, size_t n, float* out) {
    uint64_t bits = 0;
    const gmsg_error e = gmsg_parse_bits_(s, n, true, &bits);
    if (e == GMSG_OK) *out = gmsg_f32_from_bits((uint32_t)bits);
    return e;
}

/* ---- formatting ----------------------------------------------------------------------------------------- */

typedef struct gmsg_out_ {
    char* p;
    size_t cap;
    size_t n;
    bool full;
} gmsg_out_;

GMSG_PRIV_ void gmsg_put_(gmsg_out_* o, char c) {
    if (o->n < o->cap) o->p[o->n] = c;
    else o->full = true;
    ++o->n;
}

GMSG_PRIV_ void gmsg_puts_(gmsg_out_* o, const char* s) {
    while (*s) gmsg_put_(o, *s++);
}

/* Decimal digits of a big integer (most significant first); returns the count. digits must have room for all of
 * them (309 for a double's integer part, GMSG_EXACT_DIGITS_ for gmsg_exact_digits_). Destroys a. */
GMSG_PRIV_ int gmsg_big_to_dec_(gmsg_big* a, char* digits) {
    int n = 0, i;
    if (gmsg_big_is_zero_(a)) {
        digits[0] = '0';
        return 1;
    }
    while (!gmsg_big_is_zero_(a)) {
        /* divide by 10^9; the remainder gives 9 digits (least significant first) */
        uint64_t rem = 0;
        uint32_t chunk;
        int k;
        for (i = a->n - 1; i >= 0; --i) {
            const uint64_t cur = (rem << 32) | a->w[i];
            a->w[i] = (uint32_t)(cur / 1000000000u);
            rem = cur % 1000000000u;
        }
        gmsg_big_trim_(a);
        chunk = (uint32_t)rem;
        for (k = 0; k < 9 && (chunk || !gmsg_big_is_zero_(a)); ++k) {
            digits[n++] = (char)('0' + chunk % 10u);
            chunk /= 10u;
        }
    }
    for (i = 0; i < n / 2; ++i) {
        const char t = digits[i];
        digits[i] = digits[n - 1 - i];
        digits[n - 1 - i] = t;
    }
    return n;
}

/* Split raw bits into sign, significand (with hidden bit) and exponent of the lsb. Returns 0 finite, 1 inf, 2 nan. */
GMSG_PRIV_ int gmsg_split_(uint64_t bits, gmsg_fmt_ f, bool* neg, uint64_t* m, int* e2) {
    const int expbits = f.p == 24 ? 8 : 11;
    const uint64_t frac = bits & (((uint64_t)1 << (f.p - 1)) - 1u);
    const int biased = (int)((bits >> (f.p - 1)) & (((uint64_t)1 << expbits) - 1u));
    *neg = ((bits >> (f.p - 1 + expbits)) & 1u) != 0;
    if (biased == f.emax) return frac ? 2 : 1;
    if (biased == 0) {
        *m = frac;
        *e2 = f.emin_lsb;
    } else {
        *m = frac | ((uint64_t)1 << (f.p - 1));
        *e2 = biased - f.bias - (f.p - 1);
    }
    return 0;
}

/* Fixed notation with `precision` decimals of the exact value m * 2^e2, rounded half to even. */
GMSG_PRIV_ void gmsg_fixed_(gmsg_out_* o, uint64_t m, int e2, int precision) {
    gmsg_big ip, frac, one;
    char idig[400];
    char fdig[512];
    int ni, k, fbits;
    bool round_up = false;
    /* integer part and fraction numerator over 2^fbits */
    gmsg_big_set_(&ip, m);
    frac.n = 0;
    fbits = 0;
    if (e2 >= 0) {
        gmsg_big_shl_(&ip, e2);
    } else {
        bool dummy = false;
        fbits = -e2;
        gmsg_big_set_(&frac, fbits >= 64 ? m : (m & (((uint64_t)1 << fbits) - 1u)));
        gmsg_big_shr_(&ip, fbits, &dummy);
    }
    gmsg_big_set_(&one, 1);
    gmsg_big_shl_(&one, fbits); /* 2^fbits */
    /* fractional digits: frac = frac * 10, digit = frac >> fbits */
    for (k = 0; k < precision && k < (int)sizeof fdig; ++k) {
        int d = 0;
        bool dummy = false;
        gmsg_big hi;
        gmsg_big_mul_small_(&frac, 10);
        gmsg_big_copy_(&hi, &frac);
        gmsg_big_shr_(&hi, fbits, &dummy);
        d = (int)gmsg_big_low64_(&hi);
        fdig[k] = (char)('0' + d);
        if (d) {
            gmsg_big_shl_(&hi, fbits);
            gmsg_big_sub_(&frac, &hi);
        }
    }
    /* rounding: compare the remainder with one half */
    if (!gmsg_big_is_zero_(&frac)) {
        gmsg_big twice;
        int c;
        gmsg_big_copy_(&twice, &frac);
        gmsg_big_shl_(&twice, 1);
        c = gmsg_big_cmp_(&twice, &one);
        if (c > 0) round_up = true;
        else if (c == 0) {
            const int last = precision > 0 ? fdig[precision - 1] - '0' : (int)(gmsg_big_low64_(&ip) & 1u);
            round_up = (last & 1) != 0;
        }
    }
    ni = gmsg_big_to_dec_(&ip, idig);
    if (round_up) {
        int i = precision - 1;
        while (i >= 0 && fdig[i] == '9') fdig[i--] = '0';
        if (i >= 0) {
            ++fdig[i];
        } else {
            int j = ni - 1;
            while (j >= 0 && idig[j] == '9') idig[j--] = '0';
            if (j >= 0) {
                ++idig[j];
            } else {
                for (j = ni; j > 0; --j) idig[j] = idig[j - 1];
                idig[0] = '1';
                ++ni;
            }
        }
    }
    for (k = 0; k < ni; ++k) gmsg_put_(o, idig[k]);
    if (precision > 0) {
        gmsg_put_(o, '.');
        for (k = 0; k < precision; ++k) gmsg_put_(o, fdig[k]);
    }
}

/* Shortest round-trip digits of m * 2^e2 (Burger & Dybvig free-format, closest, ties to even).
 * Writes the digits and returns their count; *k is the decimal exponent: value = 0.d1d2... * 10^k. */
GMSG_PRIV_ int gmsg_shortest_(uint64_t m, int e2, gmsg_fmt_ f, char* digits, int* k) {
    gmsg_big r, s, mp, mm, t;
    const bool even = (m & 1u) == 0;
    const bool unequal_gaps = m == ((uint64_t)1 << (f.p - 1)) && e2 > f.emin_lsb;
    int lead, kk, n = 0, bl = 0;
    uint64_t tm = m;
    while (tm) {
        ++bl;
        tm >>= 1;
    }
    if (e2 >= 0) {
        gmsg_big_set_(&r, m);
        gmsg_big_shl_(&r, e2 + (unequal_gaps ? 2 : 1));
        gmsg_big_set_(&s, unequal_gaps ? 4 : 2);
        gmsg_big_set_(&mp, 1);
        gmsg_big_shl_(&mp, e2 + (unequal_gaps ? 1 : 0));
        gmsg_big_set_(&mm, 1);
        gmsg_big_shl_(&mm, e2);
    } else {
        gmsg_big_set_(&r, m);
        gmsg_big_shl_(&r, unequal_gaps ? 2 : 1);
        gmsg_big_set_(&s, 1);
        gmsg_big_shl_(&s, -e2 + (unequal_gaps ? 2 : 1));
        gmsg_big_set_(&mp, unequal_gaps ? 2 : 1);
        gmsg_big_set_(&mm, 1);
    }
    /* estimate k = ceil(log10(v)) from the binary exponent of the leading bit */
    lead = e2 + bl - 1;
    {
        const long x = (long)lead * 78913L;
        const long fl = x >= 0 ? x / 262144L : -((-x + 262143L) / 262144L);
        kk = (int)fl + 1;
    }
    if (kk >= 0) gmsg_big_mul_pow10_(&s, kk);
    else {
        gmsg_big_mul_pow10_(&r, -kk);
        gmsg_big_mul_pow10_(&mp, -kk);
        gmsg_big_mul_pow10_(&mm, -kk);
    }
    /* fix the estimate: (r + m+) / s must lie in (1/10, 1] (open / closed as the boundaries are) */
    for (;;) {
        const int c = gmsg_big_cmp_sum_(&r, &mp, &s);
        if (even ? c >= 0 : c > 0) {
            gmsg_big_mul_small_(&s, 10);
            ++kk;
            continue;
        }
        gmsg_big_copy_(&t, &r);
        gmsg_big_add_(&t, &mp);
        gmsg_big_mul_small_(&t, 10);
        {
            const int c10 = gmsg_big_cmp_(&t, &s);
            if (even ? c10 < 0 : c10 <= 0) {
                gmsg_big_mul_small_(&r, 10);
                gmsg_big_mul_small_(&mp, 10);
                gmsg_big_mul_small_(&mm, 10);
                --kk;
                continue;
            }
        }
        break;
    }
    for (;;) {
        int d = 0, c;
        bool tc1, tc2;
        gmsg_big_mul_small_(&r, 10);
        gmsg_big_mul_small_(&mp, 10);
        gmsg_big_mul_small_(&mm, 10);
        while (gmsg_big_cmp_(&r, &s) >= 0) {
            gmsg_big_sub_(&r, &s);
            ++d;
        }
        c = gmsg_big_cmp_(&r, &mm);
        tc1 = even ? c <= 0 : c < 0;
        c = gmsg_big_cmp_sum_(&r, &mp, &s);
        tc2 = even ? c >= 0 : c > 0;
        if (!tc1 && !tc2) {
            digits[n++] = (char)('0' + d);
            if (n >= 40) break; /* cannot happen */
            continue;
        }
        if (tc1 && tc2) {
            gmsg_big_copy_(&t, &r);
            gmsg_big_shl_(&t, 1);
            c = gmsg_big_cmp_(&t, &s);
            if (c > 0 || (c == 0 && (d & 1))) ++d;
        } else if (tc2) {
            ++d;
        }
        digits[n++] = (char)('0' + d);
        break;
    }
    /* a final digit of 10 carries into the previous digits */
    {
        int i = n - 1;
        while (i > 0 && digits[i] > '9') {
            digits[i] = '0';
            ++digits[--i];
        }
        if (digits[0] > '9') {
            digits[0] = '1';
            for (i = 1; i < n; ++i) digits[i] = '0';
            ++kk;
        }
        while (n > 1 && digits[n - 1] == '0') --n;
    }
    *k = kk;
    return n;
}

/* Shortest representation as std::to_chars(value): fixed or scientific, whichever is shorter (fixed on a tie). */
GMSG_PRIV_ void gmsg_shortest_text_(gmsg_out_* o, uint64_t m, int e2, gmsg_fmt_ f) {
    char d[48];
    int k = 0, i;
    const int n = gmsg_shortest_(m, e2, f, d, &k);
    const int x = k - 1; /* scientific exponent */
    const int ax = x < 0 ? -x : x;
    const int sci_len = n + (n > 1 ? 1 : 0) + 2 + (ax >= 100 ? 3 : 2);
    int fixed_len;
    if (k <= 0) fixed_len = 2 - k + n;      /* 0.000ddd */
    else if (k < n) fixed_len = n + 1;      /* dd.ddd */
    else fixed_len = k;                     /* ddd000 */
    if (fixed_len <= sci_len) {
        if (k <= 0) {
            gmsg_puts_(o, "0.");
            for (i = 0; i < -k; ++i) gmsg_put_(o, '0');
            for (i = 0; i < n; ++i) gmsg_put_(o, d[i]);
        } else if (k < n) {
            for (i = 0; i < n; ++i) {
                if (i == k) gmsg_put_(o, '.');
                gmsg_put_(o, d[i]);
            }
        } else if (e2 > 0) {
            /* an integer whose neighbours are 2 or more apart: print its exact value (as to_chars), same length */
            gmsg_fixed_(o, m, e2, 0);
        } else {
            for (i = 0; i < n; ++i) gmsg_put_(o, d[i]);
            for (i = n; i < k; ++i) gmsg_put_(o, '0');
        }
        return;
    }
    gmsg_put_(o, d[0]);
    if (n > 1) {
        gmsg_put_(o, '.');
        for (i = 1; i < n; ++i) gmsg_put_(o, d[i]);
    }
    gmsg_put_(o, 'e');
    gmsg_put_(o, x < 0 ? '-' : '+');
    if (ax >= 100) gmsg_put_(o, (char)('0' + ax / 100));
    gmsg_put_(o, (char)('0' + ax / 10 % 10));
    gmsg_put_(o, (char)('0' + ax % 10));
}

/* Every decimal digit of the exact value m * 2^e2 (m > 0), without trailing zeros: digits[0..n), returned n, and
 * *k with value = 0.d1d2...dn * 10^k. A double needs up to 767 digits. */
#define GMSG_EXACT_DIGITS_ 800
GMSG_PRIV_ int gmsg_exact_digits_(uint64_t m, int e2, char* digits, int* k) {
    gmsg_big a;
    int n, scale = 0;
    gmsg_big_set_(&a, m);
    if (e2 >= 0) {
        gmsg_big_shl_(&a, e2);
    } else {
        /* m / 2^s = m * 5^s / 10^s */
        int left = -e2;
        scale = left;
        while (left >= 13) {
            gmsg_big_mul_small_(&a, 1220703125u); /* 5^13 */
            left -= 13;
        }
        while (left-- > 0) gmsg_big_mul_small_(&a, 5u);
    }
    n = gmsg_big_to_dec_(&a, digits);
    *k = n - scale;
    while (n > 1 && digits[n - 1] == '0') --n;
    return n;
}

/* printf "%.*g": `sig` significant digits (rounded half to even), fixed when -4 <= exponent < sig, otherwise
 * scientific; trailing zeros removed. */
GMSG_PRIV_ void gmsg_general_(gmsg_out_* o, uint64_t m, int e2, int sig) {
    char d[GMSG_EXACT_DIGITS_];
    int k = 1, n = 1, x, i;
    if (sig <= 0) sig = 1;
    if (m == 0) d[0] = '0';
    else n = gmsg_exact_digits_(m, e2, d, &k);
    if (n > sig) {
        bool up;
        if (d[sig] != '5') up = d[sig] > '5';
        else up = n > sig + 1 || ((d[sig - 1] - '0') & 1) != 0; /* digits after the 5 are not all zero, or a tie */
        n = sig;
        if (up) {
            i = sig - 1;
            while (i >= 0 && d[i] == '9') d[i--] = '0';
            if (i >= 0) {
                ++d[i];
            } else {
                d[0] = '1';
                ++k;
            }
        }
        while (n > 1 && d[n - 1] == '0') --n;
    }
    x = k - 1;
    if (x >= -4 && x < sig) {
        if (k <= 0) {
            gmsg_puts_(o, "0.");
            for (i = 0; i < -k; ++i) gmsg_put_(o, '0');
            for (i = 0; i < n; ++i) gmsg_put_(o, d[i]);
        } else {
            for (i = 0; i < n || i < k; ++i) {
                if (i == k) gmsg_put_(o, '.');
                gmsg_put_(o, i < n ? d[i] : '0');
            }
        }
        return;
    }
    gmsg_put_(o, d[0]);
    if (n > 1) {
        gmsg_put_(o, '.');
        for (i = 1; i < n; ++i) gmsg_put_(o, d[i]);
    }
    {
        const int ax = x < 0 ? -x : x;
        gmsg_put_(o, 'e');
        gmsg_put_(o, x < 0 ? '-' : '+');
        if (ax >= 100) gmsg_put_(o, (char)('0' + ax / 100));
        gmsg_put_(o, (char)('0' + ax / 10 % 10));
        gmsg_put_(o, (char)('0' + ax % 10));
    }
}

/* precision >= 0: fixed; precision < 0: shortest; general_digits > 0 (with precision < 0): %.*g. */
GMSG_PRIV_ gmsg_error gmsg_format_bits_(uint64_t bits, bool is_float, int precision, int general_digits, char* out, size_t cap,
                                      size_t* written) {
    const gmsg_fmt_ f = gmsg_fmt_of_(is_float);
    gmsg_out_ o;
    bool neg = false;
    uint64_t m = 0;
    int e2 = 0;
    const int kind = gmsg_split_(bits, f, &neg, &m, &e2);
    o.p = out;
    o.cap = cap;
    o.n = 0;
    o.full = false;
    if (neg) gmsg_put_(&o, '-');
    if (kind == 1) gmsg_puts_(&o, "inf");
    else if (kind == 2) gmsg_puts_(&o, "nan");
    else if (precision >= 0) gmsg_fixed_(&o, m, e2, precision);
    else if (general_digits > 0) gmsg_general_(&o, m, e2, general_digits);
    else if (m == 0) gmsg_put_(&o, '0');
    else gmsg_shortest_text_(&o, m, e2, f);
    if (o.full) return GMSG_ERR_BUFFER_FULL;
    *written = o.n;
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_format_double(double v, int precision, char* out, size_t cap, size_t* written) {
    return gmsg_format_bits_(gmsg_f64_to_bits(v), false, precision, 0, out, cap, written);
}

GMSG_API gmsg_error gmsg_format_general(double v, int digits, char* out, size_t cap, size_t* written) {
    return gmsg_format_bits_(gmsg_f64_to_bits(v), false, -1, digits < 1 ? 1 : digits, out, cap, written);
}

GMSG_API gmsg_error gmsg_format_float(float v, int precision, char* out, size_t cap, size_t* written) {
    if (precision >= 0) return gmsg_format_double((double)v, precision, out, cap, written); /* exact widening */
    return gmsg_format_bits_(gmsg_f32_to_bits(v), true, precision, 0, out, cap, written);
}
#elif GMSG_FLOAT_TEXT == GMSG_FLOAT_TEXT_LIBC

GMSG_API gmsg_error gmsg_format_double(double v, int precision, char* out, size_t cap, size_t* written) {
    int n;
    if (precision >= 0) {
        n = snprintf(out, cap, "%.*f", precision, v);
    } else {
        int p;
        char tmp[64];
        n = 0;
        for (p = 1; p <= 17; ++p) {
            n = snprintf(tmp, sizeof tmp, "%.*g", p, v);
            if (strtod(tmp, NULL) == v) break;
        }
        n = snprintf(out, cap, "%s", tmp);
    }
    if (n < 0) return GMSG_ERR_INVALID_VALUE;
    if ((size_t)n >= cap) return GMSG_ERR_BUFFER_FULL;
    *written = (size_t)n;
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_format_general(double v, int digits, char* out, size_t cap, size_t* written) {
    const int n = snprintf(out, cap, "%.*g", digits < 1 ? 1 : digits, v);
    if (n < 0) return GMSG_ERR_INVALID_VALUE;
    if ((size_t)n >= cap) return GMSG_ERR_BUFFER_FULL;
    *written = (size_t)n;
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_format_float(float v, int precision, char* out, size_t cap, size_t* written) {
    if (precision >= 0) return gmsg_format_double((double)v, precision, out, cap, written);
    {
        int p, n = 0;
        char tmp[48];
        for (p = 1; p <= 9; ++p) {
            n = snprintf(tmp, sizeof tmp, "%.*g", p, (double)v);
            if ((float)strtod(tmp, NULL) == v) break;
        }
        if (n < 0) return GMSG_ERR_INVALID_VALUE;
        if ((size_t)n >= cap) return GMSG_ERR_BUFFER_FULL;
        n = snprintf(out, cap, "%s", tmp);
        *written = (size_t)n;
        return GMSG_OK;
    }
}

GMSG_PRIV_ gmsg_error gmsg_parse_libc_(const char* s, size_t n, double* out) {
    char tmp[128];
    char* end = NULL;
    if (n == 0) return GMSG_ERR_MISSING_FIELD;
    if (s[0] == '+') {
        ++s;
        --n;
    }
    if (n == 0 || n >= sizeof tmp || s[0] == ' ' || s[0] == '+') return GMSG_ERR_PARSE;
    {
        size_t i;
        for (i = 0; i < n; ++i) tmp[i] = s[i];
        tmp[n] = '\0';
    }
    *out = strtod(tmp, &end);
    if (end != tmp + n) return GMSG_ERR_PARSE;
    return GMSG_OK;
}

GMSG_API gmsg_error gmsg_parse_double(const char* s, size_t n, double* out) { return gmsg_parse_libc_(s, n, out); }

GMSG_API gmsg_error gmsg_parse_float(const char* s, size_t n, float* out) {
    double d = 0;
    const gmsg_error e = gmsg_parse_libc_(s, n, &d);
    if (e == GMSG_OK) *out = (float)d;
    return e;
}
#endif /* GMSG_FLOAT_TEXT */

#endif /* implementation */

GMSG_C_END

#endif /* GMSG_C_FLOAT_TEXT_H */
