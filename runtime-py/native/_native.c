/* gmsg._native - optional accelerator of the Python runtime (runtime-py/gmsg), built from the C runtime (runtime-c).
 *
 * The pure-Python modules call these functions when the module is importable (gmsg/_accel.py); every function has a
 * pure-Python twin with the same results, and the tests run both. Only the CPython limited API is used (abi3,
 * Py_LIMITED_API 3.12), so one binary works with every CPython from 3.12 on.
 *
 * Strings are passed as str objects and read with PyUnicode_AsUTF8AndSize: the '#' formats of PyArg_ParseTuple depend
 * on PY_SSIZE_T_CLEAN handling that differs between the 3.12 and 3.13 headers of the stable ABI.
 *
 * Conventions: errors of the GMP kind are returned as an int error code (the Python side raises GmpError); Python
 * exceptions are raised only for misuse (wrong types, patterns longer than the C runtime supports). Configurations
 * (escape, framing) are built once and kept in capsules by the Python objects that own them.
 */
#define PY_SSIZE_T_CLEAN
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x030C0000
#endif
#include <Python.h>

#include <string.h>

#include "gmp/c/runtime.h"

/* ---- helpers -------------------------------------------------------------------------------------------------- */

/* Clamp [start, end) to the buffer (end < 0: the end of the buffer). */
static void clamp_range(Py_ssize_t len, Py_ssize_t* start, Py_ssize_t* end) {
    if (*end < 0 || *end > len) *end = len;
    if (*start < 0) *start = 0;
    if (*start > *end) *start = *end;
}

static const char ESCAPE_CAPSULE[] = "gmsg.escape_config";
static const char FIXED_CAPSULE[] = "gmsg.fixed_config";
static const char LENGTH_CAPSULE[] = "gmsg.length_prefixed_config";
static const char DELIMITED_CAPSULE[] = "gmsg.delimited_config";

static void free_capsule(PyObject* cap) {
    void* p = PyCapsule_GetPointer(cap, PyCapsule_GetName(cap));
    PyMem_Free(p);
}

static PyObject* new_capsule(const void* value, size_t size, const char* name) {
    void* p = PyMem_Malloc(size);
    PyObject* cap;
    if (!p) return PyErr_NoMemory();
    memcpy(p, value, size);
    cap = PyCapsule_New(p, name, free_capsule);
    if (!cap) PyMem_Free(p);
    return cap;
}

static const gmsg_escape_config* escape_of(PyObject* cap) {
    return (const gmsg_escape_config*)PyCapsule_GetPointer(cap, ESCAPE_CAPSULE);
}

static int to_pattern(const Py_buffer* b, gmsg_pattern* p) {
    if (b->len > (Py_ssize_t)sizeof p->bytes) {
        PyErr_SetString(PyExc_ValueError, "pattern longer than 8 bytes");
        return -1;
    }
    memset(p, 0, sizeof *p);
    memcpy(p->bytes, b->buf, (size_t)b->len);
    p->len = (uint8_t)b->len;
    return 0;
}

static PyObject* scan_result(gmsg_frame_scan s) {
    return Py_BuildValue("(innni)", (int)s.kind, (Py_ssize_t)s.size, (Py_ssize_t)s.consume,
                         (Py_ssize_t)s.consume_on_error, (int)s.error);
}

/* ---- checksums ------------------------------------------------------------------------------------------------ */

/* Table-driven CRC update for widths >= 8, with a few tables cached by (width, poly, refin). The state is the C
 * runtime's register (reflected when refin), as in the pure-Python implementation. */
typedef struct crc_table {
    unsigned width;
    uint64_t poly;
    int refin;
    uint64_t t[256];
} crc_table;

#define CRC_CACHE 8
static crc_table* crc_cache[CRC_CACHE];
static unsigned crc_next;

static uint64_t reflect(uint64_t v, unsigned w) {
    uint64_t r = 0;
    unsigned i;
    for (i = 0; i < w; ++i) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

static const crc_table* table_for(unsigned width, uint64_t poly, int refin) {
    unsigned i, k;
    crc_table* t;
    const uint64_t mask = width >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1);
    for (i = 0; i < CRC_CACHE; ++i)
        if (crc_cache[i] && crc_cache[i]->width == width && crc_cache[i]->poly == poly && crc_cache[i]->refin == refin)
            return crc_cache[i];
    t = crc_cache[crc_next];
    if (!t) {
        t = (crc_table*)PyMem_Malloc(sizeof *t);
        if (!t) return NULL;
        crc_cache[crc_next] = t;
    }
    crc_next = (crc_next + 1) % CRC_CACHE;
    t->width = width;
    t->poly = poly;
    t->refin = refin;
    if (refin) {
        const uint64_t rpoly = reflect(poly, width);
        for (i = 0; i < 256; ++i) {
            uint64_t c = i;
            for (k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ rpoly : c >> 1;
            t->t[i] = c;
        }
    } else {
        const uint64_t top = (uint64_t)1 << (width - 1);
        for (i = 0; i < 256; ++i) {
            uint64_t c = (uint64_t)i << (width - 8);
            for (k = 0; k < 8; ++k) c = (c & top) ? ((c << 1) ^ poly) & mask : (c << 1) & mask;
            t->t[i] = c;
        }
    }
    return t;
}

/* crc_update(width, poly, refin, state, data, start, end) -> state */
static PyObject* crc_update(PyObject* self, PyObject* args) {
    unsigned width;
    unsigned long long poly, state;
    int refin;
    Py_buffer b;
    Py_ssize_t start, end, i;
    uint64_t c;
    (void)self;
    if (!PyArg_ParseTuple(args, "IKpKy*nn", &width, &poly, &refin, &state, &b, &start, &end)) return NULL;
    if (width < 1 || width > 64) {
        PyBuffer_Release(&b);
        PyErr_SetString(PyExc_ValueError, "CRC width must be 1..64");
        return NULL;
    }
    clamp_range(b.len, &start, &end);
    c = state;
    if (width < 8) {
        gmsg_crc_params p;
        p.width = (uint8_t)width;
        p.refin = refin != 0;
        p.refout = false;
        p.poly = poly;
        p.init = 0;
        p.xorout = 0;
        c = gmsg_crc_update(&p, c, (const uint8_t*)b.buf + start, (size_t)(end - start));
    } else {
        const crc_table* t = table_for(width, poly, refin);
        const uint8_t* d = (const uint8_t*)b.buf;
        if (!t) {
            PyBuffer_Release(&b);
            return PyErr_NoMemory();
        }
        if (refin) {
            for (i = start; i < end; ++i) c = t->t[(c ^ d[i]) & 0xFFu] ^ (c >> 8);
        } else {
            const unsigned s = width - 8;
            const uint64_t mask = width >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1);
            for (i = start; i < end; ++i) c = (t->t[((c >> s) ^ d[i]) & 0xFFu] ^ (c << 8)) & mask;
        }
    }
    PyBuffer_Release(&b);
    return PyLong_FromUnsignedLongLong(c);
}

/* simple_update(kind, state, data, start, end) -> state; kind: xor8, sum8, lrc8, sum16, fletcher16, fletcher8_ubx */
static PyObject* simple_update(PyObject* self, PyObject* args) {
    int kind;
    unsigned long long state;
    Py_buffer b;
    Py_ssize_t start, end;
    uint64_t r = 0;
    const uint8_t* d;
    size_t n;
    (void)self;
    if (!PyArg_ParseTuple(args, "iKy*nn", &kind, &state, &b, &start, &end)) return NULL;
    clamp_range(b.len, &start, &end);
    d = (const uint8_t*)b.buf + start;
    n = (size_t)(end - start);
    switch (kind) {
        case 0: r = gmsg_xor8_update((uint8_t)state, d, n); break;
        case 1: r = gmsg_sum8_update((uint8_t)state, d, n); break;
        case 2: r = gmsg_lrc8_update((uint8_t)state, d, n); break;
        case 3: r = gmsg_sum16_update((uint16_t)state, d, n); break;
        case 4: r = gmsg_fletcher16_update((uint16_t)state, d, n); break;
        case 5: r = gmsg_fletcher8_ubx_update((uint16_t)state, d, n); break;
        default:
            PyBuffer_Release(&b);
            PyErr_SetString(PyExc_ValueError, "unknown checksum kind");
            return NULL;
    }
    PyBuffer_Release(&b);
    return PyLong_FromUnsignedLongLong(r);
}

/* ---- numbers and text ----------------------------------------------------------------------------------------- */

/* parse_int(s, base, signed, lo, hi) -> int, or (error,) */
static PyObject* parse_int(PyObject* self, PyObject* args) {
    const char* s;
    Py_ssize_t n;
    int base, is_signed;
    PyObject *str, *lo, *hi;
    gmsg_error e;
    (void)self;
    if (!PyArg_ParseTuple(args, "UipOO", &str, &base, &is_signed, &lo, &hi)) return NULL;
    if (!(s = PyUnicode_AsUTF8AndSize(str, &n))) return NULL;
    if (is_signed) {
        int64_t v = 0;
        const long long mn = PyLong_AsLongLong(lo), mx = PyLong_AsLongLong(hi);
        if (PyErr_Occurred()) return NULL;
        e = gmsg_parse_i64(s, (size_t)n, base, mn, mx, &v);
        if (e != GMSG_OK) return Py_BuildValue("(i)", (int)e);
        return PyLong_FromLongLong(v);
    } else {
        uint64_t v = 0;
        const unsigned long long mx = PyLong_AsUnsignedLongLong(hi);
        if (PyErr_Occurred()) return NULL;
        e = gmsg_parse_u64(s, (size_t)n, base, mx, &v);
        if (e != GMSG_OK) return Py_BuildValue("(i)", (int)e);
        return PyLong_FromUnsignedLongLong(v);
    }
}

/* parse_float(s, is_f32) -> float, or error (int) */
static PyObject* parse_float(PyObject* self, PyObject* args) {
    const char* s;
    Py_ssize_t n;
    int is_f32;
    PyObject* str;
    gmsg_error e;
    (void)self;
    if (!PyArg_ParseTuple(args, "Up", &str, &is_f32)) return NULL;
    if (!(s = PyUnicode_AsUTF8AndSize(str, &n))) return NULL;
    if (is_f32) {
        float f = 0;
        e = gmsg_parse_float(s, (size_t)n, &f);
        if (e != GMSG_OK) return PyLong_FromLong((long)e);
        return PyFloat_FromDouble((double)f);
    } else {
        double d = 0;
        e = gmsg_parse_double(s, (size_t)n, &d);
        if (e != GMSG_OK) return PyLong_FromLong((long)e);
        return PyFloat_FromDouble(d);
    }
}

static PyObject* text_result(gmsg_error e, const char* out, size_t written) {
    if (e != GMSG_OK) return PyLong_FromLong((long)e);
    return PyUnicode_FromStringAndSize(out, (Py_ssize_t)written);
}

/* format_float(x, precision, is_f32) -> str, or error (int) */
static PyObject* format_float(PyObject* self, PyObject* args) {
    double x;
    int precision, is_f32;
    size_t cap, written = 0;
    char small[64];
    char* out = small;
    gmsg_error e;
    PyObject* r;
    (void)self;
    if (!PyArg_ParseTuple(args, "dip", &x, &precision, &is_f32)) return NULL;
    cap = precision < 0 ? sizeof small : 340u + (size_t)precision;
    if (cap > sizeof small) {
        out = (char*)PyMem_Malloc(cap);
        if (!out) return PyErr_NoMemory();
    }
    if (is_f32 && precision < 0) e = gmsg_format_float((float)x, precision, out, cap, &written);
    else e = gmsg_format_double(x, precision, out, cap, &written);
    r = text_result(e, out, written);
    if (out != small) PyMem_Free(out);
    return r;
}

/* format_general(x, digits) -> str */
static PyObject* format_general(PyObject* self, PyObject* args) {
    double x;
    int digits;
    size_t cap, written = 0;
    char* out;
    gmsg_error e;
    PyObject* r;
    (void)self;
    if (!PyArg_ParseTuple(args, "di", &x, &digits)) return NULL;
    cap = 64u + (size_t)(digits > 0 ? digits : 0) + 340u;
    out = (char*)PyMem_Malloc(cap);
    if (!out) return PyErr_NoMemory();
    e = gmsg_format_general(x, digits, out, cap, &written);
    r = text_result(e, out, written);
    PyMem_Free(out);
    return r;
}

/* ---- byte stuffing -------------------------------------------------------------------------------------------- */

/* escape_config(enabled, esc, is_table, xor_mask, raw, code) -> capsule */
static PyObject* escape_config(PyObject* self, PyObject* args) {
    int enabled, is_table;
    unsigned char esc, xor_mask;
    Py_buffer raw, code;
    gmsg_escape_config c;
    Py_ssize_t i;
    (void)self;
    if (!PyArg_ParseTuple(args, "pbpby*y*", &enabled, &esc, &is_table, &xor_mask, &raw, &code)) return NULL;
    memset(&c, 0, sizeof c);
    c.enabled = enabled != 0;
    c.esc = esc;
    c.style = (uint8_t)(is_table ? GMSG_ESCAPE_TABLE : GMSG_ESCAPE_XOR);
    c.xor_mask = xor_mask;
    c.count = (uint8_t)(raw.len < GMSG_ESCAPE_MAX_RESERVED ? raw.len : GMSG_ESCAPE_MAX_RESERVED);
    for (i = 0; i < c.count; ++i) {
        c.raw[i] = ((const uint8_t*)raw.buf)[i];
        c.code[i] = i < code.len ? ((const uint8_t*)code.buf)[i] : 0;
    }
    if (c.enabled) {
        c.mask[esc >> 5] |= 1u << (esc & 31u);
        for (i = 0; i < c.count; ++i) c.mask[c.raw[i] >> 5] |= 1u << (c.raw[i] & 31u);
    }
    PyBuffer_Release(&raw);
    PyBuffer_Release(&code);
    return new_capsule(&c, sizeof c, ESCAPE_CAPSULE);
}

/* walk_escaped(cfg, buf, at, n, pick_at, pick or None) -> (kind, wire, error) */
static PyObject* walk_escaped(PyObject* self, PyObject* args) {
    PyObject *cap, *pick_obj;
    Py_buffer b, pick;
    Py_ssize_t at, n, pick_at;
    const gmsg_escape_config* c;
    gmsg_escape_walk w;
    int has_pick = 0;
    (void)self;
    if (!PyArg_ParseTuple(args, "Oy*nnnO", &cap, &b, &at, &n, &pick_at, &pick_obj)) return NULL;
    c = escape_of(cap);
    if (!c) {
        PyBuffer_Release(&b);
        return NULL;
    }
    if (pick_obj != Py_None) {
        if (PyObject_GetBuffer(pick_obj, &pick, PyBUF_WRITABLE) < 0) {
            PyBuffer_Release(&b);
            return NULL;
        }
        has_pick = 1;
    }
    if (n < 0) n = 0;
    if (at < 0) at = 0;
    if (pick_at < 0) pick_at = 0;
    w = gmsg_walk_escaped((const uint8_t*)b.buf, (size_t)b.len, (size_t)at, (size_t)n, c, (size_t)pick_at,
                          has_pick ? (uint8_t*)pick.buf : NULL, has_pick ? (size_t)pick.len : 0);
    if (has_pick) PyBuffer_Release(&pick);
    PyBuffer_Release(&b);
    return Py_BuildValue("(ini)", (int)w.kind, (Py_ssize_t)w.wire, (int)w.error);
}

/* unescape(cfg, wire) -> bytes, or error (int) */
static PyObject* unescape(PyObject* self, PyObject* args) {
    PyObject *cap, *r;
    Py_buffer b;
    const gmsg_escape_config* c;
    uint8_t* copy;
    size_t out_n = 0;
    gmsg_error e;
    (void)self;
    if (!PyArg_ParseTuple(args, "Oy*", &cap, &b)) return NULL;
    c = escape_of(cap);
    if (!c) {
        PyBuffer_Release(&b);
        return NULL;
    }
    copy = (uint8_t*)PyMem_Malloc(b.len ? (size_t)b.len : 1u);
    if (!copy) {
        PyBuffer_Release(&b);
        return PyErr_NoMemory();
    }
    memcpy(copy, b.buf, (size_t)b.len);
    e = gmsg_unescape_in_place(copy, (size_t)b.len, c, &out_n);
    r = e != GMSG_OK ? PyLong_FromLong((long)e) : PyBytes_FromStringAndSize((const char*)copy, (Py_ssize_t)out_n);
    PyMem_Free(copy);
    PyBuffer_Release(&b);
    return r;
}

/* escape_range(cfg, data, at, length) -> bytes: a copy of data with data[at:at + length] escaped */
static PyObject* escape_range(PyObject* self, PyObject* args) {
    PyObject *cap, *r;
    Py_buffer b;
    Py_ssize_t at, length, i, k = 0;
    const gmsg_escape_config* c;
    const uint8_t* d;
    uint8_t* out;
    (void)self;
    if (!PyArg_ParseTuple(args, "Oy*nn", &cap, &b, &at, &length)) return NULL;
    c = escape_of(cap);
    if (!c) {
        PyBuffer_Release(&b);
        return NULL;
    }
    if (at < 0) at = 0;
    if (at > b.len) at = b.len;
    if (length < 0) length = 0;
    if (length > b.len - at) length = b.len - at;
    d = (const uint8_t*)b.buf;
    out = (uint8_t*)PyMem_Malloc((size_t)(b.len + length) + 1u);
    if (!out) {
        PyBuffer_Release(&b);
        return PyErr_NoMemory();
    }
    memcpy(out, d, (size_t)at);
    k = at;
    for (i = at; i < at + length; ++i) {
        if (c->enabled && gmsg_escape_needs(c, d[i])) {
            out[k++] = c->esc;
            out[k++] = gmsg_escape_encode(c, d[i]);
        } else {
            out[k++] = d[i];
        }
    }
    memcpy(out + k, d + at + length, (size_t)(b.len - at - length));
    k += b.len - at - length;
    r = PyBytes_FromStringAndSize((const char*)out, k);
    PyMem_Free(out);
    PyBuffer_Release(&b);
    return r;
}

/* ---- frame scanners ------------------------------------------------------------------------------------------- */

/* fixed_config(sync, frame_size, escape, suffix_len) -> capsule */
static PyObject* fixed_config(PyObject* self, PyObject* args) {
    Py_buffer sync;
    Py_ssize_t frame_size, suffix_len;
    PyObject* esc;
    const gmsg_escape_config* e;
    gmsg_fixed_config c;
    (void)self;
    if (!PyArg_ParseTuple(args, "y*nOn", &sync, &frame_size, &esc, &suffix_len)) return NULL;
    memset(&c, 0, sizeof c);
    if (to_pattern(&sync, &c.sync) < 0 || !(e = escape_of(esc))) {
        PyBuffer_Release(&sync);
        return NULL;
    }
    PyBuffer_Release(&sync);
    c.frame_size = (size_t)frame_size;
    c.escape = *e;
    c.suffix_len = (size_t)suffix_len;
    return new_capsule(&c, sizeof c, FIXED_CAPSULE);
}

/* length_prefixed_config(sync, length_offset, length_size, little, add, min_frame, max_frame, escape, suffix_len,
 *                        counts_wire, header_len, trailer_len, wire_includes_trailer) -> capsule */
static PyObject* length_prefixed_config(PyObject* self, PyObject* args) {
    Py_buffer sync;
    Py_ssize_t length_offset, min_frame, max_frame, suffix_len, header_len, trailer_len;
    int length_size, little, counts_wire, wire_includes_trailer;
    long long add;
    PyObject* esc;
    const gmsg_escape_config* e;
    gmsg_length_prefixed_config c;
    (void)self;
    if (!PyArg_ParseTuple(args, "y*nipLnnOnpnnp", &sync, &length_offset, &length_size, &little, &add, &min_frame,
                          &max_frame, &esc, &suffix_len, &counts_wire, &header_len, &trailer_len,
                          &wire_includes_trailer))
        return NULL;
    memset(&c, 0, sizeof c);
    if (to_pattern(&sync, &c.sync) < 0 || !(e = escape_of(esc))) {
        PyBuffer_Release(&sync);
        return NULL;
    }
    PyBuffer_Release(&sync);
    if (length_size < 1 || length_size > 8) {
        PyErr_SetString(PyExc_ValueError, "length size must be 1..8");
        return NULL;
    }
    c.length_offset = (size_t)length_offset;
    c.length_size = (uint8_t)length_size;
    c.length_endian = little ? GMSG_ENDIAN_LITTLE : GMSG_ENDIAN_BIG;
    c.add = (int64_t)add;
    c.min_frame = (size_t)min_frame;
    c.max_frame = (size_t)max_frame;
    c.escape = *e;
    c.suffix_len = (size_t)suffix_len;
    c.counts_wire = counts_wire != 0;
    c.header_len = (size_t)header_len;
    c.trailer_len = (size_t)trailer_len;
    c.wire_includes_trailer = wire_includes_trailer != 0;
    return new_capsule(&c, sizeof c, LENGTH_CAPSULE);
}

/* delimited_config(start, end, escape, max_frame) -> capsule */
static PyObject* delimited_config(PyObject* self, PyObject* args) {
    Py_buffer start, end;
    Py_ssize_t max_frame;
    PyObject* esc;
    const gmsg_escape_config* e;
    gmsg_delimited_config c;
    int bad;
    (void)self;
    if (!PyArg_ParseTuple(args, "y*y*On", &start, &end, &esc, &max_frame)) return NULL;
    memset(&c, 0, sizeof c);
    bad = to_pattern(&start, &c.start) < 0 || to_pattern(&end, &c.end) < 0 || !(e = escape_of(esc));
    PyBuffer_Release(&start);
    PyBuffer_Release(&end);
    if (bad) return NULL;
    c.escape = *e;
    c.max_frame = (size_t)max_frame;
    return new_capsule(&c, sizeof c, DELIMITED_CAPSULE);
}

#define SCANNER_(fn, type, capsule)                                                                    \
    static PyObject* fn(PyObject* self, PyObject* args) {                                              \
        PyObject* cap;                                                                                 \
        Py_buffer b;                                                                                   \
        const type* c;                                                                                 \
        gmsg_frame_scan s;                                                                             \
        (void)self;                                                                                    \
        if (!PyArg_ParseTuple(args, "Oy*", &cap, &b)) return NULL;                                     \
        c = (const type*)PyCapsule_GetPointer(cap, capsule);                                           \
        if (!c) {                                                                                      \
            PyBuffer_Release(&b);                                                                      \
            return NULL;                                                                               \
        }                                                                                              \
        s = gmsg_##fn((const uint8_t*)b.buf, (size_t)b.len, c);                                        \
        PyBuffer_Release(&b);                                                                          \
        return scan_result(s);                                                                         \
    }

SCANNER_(scan_fixed, gmsg_fixed_config, FIXED_CAPSULE)
SCANNER_(scan_length_prefixed, gmsg_length_prefixed_config, LENGTH_CAPSULE)
SCANNER_(scan_delimited, gmsg_delimited_config, DELIMITED_CAPSULE)

/* ---- module --------------------------------------------------------------------------------------------------- */

static PyMethodDef methods[] = {
    {"crc_update", crc_update, METH_VARARGS, "crc_update(width, poly, refin, state, data, start, end) -> state"},
    {"simple_update", simple_update, METH_VARARGS, "simple_update(kind, state, data, start, end) -> state"},
    {"parse_int", parse_int, METH_VARARGS, "parse_int(s, base, signed, lo, hi) -> int or (error,)"},
    {"parse_float", parse_float, METH_VARARGS, "parse_float(s, is_f32) -> float or error"},
    {"format_float", format_float, METH_VARARGS, "format_float(x, precision, is_f32) -> str or error"},
    {"format_general", format_general, METH_VARARGS, "format_general(x, digits) -> str or error"},
    {"escape_config", escape_config, METH_VARARGS, "escape_config(enabled, esc, is_table, xor_mask, raw, code)"},
    {"walk_escaped", walk_escaped, METH_VARARGS, "walk_escaped(cfg, buf, at, n, pick_at, pick) -> (kind, wire, error)"},
    {"unescape", unescape, METH_VARARGS, "unescape(cfg, wire) -> bytes or error"},
    {"escape_range", escape_range, METH_VARARGS, "escape_range(cfg, data, at, length) -> bytes"},
    {"fixed_config", fixed_config, METH_VARARGS, "fixed_config(sync, frame_size, escape, suffix_len)"},
    {"length_prefixed_config", length_prefixed_config, METH_VARARGS, "length_prefixed_config(...)"},
    {"delimited_config", delimited_config, METH_VARARGS, "delimited_config(start, end, escape, max_frame)"},
    {"scan_fixed", scan_fixed, METH_VARARGS, "scan_fixed(cfg, buf) -> (kind, size, consume, consume_on_error, error)"},
    {"scan_length_prefixed", scan_length_prefixed, METH_VARARGS, "scan_length_prefixed(cfg, buf) -> scan tuple"},
    {"scan_delimited", scan_delimited, METH_VARARGS, "scan_delimited(cfg, buf) -> scan tuple"},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "_native", "Accelerator of the gmsg runtime, built from the GMP C runtime.", -1, methods,
    NULL, NULL, NULL, NULL,
};

PyMODINIT_FUNC PyInit__native(void) { return PyModule_Create(&module); }
