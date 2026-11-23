/* gmsg.js - the GMP runtime for JavaScript (used by `gmpc --lang js` output and by Schema Studio).
 *
 * A port of the C++ runtime (runtime/include/gmp) with the same behaviour and the same text output, written the way
 * JavaScript code is normally written:
 *   - errors are thrown as `GmpError` (`.code` is one of `gmsg.Err`, `.message` the same text as gmp::to_string)
 *   - bytes are Uint8Array; strings are JS strings with one character per byte (latin1), so they round-trip exactly
 *   - integers are Numbers, except 64-bit types (u64 / i64), which are BigInt
 *   - encoders return a new Uint8Array (no caller buffer, no BufferFull); printing returns a string
 *   - the stream parser takes listeners (`on(name, fn)`) and has a pull API with iterators; polled views own a copy of
 *     their frame, so they never expire
 *
 * Loads as a CommonJS module (`require('./gmsg.js')`), as an ES module import through a bundler or Node's CommonJS
 * interop, or as a classic <script> (global `gmsg`), so pages opened from file:// work too. No dependencies.
 */
(function (root, factory) {
  'use strict';
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.gmsg = factory();
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  // ================================================================================================================
  // errors (gmp/error.hpp)
  // ================================================================================================================
  /** Error codes, same values and order as gmp::Error / gmsg_error. */
  const Err = Object.freeze({
    None: 0, Truncated: 1, BadSync: 2, BadChecksum: 3, BadLength: 4, BadDelimiter: 5, BadConst: 6, OutOfRange: 7,
    UnknownType: 8, BufferFull: 9, ParseError: 10, MissingField: 11, InvalidValue: 12, Custom: 13, UnknownEnum: 14,
    ValueOutOfLimits: 15,
  });
  const ERR_TEXT = ['none', 'truncated', 'bad sync', 'bad checksum', 'bad length', 'bad delimiter', 'bad constant',
    'out of range', 'unknown message type', 'buffer full', 'parse error', 'missing field', 'invalid value',
    'custom error', 'unknown enum value', 'value out of limits'];
  const ERR_NAME = Object.keys(Err);

  /** Text of an error code (gmp::to_string(Error)). */
  const errorText = (code) => ERR_TEXT[code] || 'unknown';
  /** Name of an error code ('BadChecksum'). */
  const errorName = (code) => ERR_NAME[code] || 'Unknown';

  /** Thrown by decoders, encoders and parsers. `code` is a gmsg.Err value; `bytes` (stream errors) the dropped bytes. */
  class GmpError extends Error {
    constructor(code, detail) {
      super(detail ? `${errorText(code)}: ${detail}` : errorText(code));
      this.name = 'GmpError';
      this.code = code;
    }
    /** gmp::to_string text of the code, without the detail. */
    get text() { return errorText(this.code); }
  }
  const fail = (code, detail) => { throw new GmpError(code, detail); };

  // ================================================================================================================
  // byte helpers, endian (gmp/endian.hpp)
  // ================================================================================================================
  const BIG = 'big', LITTLE = 'little';
  const MSB_FIRST = 'msb_first', LSB_FIRST = 'lsb_first';
  const isLittle = (e) => e === LITTLE;

  /** Accept Uint8Array, ArrayBuffer, DataView, Buffer or an array of numbers. */
  function toBytes(b) {
    if (b instanceof Uint8Array) return b;
    if (b instanceof ArrayBuffer) return new Uint8Array(b);
    if (ArrayBuffer.isView(b)) return new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
    if (typeof b === 'string') return latin1Bytes(b);
    if (Array.isArray(b)) return Uint8Array.from(b);
    fail(Err.InvalidValue, 'expected bytes');
  }
  const viewOf = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);

  /** JS string <-> bytes, one character per byte (latin1). */
  function latin1Bytes(s) {
    const out = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c > 255) fail(Err.InvalidValue, `character U+${c.toString(16).toUpperCase().padStart(4, '0')} is not a byte`);
      out[i] = c;
    }
    return out;
  }
  function latin1String(b, start = 0, end = b.length) {
    let s = '';
    for (let i = start; i < end; i += 4096) s += String.fromCharCode.apply(null, b.subarray(i, Math.min(end, i + 4096)));
    return s;
  }

  /** Unsigned integer of n (1..8) bytes as a Number (exact up to 2^53; used for length fields). */
  function loadUint(p, at, n, endian) {
    let v = 0;
    if (isLittle(endian)) for (let i = n - 1; i >= 0; i--) v = v * 256 + p[at + i];
    else for (let i = 0; i < n; i++) v = v * 256 + p[at + i];
    return v;
  }
  /** Store an unsigned integer (Number or BigInt) of n bytes. */
  function storeUint(p, at, n, v, endian) {
    let x = BigInt.asUintN(64, BigInt(v));
    for (let k = 0; k < n; k++) {
      const i = isLittle(endian) ? at + k : at + n - 1 - k;
      p[i] = Number(x & 0xFFn);
      x >>= 8n;
    }
  }

  /** Unsigned integer of n (1..8) bytes as a BigInt (checksums of any width). */
  function loadUintBig(p, at, n, endian) {
    let v = 0n;
    if (isLittle(endian)) for (let i = n - 1; i >= 0; i--) v = (v << 8n) | BigInt(p[at + i]);
    else for (let i = 0; i < n; i++) v = (v << 8n) | BigInt(p[at + i]);
    return v;
  }
  /** A value of primitive type t ('u16', 'f32', 'bool', 'char', ...) at p[at] (gmp::load). */
  function load(p, at, t, endian) {
    if (p.length - at < SIZE[t] || at < 0) fail(Err.Truncated);
    const dv = viewOf(p), le = isLittle(endian);
    switch (t) {
      case 'u8': return p[at];
      case 'i8': return dv.getInt8(at);
      case 'bool': return p[at] !== 0;
      case 'char': return String.fromCharCode(p[at]);
      case 'u16': return dv.getUint16(at, le);
      case 'i16': return dv.getInt16(at, le);
      case 'u32': return dv.getUint32(at, le);
      case 'i32': return dv.getInt32(at, le);
      case 'u64': return dv.getBigUint64(at, le);
      case 'i64': return dv.getBigInt64(at, le);
      case 'f32': return dv.getFloat32(at, le);
      case 'f64': return dv.getFloat64(at, le);
      default: return fail(Err.InvalidValue, `unknown type ${t}`);
    }
  }

  // ---- integer types ----------------------------------------------------------------------------------------------
  const INT_TYPES = {
    u8: [8, false], u16: [16, false], u32: [32, false], u64: [64, false],
    i8: [8, true], i16: [16, true], i32: [32, true], i64: [64, true],
  };
  const SIZE = { u8: 1, u16: 2, u32: 4, u64: 8, i8: 1, i16: 2, i32: 4, i64: 8, f32: 4, f64: 8, bool: 1, char: 1 };
  const is64 = (t) => t === 'u64' || t === 'i64';

  /** Check that v is a valid value of integer type t (Number, or BigInt for 64-bit types); returns it. */
  function checkInt(v, t, what) {
    const [bits, signed] = INT_TYPES[t];
    if (bits === 64) {
      if (typeof v !== 'bigint') {
        if (typeof v === 'number' && Number.isInteger(v)) v = BigInt(v);
        else fail(Err.OutOfRange, `${what || 'value'} must be a BigInt (${t})`);
      }
      if ((signed ? BigInt.asIntN(64, v) : BigInt.asUintN(64, v)) !== v) fail(Err.OutOfRange, `${what || 'value'} ${v} does not fit ${t}`);
      return v;
    }
    if (typeof v === 'bigint') v = Number(v);
    if (typeof v !== 'number' || !Number.isInteger(v)) fail(Err.OutOfRange, `${what || 'value'} must be an integer (${t})`);
    const lo = signed ? -(2 ** (bits - 1)) : 0, hi = signed ? 2 ** (bits - 1) - 1 : 2 ** bits - 1;
    if (v < lo || v > hi) fail(Err.OutOfRange, `${what || 'value'} ${v} does not fit ${t}`);
    return v;
  }
  /** A field value as a 64-bit signed BigInt, as gmp::to_int does for generated expressions: bool -> 0 / 1, char ->
   *  its (signed) character code, floats truncated toward zero. */
  function toInt(v) {
    if (typeof v === 'bigint') return BigInt.asIntN(64, v);
    if (typeof v === 'boolean') return v ? 1n : 0n;
    if (typeof v === 'string') { const c = v.length ? v.charCodeAt(0) & 0xFF : 0; return BigInt(c > 127 ? c - 256 : c); }
    if (typeof v !== 'number' || !Number.isFinite(v)) return 0n;
    return BigInt.asIntN(64, BigInt(Math.trunc(v)));
  }
  /** Round half away from zero (std::llround), as a BigInt. */
  const llround = (x) => (Number.isFinite(x) ? BigInt(Math.sign(x) * Math.round(Math.abs(x))) : 0n);
  /** Integer division / remainder of generated expressions (int64, truncating); division by zero fails. */
  function idiv(a, b) { if (b === 0n) fail(Err.InvalidValue, 'division by zero'); return BigInt.asIntN(64, a / b); }
  function imod(a, b) { if (b === 0n) fail(Err.InvalidValue, 'division by zero'); return a % b; }
  /** Trailing pad characters removed (gmp::strip_padding). */
  function stripPadding(s, pad) {
    let e = s.length;
    while (e > 0 && s.charCodeAt(e - 1) === pad) e--;
    return s.slice(0, e);
  }

  /** Wrap an integer to type t (C++ static_cast semantics): Number, or BigInt for 64-bit types. */
  function wrapInt(v, t) {
    const [bits, signed] = INT_TYPES[t];
    const b = typeof v === 'bigint' ? v : BigInt(Math.trunc(Number(v)));
    const w = signed ? BigInt.asIntN(bits, b) : BigInt.asUintN(bits, b);
    return bits === 64 ? w : Number(w);
  }

  // ================================================================================================================
  // Reader / Writer (gmp/byte_reader.hpp, byte_writer.hpp)
  // ================================================================================================================
  /** Sequential reader over bytes[start, end). Reading past the end throws GmpError(Truncated). */
  class Reader {
    constructor(bytes, start = 0, end) {
      this.bytes = toBytes(bytes);
      this.pos = start;
      this.end = end === undefined ? this.bytes.length : end;
      this.dv = viewOf(this.bytes);
    }
    get remaining() { return this.end - this.pos; }
    need(n) { if (this.end - this.pos < n) fail(Err.Truncated); }
    skip(n) { this.need(n); this.pos += n; }
    u8() { this.need(1); return this.bytes[this.pos++]; }
    i8() { this.need(1); return this.dv.getInt8(this.pos++); }
    bool() { return this.u8() !== 0; }
    char() { return String.fromCharCode(this.u8()); }
    u16(e) { this.need(2); const v = this.dv.getUint16(this.pos, isLittle(e)); this.pos += 2; return v; }
    i16(e) { this.need(2); const v = this.dv.getInt16(this.pos, isLittle(e)); this.pos += 2; return v; }
    u32(e) { this.need(4); const v = this.dv.getUint32(this.pos, isLittle(e)); this.pos += 4; return v; }
    i32(e) { this.need(4); const v = this.dv.getInt32(this.pos, isLittle(e)); this.pos += 4; return v; }
    u64(e) { this.need(8); const v = this.dv.getBigUint64(this.pos, isLittle(e)); this.pos += 8; return v; }
    i64(e) { this.need(8); const v = this.dv.getBigInt64(this.pos, isLittle(e)); this.pos += 8; return v; }
    f32(e) { this.need(4); const v = this.dv.getFloat32(this.pos, isLittle(e)); this.pos += 4; return v; }
    f64(e) { this.need(8); const v = this.dv.getFloat64(this.pos, isLittle(e)); this.pos += 8; return v; }
    /** Read a value of primitive type t ('u16', 'f32', 'bool', ...). */
    read(t, e) { return this[t](e); }
    /** n bytes (a copy). */
    take(n) { this.need(n); const b = this.bytes.slice(this.pos, this.pos + n); this.pos += n; return b; }
    /** n bytes without copying. */
    view(n) { this.need(n); const b = this.bytes.subarray(this.pos, this.pos + n); this.pos += n; return b; }
    /** The bytes before the next `terminator` (a copy); the terminator is consumed. Truncated when there is none. */
    readUntil(terminator) {
      for (let i = this.pos; i < this.end; i++) {
        if (this.bytes[i] === terminator) { const b = this.bytes.slice(this.pos, i); this.pos = i + 1; return b; }
      }
      return fail(Err.Truncated);
    }
  }

  /** Growable writer. Values are range-checked against their type (GmpError OutOfRange). */
  class Writer {
    constructor(capacity = 64) {
      this.buf = new Uint8Array(Math.max(8, capacity));
      this.dv = viewOf(this.buf);
      this.pos = 0;
    }
    ensure(n) {
      if (this.pos + n <= this.buf.length) return;
      let cap = this.buf.length * 2;
      while (cap < this.pos + n) cap *= 2;
      const nb = new Uint8Array(cap);
      nb.set(this.buf.subarray(0, this.pos));
      this.buf = nb;
      this.dv = viewOf(nb);
    }
    u8(v) { v = checkInt(v, 'u8'); this.ensure(1); this.buf[this.pos++] = v; }
    i8(v) { v = checkInt(v, 'i8'); this.ensure(1); this.dv.setInt8(this.pos++, v); }
    bool(v) { this.ensure(1); this.buf[this.pos++] = v ? 1 : 0; }
    char(v) {
      const c = typeof v === 'number' ? v : String(v).charCodeAt(0);
      if (typeof v === 'string' && v.length !== 1) fail(Err.OutOfRange, 'char must be one character');
      this.u8(c);
    }
    u16(v, e) { v = checkInt(v, 'u16'); this.ensure(2); this.dv.setUint16(this.pos, v, isLittle(e)); this.pos += 2; }
    i16(v, e) { v = checkInt(v, 'i16'); this.ensure(2); this.dv.setInt16(this.pos, v, isLittle(e)); this.pos += 2; }
    u32(v, e) { v = checkInt(v, 'u32'); this.ensure(4); this.dv.setUint32(this.pos, v, isLittle(e)); this.pos += 4; }
    i32(v, e) { v = checkInt(v, 'i32'); this.ensure(4); this.dv.setInt32(this.pos, v, isLittle(e)); this.pos += 4; }
    u64(v, e) { v = checkInt(v, 'u64'); this.ensure(8); this.dv.setBigUint64(this.pos, v, isLittle(e)); this.pos += 8; }
    i64(v, e) { v = checkInt(v, 'i64'); this.ensure(8); this.dv.setBigInt64(this.pos, v, isLittle(e)); this.pos += 8; }
    f32(v, e) { this.ensure(4); this.dv.setFloat32(this.pos, Number(v), isLittle(e)); this.pos += 4; }
    f64(v, e) { this.ensure(8); this.dv.setFloat64(this.pos, Number(v), isLittle(e)); this.pos += 8; }
    write(t, v, e) { this[t](v, e); }
    put(bytes) { const b = toBytes(bytes); this.ensure(b.length); this.buf.set(b, this.pos); this.pos += b.length; }
    zeros(n) { this.ensure(n); this.buf.fill(0, this.pos, this.pos + n); this.pos += n; }
    fill(value, n) { this.ensure(n); this.buf.fill(value, this.pos, this.pos + n); this.pos += n; }
    /** An unsigned integer of n bytes (Number or BigInt, truncated to n bytes). */
    uint(v, n, e) { this.ensure(n); storeUint(this.buf, this.pos, n, v, e); this.pos += n; }
    /** Overwrite n bytes at absolute offset `at` (back-patching a length field). */
    patchUint(at, v, n, e) { storeUint(this.buf, at, n, v, e); }
    /** The bytes written so far (a copy). */
    toBytes() { return this.buf.slice(0, this.pos); }
    /** The bytes written so far, without copying (valid until the next write). */
    get bytes() { return this.buf.subarray(0, this.pos); }
  }

  // ================================================================================================================
  // bits (gmp/bits.hpp). Values of up to 32 bits are Numbers, wider ones BigInt.
  // ================================================================================================================
  function extractBits(p, off, n, order) {
    if (n > 32) {
      let v = 0n;
      if (order === LSB_FIRST) {
        for (let i = 0; i < n; i++) { const b = off + i; if ((p[b >> 3] >> (b & 7)) & 1) v |= 1n << BigInt(i); }
      } else {
        for (let i = 0; i < n; i++) { const b = off + i; v = (v << 1n) | BigInt((p[b >> 3] >> (7 - (b & 7))) & 1); }
      }
      return v;
    }
    let v = 0;
    if (order === LSB_FIRST) {
      for (let i = 0; i < n; i++) { const b = off + i; if ((p[b >> 3] >> (b & 7)) & 1) v += 2 ** i; }
    } else {
      for (let i = 0; i < n; i++) { const b = off + i; v = v * 2 + ((p[b >> 3] >> (7 - (b & 7))) & 1); }
    }
    return v;
  }
  function insertBits(p, off, n, value, order) {
    const v = BigInt.asUintN(n, BigInt(value));
    for (let i = 0; i < n; i++) {
      const b = off + i;
      const bit = order === LSB_FIRST ? Number((v >> BigInt(i)) & 1n) : Number((v >> BigInt(n - 1 - i)) & 1n);
      const mask = order === LSB_FIRST ? 1 << (b & 7) : 1 << (7 - (b & 7));
      p[b >> 3] = bit ? p[b >> 3] | mask : p[b >> 3] & ~mask;
    }
  }
  function signExtend(v, n) {
    if (typeof v === 'bigint') return BigInt.asIntN(n, v);
    return v >= 2 ** (n - 1) ? v - 2 ** n : v;
  }
  /** Read a bit field and convert it to type t (integer type, 'bool', or an enum's base type). */
  function getBits(p, off, n, order, t) {
    const raw = extractBits(p, off, n, order);
    if (t === 'bool') return raw !== 0 && raw !== 0n;
    const [, signed] = INT_TYPES[t] || [0, false];
    const v = signed ? signExtend(raw, n) : raw;
    return is64(t) ? BigInt(v) : Number(v);
  }
  /** Write a value into a bit field (truncated to n bits, as gmp::set_bits). */
  function setBits(p, off, n, order, value) {
    insertBits(p, off, n, typeof value === 'boolean' ? (value ? 1 : 0) : value, order);
  }

  // ================================================================================================================
  // checksums (gmp/checksum.hpp). Each algorithm: { size, init(), update(state, bytes), finish(state), compute(bytes) }.
  // State and results are Numbers for widths up to 32 bits and BigInt for 64-bit CRCs.
  // ================================================================================================================
  function reflectN(v, w) { let r = 0; for (let i = 0; i < w; i++) { r = (r * 2) + (v & 1); v >>>= 1; } return r >>> 0; }
  function reflectB(v, w) { let r = 0n; for (let i = 0; i < w; i++) { r = (r << 1n) | (v & 1n); v >>= 1n; } return r; }

  /** Generic CRC (Rocksoft model), the same as gmp::checksum::Crc. Parameters as Numbers (BigInt for width > 32). */
  function crc({ width, poly, init = 0, refin = false, refout = false, xorout = 0, name }) {
    const size = (width + 7) >> 3;
    if (width > 32) return crcBig(width, BigInt(poly), BigInt(init), refin, refout, BigInt(xorout), size, name);
    poly = Number(poly) >>> 0; init = Number(init) >>> 0; xorout = Number(xorout) >>> 0;
    const mask = width === 32 ? 0xFFFFFFFF : (2 ** width) - 1;
    const rpoly = reflectN(poly, width);
    let table = null;
    if (width >= 8) {
      table = new Uint32Array(256);
      for (let i = 0; i < 256; i++) {
        let c;
        if (refin) {
          c = i;
          for (let k = 0; k < 8; k++) c = c & 1 ? ((c >>> 1) ^ rpoly) >>> 0 : c >>> 1;
        } else {
          const top = 2 ** (width - 1);
          c = (i * 2 ** (width - 8)) >>> 0;
          for (let k = 0; k < 8; k++) c = c >= top ? (((c * 2) % 2 ** width) ^ poly) >>> 0 : (c * 2) % 2 ** width;
          c = (c & mask) >>> 0;
        }
        table[i] = c;
      }
    }
    const algo = {
      name, width, size,
      init: () => (refin ? reflectN(init, width) : init),
      update(c, data, start = 0, end = data.length) {
        if (table) {
          if (refin) for (let i = start; i < end; i++) c = (table[(c ^ data[i]) & 0xFF] ^ (c >>> 8)) >>> 0;
          else for (let i = start; i < end; i++) c = ((table[((c >>> (width - 8)) ^ data[i]) & 0xFF] ^ (c << 8)) & mask) >>> 0;
          return c;
        }
        for (let i = start; i < end; i++) {  // bitwise fallback for widths < 8
          const b = data[i];
          if (refin) {
            c ^= b;
            for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ rpoly : c >>> 1;
            c &= mask;
          } else {
            for (let k = 7; k >= 0; k--) {
              const bit = ((c >>> (width - 1)) & 1) ^ ((b >> k) & 1);
              c = (c << 1) & mask;
              if (bit) c ^= poly;
            }
          }
        }
        return c >>> 0;
      },
      finish(c) { if (refin !== refout) c = reflectN(c, width); return ((c ^ xorout) & mask) >>> 0; },
    };
    algo.compute = (data, start, end) => algo.finish(algo.update(algo.init(), data, start, end));
    return Object.freeze(algo);
  }
  function crcBig(width, poly, init, refin, refout, xorout, size, name) {
    const mask = (1n << BigInt(width)) - 1n, W = BigInt(width);
    const table = new Array(256);
    const rpoly = reflectB(poly, width);
    for (let i = 0; i < 256; i++) {
      let c;
      if (refin) { c = BigInt(i); for (let k = 0; k < 8; k++) c = c & 1n ? (c >> 1n) ^ rpoly : c >> 1n; }
      else {
        c = BigInt(i) << (W - 8n);
        const top = 1n << (W - 1n);
        for (let k = 0; k < 8; k++) c = c & top ? ((c << 1n) ^ poly) & mask : (c << 1n) & mask;
      }
      table[i] = c;
    }
    const algo = {
      name, width, size,
      init: () => (refin ? reflectB(init, width) : init),
      update(c, data, start = 0, end = data.length) {
        if (refin) for (let i = start; i < end; i++) c = table[Number((c ^ BigInt(data[i])) & 0xFFn)] ^ (c >> 8n);
        else for (let i = start; i < end; i++) c = (table[Number(((c >> (W - 8n)) ^ BigInt(data[i])) & 0xFFn)] ^ (c << 8n)) & mask;
        return c;
      },
      finish(c) { if (refin !== refout) c = reflectB(c, width); return (c ^ xorout) & mask; },
    };
    algo.compute = (data, start, end) => algo.finish(algo.update(algo.init(), data, start, end));
    return Object.freeze(algo);
  }
  function simple(name, size, update, finish = (s) => s) {
    const algo = { name, size, width: size * 8, init: () => 0, update, finish };
    algo.compute = (data, start, end) => algo.finish(algo.update(0, data, start, end));
    return Object.freeze(algo);
  }
  const forRange = (fn) => (s, d, start = 0, end = d.length) => { for (let i = start; i < end; i++) s = fn(s, d[i]); return s; };
  const checksum = {
    crc,
    crc8: crc({ name: 'crc8', width: 8, poly: 0x07 }),
    crc8_maxim: crc({ name: 'crc8_maxim', width: 8, poly: 0x31, refin: true, refout: true }),
    crc8_autosar: crc({ name: 'crc8_autosar', width: 8, poly: 0x2F, init: 0xFF, xorout: 0xFF }),
    crc8_nrsc5: crc({ name: 'crc8_nrsc5', width: 8, poly: 0x31, init: 0xFF }),
    crc15_can: crc({ name: 'crc15_can', width: 15, poly: 0x4599 }),
    crc16_ccitt: crc({ name: 'crc16_ccitt', width: 16, poly: 0x1021, init: 0xFFFF }),
    crc16_xmodem: crc({ name: 'crc16_xmodem', width: 16, poly: 0x1021 }),
    crc16_kermit: crc({ name: 'crc16_kermit', width: 16, poly: 0x1021, refin: true, refout: true }),
    crc16_x25: crc({ name: 'crc16_x25', width: 16, poly: 0x1021, init: 0xFFFF, refin: true, refout: true, xorout: 0xFFFF }),
    crc16_modbus: crc({ name: 'crc16_modbus', width: 16, poly: 0x8005, init: 0xFFFF, refin: true, refout: true }),
    crc16_arc: crc({ name: 'crc16_arc', width: 16, poly: 0x8005, refin: true, refout: true }),
    crc24_openpgp: crc({ name: 'crc24_openpgp', width: 24, poly: 0x864CFB, init: 0xB704CE }),
    crc32: crc({ name: 'crc32', width: 32, poly: 0x04C11DB7, init: 0xFFFFFFFF, refin: true, refout: true, xorout: 0xFFFFFFFF }),
    crc32c: crc({ name: 'crc32c', width: 32, poly: 0x1EDC6F41, init: 0xFFFFFFFF, refin: true, refout: true, xorout: 0xFFFFFFFF }),
    crc32_mpeg2: crc({ name: 'crc32_mpeg2', width: 32, poly: 0x04C11DB7, init: 0xFFFFFFFF }),
    crc64_ecma: crc({ name: 'crc64_ecma', width: 64, poly: 0x42F0E1EBA9EA3693n }),
    xor8: simple('xor8', 1, forRange((s, b) => s ^ b)),
    sum8: simple('sum8', 1, forRange((s, b) => (s + b) & 0xFF)),
    lrc8: simple('lrc8', 1, forRange((s, b) => (s + b) & 0xFF), (s) => (-s) & 0xFF),
    sum16: simple('sum16', 2, forRange((s, b) => (s + b) & 0xFFFF)),
    fletcher16: simple('fletcher16', 2, (s, d, start = 0, end = d.length) => {
      let s1 = s & 0xFF, s2 = (s >> 8) & 0xFF;
      for (let i = start; i < end; i++) { s1 = (s1 + d[i]) % 255; s2 = (s2 + s1) % 255; }
      return (s2 << 8) | s1;
    }),
    fletcher8_ubx: simple('fletcher8_ubx', 2, (s, d, start = 0, end = d.length) => {
      let a = (s >> 8) & 0xFF, b = s & 0xFF;
      for (let i = start; i < end; i++) { a = (a + d[i]) & 0xFF; b = (b + a) & 0xFF; }
      return (a << 8) | b;
    }),
  };

  // ================================================================================================================
  // escaping (gmp/framing/escape.hpp)
  // ================================================================================================================
  const MAX_RESERVED = 16;
  function makeEscape(esc, style, xorMask, raw, code) {
    const set = new Uint8Array(256);
    set[esc] = 1;
    for (const b of raw) set[b] = 1;
    return Object.freeze({
      enabled: true, esc, style, xorMask, raw: raw.slice(0, MAX_RESERVED), code: code.slice(0, MAX_RESERVED), set,
      needsEscape: (b) => set[b] === 1,
      encode: (b) => {
        if (style === 'table') { const i = raw.indexOf(b); if (i >= 0 && i < MAX_RESERVED) return code[i]; }
        return (b ^ xorMask) & 0xFF;
      },
      /** The raw byte for an escape code, or -1 when the code is not valid. */
      decode: (c) => {
        if (style === 'table') { const i = code.indexOf(c); return i >= 0 && i < raw.length ? raw[i] : -1; }
        return (c ^ xorMask) & 0xFF;
      },
    });
  }
  const NO_ESCAPE = Object.freeze({ enabled: false });
  /** XOR style (HDLC / PPP): each reserved byte (and esc) is sent as {esc, b ^ xorMask}. */
  const xorEscape = (esc, xorMask, reserved) => makeEscape(esc, 'xor', xorMask, [...reserved], []);
  /** Table style: raw[i] is sent as {esc, code[i]}. `raw` should include esc. */
  const mapEscape = (esc, raw, code) => makeEscape(esc, 'table', 0x20, [...raw], [...code]);
  /** Index style: reserved[i] is sent as {esc, base + i}. `reserved` should include esc. */
  const indexEscape = (esc, reserved, base = 0) =>
    makeEscape(esc, 'table', 0x20, [...reserved], [...reserved].map((_, i) => (base + i) & 0xFF));

  /** Walk escaped bytes from `at` until n unescaped bytes are produced (gmp::walk_escaped). Unescaped bytes with
   *  index in [pickAt, pickAt + pick.length) are copied into `pick`. */
  function walkEscaped(buf, at, n, cfg, pickAt = 0, pick = null) {
    let i = at;
    for (let u = 0; u < n; u++) {
      if (i >= buf.length) return { kind: 'need_more', wire: i, error: Err.None };
      let b = buf[i];
      if (b === cfg.esc) {
        if (i + 1 >= buf.length) return { kind: 'need_more', wire: i, error: Err.None };
        const d = cfg.decode(buf[i + 1]);
        if (d < 0) return { kind: 'bad', wire: i, error: Err.InvalidValue };
        b = d;
        i += 2;
      } else if (cfg.needsEscape(b)) {
        return { kind: 'bad', wire: i, error: Err.BadDelimiter };
      } else {
        i++;
      }
      if (pick && u >= pickAt && u - pickAt < pick.length) pick[u - pickAt] = b;
    }
    return { kind: 'done', wire: i, error: Err.None };
  }
  /** Unescape bytes[start, end) in place; returns the new end (gmp::unescape_in_place). */
  function unescapeInPlace(buf, cfg, start = 0, end = buf.length) {
    let w = start;
    for (let r = start; r < end; r++) {
      let b = buf[r];
      if (b === cfg.esc) {
        if (++r >= end) fail(Err.Truncated);
        const d = cfg.decode(buf[r]);
        if (d < 0) fail(Err.InvalidValue);
        b = d;
      }
      buf[w++] = b;
    }
    return w;
  }
  /** Unescaped copy of `wire`. */
  function unescape(wire, cfg) { const b = toBytes(wire).slice(); return b.subarray(0, unescapeInPlace(b, cfg)); }
  /** Number of bytes produced by unescaping `wire`. */
  function unescapedSize(wire, cfg) {
    let n = 0;
    for (let i = 0; i < wire.length; i++, n++) if (wire[i] === cfg.esc && ++i >= wire.length) fail(Err.Truncated);
    return n;
  }
  /** Number of escape pairs needed for raw[start, end). */
  function escapeOverhead(raw, cfg, start = 0, end = raw.length) {
    let n = 0;
    for (let i = start; i < end; i++) if (cfg.needsEscape(raw[i])) n++;
    return n;
  }
  /** A copy of `bytes` with bytes[at, at + len) escaped. */
  function escapeRange(bytes, at, len, cfg) {
    const extra = escapeOverhead(bytes, cfg, at, at + len);
    const out = new Uint8Array(bytes.length + extra);
    out.set(bytes.subarray(0, at));
    let w = at;
    for (let r = at; r < at + len; r++) {
      const b = bytes[r];
      if (cfg.needsEscape(b)) { out[w++] = cfg.esc; out[w++] = cfg.encode(b); } else out[w++] = b;
    }
    out.set(bytes.subarray(at + len), w);
    return out;
  }
  const escape = (raw, cfg) => escapeRange(toBytes(raw), 0, raw.length, cfg);

  // ================================================================================================================
  // framing (gmp/framing/*.hpp). A scan returns { kind: 'need_more' | 'frame' | 'skip', size, consume,
  // consumeOnError, error }.
  // ================================================================================================================
  const needMore = (hint = 0) => ({ kind: 'need_more', size: hint, consume: 0, consumeOnError: 0, error: Err.None });
  const frameScan = (n, consume, consumeOnError) => ({ kind: 'frame', size: n, consume, consumeOnError, error: Err.None });
  const skip = (n, error = Err.None) => ({ kind: 'skip', size: n, consume: 0, consumeOnError: 0, error });
  const EMPTY = new Uint8Array(0);
  /** Scan results for custom framers (gmp::FrameScan::need_more / frame / skip). */
  const FrameScan = Object.freeze({ needMore, frame: frameScan, skip });

  /** Locate `pat` at the start of buf: null when matched, otherwise the scan to return. */
  function findPatternAtStart(buf, pat) {
    if (!pat || pat.length === 0) return null;
    for (let i = 0; i < buf.length; i++) {
      const avail = Math.min(pat.length, buf.length - i);
      let ok = true;
      for (let k = 0; k < avail; k++) if (buf[i + k] !== pat[k]) { ok = false; break; }
      if (!ok) continue;
      if (i > 0) return skip(i, Err.BadSync);
      if (avail < pat.length) return needMore(pat.length);
      return null;
    }
    return buf.length === 0 ? needMore(pat.length) : skip(buf.length, Err.BadSync);
  }
  function matchesAt(buf, at, pat) {
    if (at + pat.length > buf.length) return false;
    for (let k = 0; k < pat.length; k++) if (buf[at + k] !== pat[k]) return false;
    return true;
  }

  /** Fixed-size frames: { sync, frameSize, escape, suffixLen }. */
  function scanFixed(buf, cfg) {
    const sync = cfg.sync || EMPTY, esc = cfg.escape || NO_ESCAPE;
    const r = findPatternAtStart(buf, sync);
    if (r) return r;
    if (!esc.enabled) {
      if (buf.length < cfg.frameSize) return needMore(cfg.frameSize);
      return frameScan(cfg.frameSize, cfg.frameSize, sync.length ? 1 : cfg.frameSize);
    }
    const s = sync.length, suffix = cfg.suffixLen || 0;
    const wk = walkEscaped(buf, s, cfg.frameSize - s - suffix, esc);
    if (wk.kind === 'need_more') return needMore();
    if (wk.kind === 'bad') return skip(wk.wire > 0 ? wk.wire : 1, wk.error);
    const n = wk.wire + suffix;
    if (buf.length < n) return needMore(n);
    return frameScan(n, n, s ? 1 : n);
  }

  /** Length-prefixed frames: { sync, lengthOffset, lengthSize, lengthEndian, add, minFrame, maxFrame, escape,
   *  suffixLen, countsWire, headerLen, trailerLen, wireIncludesTrailer }. */
  function scanLengthPrefixed(buf, cfg) {
    const sync = cfg.sync || EMPTY, esc = cfg.escape || NO_ESCAPE;
    const r = findPatternAtStart(buf, sync);
    if (r) return r;
    const add = cfg.add || 0, minFrame = cfg.minFrame || 0, suffix = cfg.suffixLen || 0, ls = cfg.lengthSize || 1;
    if (esc.enabled && cfg.countsWire) return scanLengthPrefixedWire(buf, cfg, sync, esc, add, minFrame, suffix, ls);
    const need = cfg.lengthOffset + ls;
    let value;
    if (!esc.enabled) {
      if (buf.length < need) return needMore(need);
      value = loadUint(buf, cfg.lengthOffset, ls, cfg.lengthEndian);
    } else {
      const lb = new Uint8Array(ls), s = sync.length;
      const wk = walkEscaped(buf, s, need - s, esc, cfg.lengthOffset - s, lb);
      if (wk.kind === 'need_more') return needMore();
      if (wk.kind === 'bad') return skip(wk.wire > 0 ? wk.wire : 1, wk.error);
      value = loadUint(lb, 0, ls, cfg.lengthEndian);
    }
    const total = value + add;
    if (total < minFrame || total > cfg.maxFrame || total <= 0) return skip(1, Err.BadLength);
    let n = total;
    if (esc.enabled) {
      const s = sync.length;
      if (n < s + suffix) return skip(1, Err.BadLength);
      const wk = walkEscaped(buf, s, n - s - suffix, esc);
      if (wk.kind === 'need_more') {
        if (wk.wire > cfg.maxFrame) return skip(1, Err.BadLength);
        return needMore();
      }
      if (wk.kind === 'bad') return skip(wk.wire > 0 ? wk.wire : 1, wk.error);
      n = wk.wire + suffix;
      if (n > cfg.maxFrame) return skip(1, Err.BadLength);
    }
    if (buf.length < n) return needMore(n);
    return frameScan(n, n, 1);
  }
  function scanLengthPrefixedWire(buf, cfg, sync, esc, add, minFrame, suffix, ls) {
    const s = sync.length, lb = new Uint8Array(ls);
    const hw = walkEscaped(buf, s, cfg.headerLen, esc, cfg.lengthOffset - s, lb);
    if (hw.kind === 'need_more') return needMore();
    if (hw.kind === 'bad') return skip(hw.wire > 0 ? hw.wire : 1, hw.error);
    const body = loadUint(lb, 0, ls, cfg.lengthEndian) + add;
    if (body < 0 || hw.wire + body + suffix > cfg.maxFrame) return skip(1, Err.BadLength);
    const bodyEnd = hw.wire + body;
    for (let i = hw.wire; i < bodyEnd; i++) {
      if (i >= buf.length) return needMore(bodyEnd);
      if (buf[i] === esc.esc) {
        if (i + 1 >= bodyEnd) return skip(1, Err.BadLength);
        i++;
      } else if (esc.needsEscape(buf[i])) {
        return skip(i, Err.BadDelimiter);
      }
    }
    let end = bodyEnd;
    if (!cfg.wireIncludesTrailer) {
      const tw = walkEscaped(buf, bodyEnd, cfg.trailerLen || 0, esc);
      if (tw.kind === 'need_more') return needMore();
      if (tw.kind === 'bad') return skip(tw.wire, tw.error);
      end = tw.wire;
    }
    const n = end + suffix;
    if (n < minFrame || n > cfg.maxFrame) return skip(1, Err.BadLength);
    if (buf.length < n) return needMore(n);
    return frameScan(n, n, 1);
  }

  /** Delimited frames: { start, end, escape, maxFrame }. */
  function scanDelimited(buf, cfg) {
    const start = cfg.start || EMPTY, end = cfg.end, esc = cfg.escape || NO_ESCAPE;
    const r = findPatternAtStart(buf, start);
    if (r) return r;
    const s = start.length;
    const shared = s > 0 && s === end.length && matchesAt(end, 0, start);
    for (let i = s; i < buf.length; i++) {
      if (i + 1 > cfg.maxFrame) return skip(s ? s : i, Err.BadLength);
      if (esc.enabled && buf[i] === esc.esc) { i++; continue; }
      if (matchesAt(buf, i, end)) {
        const n = i + end.length;
        if (shared) {
          if (i === s) return skip(s);
          return frameScan(n, n - end.length, n - end.length);
        }
        return frameScan(n, n, n);
      }
      if (!shared && s && matchesAt(buf, i, start)) return skip(i, Err.BadDelimiter);
    }
    if (buf.length >= cfg.maxFrame) return skip(s ? s : buf.length, Err.BadLength);
    return needMore();
  }

  // ================================================================================================================
  // stream parser (gmp/stream_parser.hpp)
  //
  // `protocol` provides: maxFrame, scan(pending: Uint8Array) -> scan result, and process(frame: Uint8Array) ->
  // { header, message } (throws GmpError when the frame is invalid). `message` is a view object whose class has a
  // static `messageName`. The frame passed to process() is a copy that the result may keep. Generated protocols
  // (gmpc --lang js) provide all of these.
  // ================================================================================================================
  const errorOf = (s) => { const e = new GmpError(s.error); e.bytes = s.bytes; e.offset = s.offset; return e; };

  class StreamParser {
    constructor(protocol, { capacity } = {}) {
      this.protocol = protocol;
      this.capacity = capacity || protocol.maxFrame;
      if (!(this.capacity > 0)) fail(Err.InvalidValue, 'stream capacity must be > 0');
      this.buf = new Uint8Array(this.capacity);
      this.head = 0;
      this.tail = 0;
      this.listeners = new Map();
      this.stats = { frames: 0, frameErrors: 0, bytesSkipped: 0, overflows: 0 };
      this.lastError = Err.None;
      this.consumed = 0;  // bytes removed from the front of the buffer since construction / reset()
    }
    get buffered() { return this.tail - this.head; }
    /** Stream offset of the next buffered byte (bytes consumed so far). */
    get position() { return this.consumed; }

    /** Listen for one message (by name or generated class), 'frame' (every message) or 'error'. Message and frame
     *  listeners get (view, header); error listeners get a GmpError with `bytes`. Returns a function that removes
     *  the listener. */
    on(what, fn) {
      const key = typeof what === 'string' ? what : what && (what.messageName || what.name);
      if (!this.listeners.has(key)) this.listeners.set(key, []);
      this.listeners.get(key).push(fn);
      return () => { const l = this.listeners.get(key); const i = l ? l.indexOf(fn) : -1; if (i >= 0) l.splice(i, 1); };
    }
    emit(key, a, b) { const l = this.listeners.get(key); if (l) for (const fn of [...l]) fn(a, b); }
    deliver(f) {
      this.emit(f.message.constructor.messageName, f.message, f.header);
      this.emit('frame', f.message, f.header);
    }
    reportError(s) { this.emit('error', errorOf(s)); }

    /** Copy as much of `bytes` as fits; returns the number of bytes accepted. */
    push(bytes) {
      const b = toBytes(bytes);
      if (this.head > 0 && this.capacity - this.tail < b.length) this.compact();
      const n = Math.min(b.length, this.capacity - this.tail);
      this.buf.set(b.subarray(0, n), this.tail);
      this.tail += n;
      return n;
    }
    /** Push all of `bytes` and deliver every complete frame to the listeners. Returns the number of frames. */
    feed(bytes) {
      let b = toBytes(bytes);
      const before = this.stats.frames;
      do {
        const n = this.push(b);
        b = b.subarray(n);
        while (this.next()) { /* deliver */ }
      } while (b.length);
      return this.stats.frames - before;
    }
    /** One step: deliver the next frame or error to the listeners. Returns false when more input is needed. */
    next() {
      const s = this.step();
      if (s.kind === 'frame') this.deliver(s.frame);
      else if (s.kind === 'error') this.reportError(s);
      return s.kind !== 'need_more';
    }

    // ---- pull API: the caller asks for the next message --------------------------------------------------------
    /** The next valid message view, or null when more input is needed. Errors go to the 'error' listeners. */
    poll() { const f = this.pull(); return f ? f.message : null; }
    /** Like poll(), as { header, message, bytes } (header is undefined for protocols without one). */
    pollFrame() {
      const f = this.pull();
      return f ? { header: f.header, message: f.message, bytes: f.bytes, offset: f.offset } : null;
    }
    /** Like poll(), decoded into the message class (a frame whose struct decode fails is reported as an error). */
    pollMessage() {
      for (;;) {
        const s = this.step((f) => { f.struct = f.message.toStruct(); });
        if (s.kind === 'frame') return s.frame.struct;
        if (s.kind === 'need_more') return null;
        this.reportError(s);
      }
    }
    /** The next message or error in stream order: { message, header, bytes, offset } or { error: GmpError, offset },
     *  or null. `offset` is the position of the frame / dropped bytes in the stream. Errors are returned here and not
     *  passed to the listeners. */
    pollEvent() {
      const s = this.step();
      if (s.kind === 'need_more') return null;
      if (s.kind === 'error') return { error: errorOf(s), offset: s.offset };
      return { message: s.frame.message, header: s.frame.header, bytes: s.frame.bytes, offset: s.frame.offset };
    }
    /** Wait (without blocking) for a message of one type; other messages go to the listeners. */
    pollFor(what) {
      const key = typeof what === 'string' ? what : what && (what.messageName || what.name);
      for (;;) {
        const f = this.pull();
        if (!f) return null;
        if (f.message.constructor.messageName === key) return f.message;
        this.deliver(f);
      }
    }
    /** Iterate over the messages in `bytes` (pushed in pieces as room allows), then over anything already buffered. */
    *messages(bytes) {
      let b = bytes === undefined ? EMPTY : toBytes(bytes);
      for (;;) {
        const n = this.push(b);
        b = b.subarray(n);
        let m;
        while ((m = this.poll())) yield m;
        if (!b.length) return;
      }
    }
    /** Like messages(), yielding pollEvent() results (messages and errors). */
    *events(bytes) {
      let b = bytes === undefined ? EMPTY : toBytes(bytes);
      for (;;) {
        const n = this.push(b);
        b = b.subarray(n);
        let e;
        while ((e = this.pollEvent())) yield e;
        if (!b.length) return;
      }
    }

    reset() {
      this.head = this.tail = 0;
      this.stats = { frames: 0, frameErrors: 0, bytesSkipped: 0, overflows: 0 };
      this.lastError = Err.None;
      this.consumed = 0;
    }

    // ---- internals ---------------------------------------------------------------------------------------------
    pull() {
      for (;;) {
        const s = this.step();
        if (s.kind === 'frame') return s.frame;
        if (s.kind === 'need_more') return null;
        this.reportError(s);
      }
    }
    /** Scan to the next frame or reportable error (StreamParser::step). Consumed bytes are dropped at once: frames
     *  and error bytes are copies. */
    step(accept) {
      for (;;) {
        if (this.head === this.tail) { this.head = this.tail = 0; return { kind: 'need_more' }; }
        const pending = this.buf.subarray(this.head, this.tail);
        const s = this.protocol.scan(pending);
        const offset = this.consumed;
        if (s.kind === 'frame') {
          const bytes = pending.slice(0, s.size);
          let f = null, error = Err.None;
          try {
            f = this.protocol.process(bytes.slice());
            f.bytes = bytes;
            f.offset = offset;
            if (accept) accept(f);
          } catch (e) {
            if (!(e instanceof GmpError)) throw e;
            error = e.code;
          }
          if (error === Err.None) {
            this.stats.frames++;
            this.consume(s.consume);
            return { kind: 'frame', frame: f };
          }
          this.stats.frameErrors++;
          this.stats.bytesSkipped += s.consumeOnError;
          this.consume(s.consumeOnError);
          this.lastError = error;
          return { kind: 'error', error, bytes, offset };
        }
        if (s.kind === 'skip') {
          const n = Math.max(1, Math.min(s.size, pending.length));
          this.stats.bytesSkipped += n;
          if (s.error === Err.None) { this.consume(n); continue; }
          const bytes = pending.slice(0, n);
          this.consume(n);
          this.lastError = s.error;
          return { kind: 'error', error: s.error, bytes, offset };
        }
        if (pending.length >= this.capacity) {
          this.stats.overflows++;
          this.stats.bytesSkipped++;
          const bytes = pending.slice(0, 1);
          this.consume(1);
          this.lastError = Err.BufferFull;
          return { kind: 'error', error: Err.BufferFull, bytes, offset };
        }
        if (this.head > 0) this.compact();
        return { kind: 'need_more' };
      }
    }
    consume(n) {
      n = Math.min(n, this.tail - this.head);
      this.consumed += n;
      this.head += n;
      if (this.head === this.tail) this.head = this.tail = 0;
    }
    compact() {
      if (this.head === 0) return;
      this.buf.copyWithin(0, this.head, this.tail);
      this.tail -= this.head;
      this.head = 0;
    }
  }

  // ================================================================================================================
  // float text (exact; same results as std::to_chars / std::from_chars, ported from runtime-c gmp/c/float_text.h)
  // ================================================================================================================
  const F64 = { p: 53, eminLsb: -1074, emax: 2047, bias: 1023, expBits: 11 };
  const F32 = { p: 24, eminLsb: -149, emax: 255, bias: 127, expBits: 8 };
  const scratch = new DataView(new ArrayBuffer(8));
  function bitsOf(x, isF32) {
    if (isF32) { scratch.setFloat32(0, x); return BigInt(scratch.getUint32(0)); }
    scratch.setFloat64(0, x); return scratch.getBigUint64(0);
  }
  function fromBits(bits, isF32) {
    if (isF32) { scratch.setUint32(0, Number(bits)); return scratch.getFloat32(0); }
    scratch.setBigUint64(0, bits); return scratch.getFloat64(0);
  }
  const bitLen = (a) => (a === 0n ? 0 : a.toString(2).length);
  const P10 = [1n];
  const pow10 = (k) => { while (P10.length <= k) P10.push(P10[P10.length - 1] * 10n); return P10[k]; };

  /** Split into { kind: 0 finite | 1 inf | 2 nan, neg, m (BigInt significand), e2 }. */
  function split(x, isF32) {
    const f = isF32 ? F32 : F64, bits = bitsOf(x, isF32), P = BigInt(f.p - 1);
    const frac = bits & ((1n << P) - 1n);
    const biased = Number((bits >> P) & ((1n << BigInt(f.expBits)) - 1n));
    const neg = ((bits >> (P + BigInt(f.expBits))) & 1n) === 1n;
    if (biased === f.emax) return { kind: frac ? 2 : 1, neg };
    if (biased === 0) return { kind: 0, neg, m: frac, e2: f.eminLsb };
    return { kind: 0, neg, m: frac | (1n << P), e2: biased - f.bias - (f.p - 1) };
  }

  /** Round q * 2^e2 (+ sticky remainder) to the format, ties to even. Returns raw bits or null on overflow. */
  function makeBits(q, e2, sticky, f) {
    if (q === 0n) return 0n;
    const lead = e2 + bitLen(q) - 1;
    let lsb = Math.max(lead - (f.p - 1), f.eminLsb);
    const shift = lsb - e2;
    let m;
    if (shift <= 0) m = q << BigInt(-shift);
    else {
      const rem = q & ((1n << BigInt(shift)) - 1n), half = 1n << BigInt(shift - 1);
      m = q >> BigInt(shift);
      if (rem > half || (rem === half && (sticky || (m & 1n)))) m++;
    }
    if (m === 1n << BigInt(f.p)) { m >>= 1n; lsb++; }
    if (m >= 1n << BigInt(f.p - 1)) {
      const biased = lsb + (f.p - 1) + f.bias;
      if (biased >= f.emax) return null;
      return (BigInt(biased) << BigInt(f.p - 1)) | (m - (1n << BigInt(f.p - 1)));
    }
    return m;
  }

  /** Parse text as std::from_chars (after gmp's leading '+' strip). Throws MissingField / ParseError / OutOfRange. */
  function parseFloatText(s, isF32 = false) {
    const f = isF32 ? F32 : F64;
    if (s.length === 0) fail(Err.MissingField);
    if (s[0] === '+') s = s.slice(1);
    let i = 0, neg = false;
    if (s[0] === '-') { neg = true; i = 1; }
    const signed = (x) => (neg ? -x : x);
    const c0 = s[i];
    if (c0 === 'i' || c0 === 'I' || c0 === 'n' || c0 === 'N') {
      const r = s.slice(i).toLowerCase();
      if (r === 'inf' || r === 'infinity') return signed(Infinity);
      if (r.startsWith('nan')) {
        if (r.length > 3 && !/^nan\([0-9a-z_]*\)$/.test(r)) fail(Err.ParseError);
        return signed(NaN);
      }
      fail(Err.ParseError);
    }
    let num = 0n, ndig = 0, exp10 = 0, any = false, dropped = false, point = false, leading = true;
    for (; i < s.length; i++) {
      const c = s[i];
      if (c === '.') { if (point) break; point = true; continue; }
      if (c < '0' || c > '9') break;
      any = true;
      if (leading && c === '0') { if (point) exp10--; continue; }
      leading = false;
      if (ndig < 800) { num = num * 10n + BigInt(c.charCodeAt(0) - 48); ndig++; if (point) exp10--; }
      else { if (c !== '0') dropped = true; if (!point) exp10++; }
    }
    if (!any) fail(Err.ParseError);
    if (i < s.length && (s[i] === 'e' || s[i] === 'E')) {
      let j = i + 1, en = false, e = 0;
      if (s[j] === '+' || s[j] === '-') { en = s[j] === '-'; j++; }
      if (j < s.length && s[j] >= '0' && s[j] <= '9') {
        for (; j < s.length && s[j] >= '0' && s[j] <= '9'; j++) if (e < 100000000) e = e * 10 + (s.charCodeAt(j) - 48);
        exp10 += en ? -e : e;
        i = j;
      }
    }
    const trailing = i !== s.length;
    if (dropped) { num = num * 10n + 1n; ndig++; exp10--; }
    if (num === 0n) { if (trailing) fail(Err.ParseError); return neg ? -0 : 0; }
    const lead10 = exp10 + ndig - 1;
    if (lead10 > 310 || lead10 < -400) fail(Err.OutOfRange);
    let raw;
    if (exp10 >= 0) {
      const v = num * pow10(exp10);
      const bl = bitLen(v);
      let sticky = false, q = v, e2 = 0;
      if (bl > 60) { e2 = bl - 60; sticky = (v & ((1n << BigInt(e2)) - 1n)) !== 0n; q = v >> BigInt(e2); }
      raw = makeBits(q, e2, sticky, f);
    } else {
      let a = num, b = pow10(-exp10);
      const sh = (f.p + 4) + bitLen(b) - bitLen(a);
      if (sh > 0) a <<= BigInt(sh); else b <<= BigInt(-sh);
      const q = a / b;
      raw = makeBits(q, -sh, a % b !== 0n, f);
    }
    if (raw === null || raw === 0n) fail(Err.OutOfRange);
    if (trailing) fail(Err.ParseError);
    return signed(fromBits(raw, isF32));
  }

  /** Fixed notation with `precision` decimals of m * 2^e2, rounded half to even. */
  function fixedDigits(m, e2, precision) {
    let ip, frac = 0n, fb = 0;
    if (e2 >= 0) ip = m << BigInt(e2);
    else { fb = -e2; ip = m >> BigInt(fb); frac = m & ((1n << BigInt(fb)) - 1n); }
    const one = 1n << BigInt(fb);
    // scaled = frac * 10^precision / 2^fb, exactly, with the remainder for rounding
    const num = frac * pow10(precision);
    let q = num >> BigInt(fb);
    const rem = num - (q << BigInt(fb));
    let whole = ip * pow10(precision) + q;
    if (rem !== 0n) {
      const c = rem * 2n - one;
      if (c > 0n || (c === 0n && (whole & 1n))) whole++;
    }
    let t = whole.toString();
    if (precision > 0) {
      t = t.padStart(precision + 1, '0');
      t = t.slice(0, t.length - precision) + '.' + t.slice(t.length - precision);
    }
    return t;
  }

  /** Shortest round-trip digits (Burger & Dybvig): { d: digit string, k } with value = 0.d * 10^k. */
  function shortestDigits(m, e2, f) {
    const even = (m & 1n) === 0n;
    const unequal = m === 1n << BigInt(f.p - 1) && e2 > f.eminLsb;
    let r, s, mp, mm;
    if (e2 >= 0) {
      r = m << BigInt(e2 + (unequal ? 2 : 1)); s = unequal ? 4n : 2n;
      mp = 1n << BigInt(e2 + (unequal ? 1 : 0)); mm = 1n << BigInt(e2);
    } else {
      r = m << BigInt(unequal ? 2 : 1); s = 1n << BigInt(-e2 + (unequal ? 2 : 1));
      mp = unequal ? 2n : 1n; mm = 1n;
    }
    const lead = e2 + bitLen(m) - 1;
    let k = Math.floor((lead * 78913) / 262144) + 1;
    if (k >= 0) s *= pow10(k); else { const p = pow10(-k); r *= p; mp *= p; mm *= p; }
    for (;;) {
      const c = r + mp;
      if (even ? c >= s : c > s) { s *= 10n; k++; continue; }
      const c10 = c * 10n;
      if (even ? c10 < s : c10 <= s) { r *= 10n; mp *= 10n; mm *= 10n; k--; continue; }
      break;
    }
    const digits = [];
    for (;;) {
      r *= 10n; mp *= 10n; mm *= 10n;
      const d0 = r / s;
      r -= d0 * s;
      let d = Number(d0);
      const tc1 = even ? r <= mm : r < mm;
      const tc2 = even ? r + mp >= s : r + mp > s;
      if (!tc1 && !tc2) { digits.push(d); if (digits.length >= 40) break; continue; }
      if (tc1 && tc2) { const c = r * 2n; if (c > s || (c === s && (d & 1))) d++; }
      else if (tc2) d++;
      digits.push(d);
      break;
    }
    let i = digits.length - 1;
    while (i > 0 && digits[i] > 9) { digits[i] = 0; digits[--i]++; }
    if (digits[0] > 9) { digits[0] = 1; for (let j = 1; j < digits.length; j++) digits[j] = 0; k++; }
    while (digits.length > 1 && digits[digits.length - 1] === 0) digits.pop();
    return { d: digits.join(''), k };
  }

  const expText = (x) => 'e' + (x < 0 ? '-' : '+') + String(Math.abs(x)).padStart(2, '0');

  function shortestText(m, e2, f) {
    const { d, k } = shortestDigits(m, e2, f);
    const n = d.length, x = k - 1;
    const sciLen = n + (n > 1 ? 1 : 0) + 2 + (Math.abs(x) >= 100 ? 3 : 2);
    const fixedLen = k <= 0 ? 2 - k + n : k < n ? n + 1 : k;
    if (fixedLen <= sciLen) {
      if (k <= 0) return '0.' + '0'.repeat(-k) + d;
      if (k < n) return d.slice(0, k) + '.' + d.slice(k);
      if (e2 > 0) return fixedDigits(m, e2, 0);  // integer with gaps of 2+: its exact value, as to_chars
      return d + '0'.repeat(k - n);
    }
    return d[0] + (n > 1 ? '.' + d.slice(1) : '') + expText(x);
  }

  /** printf "%.*g" / to_chars(general, sig): `sig` significant digits, ties to even. */
  function generalText(m, e2, sig) {
    if (sig <= 0) sig = 1;
    let d, k;
    if (m === 0n) { d = '0'; k = 1; }
    else {
      let a = m, scale = 0;
      if (e2 >= 0) a <<= BigInt(e2); else { scale = -e2; a *= 5n ** BigInt(scale); }
      d = a.toString();
      k = d.length - scale;
      d = d.replace(/0+$/, '') || '0';
    }
    if (d.length > sig) {
      const next = d[sig];
      const up = next !== '5' ? next > '5' : d.length > sig + 1 || (d.charCodeAt(sig - 1) - 48) % 2 === 1;
      let ds = d.slice(0, sig).split('').map(Number);
      if (up) {
        let i = sig - 1;
        while (i >= 0 && ds[i] === 9) ds[i--] = 0;
        if (i >= 0) ds[i]++; else { ds = [1, ...ds.slice(1)]; ds[0] = 1; k++; }
      }
      d = ds.join('').replace(/0+$/, '') || '0';
    }
    const x = k - 1;
    if (x >= -4 && x < sig) {
      if (k <= 0) return '0.' + '0'.repeat(-k) + d;
      let out = '';
      for (let i = 0; i < d.length || i < k; i++) { if (i === k) out += '.'; out += i < d.length ? d[i] : '0'; }
      return out;
    }
    return d[0] + (d.length > 1 ? '.' + d.slice(1) : '') + expText(x);
  }

  /** Format as std::to_chars: precision >= 0 -> fixed with that many decimals; otherwise the shortest text that reads
   *  back to the same value of its type (f32 when isF32). */
  function formatFloat(x, precision = -1, isF32 = false) {
    const sp = split(x, isF32 && precision < 0);
    const sign = sp.neg ? '-' : '';
    if (sp.kind === 1) return sign + 'inf';
    if (sp.kind === 2) return sign + 'nan';
    if (precision >= 0) return sign + fixedDigits(sp.m, sp.e2, precision);
    if (sp.m === 0n) return sign + '0';
    return sign + shortestText(sp.m, sp.e2, isF32 ? F32 : F64);
  }
  /** std::to_chars(x, chars_format::general, digits), as printf "%.*g". */
  function formatGeneral(x, digits) {
    const sp = split(x, false);
    const sign = sp.neg ? '-' : '';
    if (sp.kind === 1) return sign + 'inf';
    if (sp.kind === 2) return sign + 'nan';
    return sign + generalText(sp.m, sp.e2, digits < 1 ? 1 : digits);
  }

  // ================================================================================================================
  // text (gmp/text.hpp)
  // ================================================================================================================
  /** Split on sep, keeping empty tokens ("a,,b" -> ["a", "", "b"]). */
  const splitTokens = (s, sep) => s.split(sep);

  /** Parse an integer of type t as std::from_chars (after one leading '+'). Number, or BigInt for 64-bit types. */
  function parseInt_(s, t, base = 10) {
    if (s.length === 0) fail(Err.MissingField);
    if (s[0] === '+') s = s.slice(1);
    const [bits, signed] = INT_TYPES[t];
    let i = 0, neg = false;
    if (signed && s[0] === '-') { neg = true; i = 1; }
    let v = 0n, any = false;
    const B = BigInt(base);
    for (; i < s.length; i++) {
      const c = s.charCodeAt(i);
      const d = c >= 48 && c <= 57 ? c - 48 : c >= 97 && c <= 122 ? c - 87 : c >= 65 && c <= 90 ? c - 55 : 99;
      if (d >= base) break;
      any = true;
      v = v * B + BigInt(d);
    }
    if (!any) fail(Err.ParseError);
    if (neg) v = -v;
    const lo = signed ? -(1n << BigInt(bits - 1)) : 0n, hi = signed ? (1n << BigInt(bits - 1)) - 1n : (1n << BigInt(bits)) - 1n;
    if (v < lo || v > hi) fail(Err.OutOfRange);
    if (i !== s.length) fail(Err.ParseError);
    return bits === 64 ? v : Number(v);
  }
  function parseChar(s) {
    if (s.length === 0) fail(Err.MissingField);
    if (s.length !== 1) fail(Err.ParseError);
    return s;
  }
  function parseHexByte(s) {
    if (s.length !== 2) fail(Err.ParseError);
    return parseInt_(s, 'u8', 16);
  }
  /** n bytes written as 2n hex digits (a text checksum), as BigInt; null when malformed. */
  function hexValue(s, n) {
    if (s.length !== 2 * n) return null;
    let v = 0n;
    for (let i = 0; i < n; i++) {
      const [e, b] = attempt(() => parseHexByte(s.slice(2 * i, 2 * i + 2)));
      if (e) return null;
      v = (v << 8n) | BigInt(b);
    }
    return v;
  }
  /** The low n bytes of v as 2n uppercase hex digits. */
  function hexText(v, n) {
    let t = '';
    for (let k = n - 1; k >= 0; k--) t += hexByteText(Number((BigInt(v) >> BigInt(8 * k)) & 0xFFn));
    return t;
  }
  const hexByteText = (b) => '0123456789ABCDEF'[(b >> 4) & 15] + '0123456789ABCDEF'[b & 15];

  /** Builds a text message, inserting separators between tokens (gmp::text::TextWriter). */
  class TextWriter {
    constructor(sep) { this.sep = sep; this.out = ''; this.first = true; }
    beginToken() { if (!this.first) this.out += this.sep; this.first = false; }
    raw(s) { this.out += s; }
    emptyToken() { this.beginToken(); }
    token(s) { this.beginToken(); this.out += s; }
    /** Integer token in `base`, zero-padded to `width` digits. */
    intToken(v, base = 10, width = 0, upper = true) {
      this.beginToken();
      let d = (typeof v === 'bigint' ? v : BigInt(v)).toString(base);
      if (d[0] === '-') { this.out += '-'; d = d.slice(1); }
      if (d.length < width) this.out += '0'.repeat(width - d.length);
      this.out += upper && base > 10 ? d.toUpperCase() : d;
    }
    /** Float token: precision >= 0 -> fixed; width zero-pads the integer part (NMEA "0123.45"). */
    floatToken(v, precision = -1, width = 0, isF32 = false) {
      this.beginToken();
      let s = formatFloat(v, precision, isF32);
      const neg = s[0] === '-';
      if (neg) { this.out += '-'; s = s.slice(1); }
      const len = s.length + (neg ? 1 : 0);
      if (len < width) this.out += '0'.repeat(width - len);
      this.out += s;
    }
    toString() { return this.out; }
  }

  // ================================================================================================================
  // printing (gmp/print.hpp). Generated classes have printTo(p) and toString() (= gmsg.print(this)).
  // ================================================================================================================
  const HEX = '0123456789ABCDEF';
  const hexByte = (b) => HEX[(b >> 4) & 15] + HEX[b & 15];
  function escChar(c, quote) {
    if (c === 10) return '\\n';
    if (c === 13) return '\\r';
    if (c === 9) return '\\t';
    if (c === 92) return '\\\\';
    if (c === quote) return '\\' + String.fromCharCode(c);
    if (c < 0x20 || c >= 0x7f) return '\\x' + hexByte(c);
    return String.fromCharCode(c);
  }
  const DEFAULT_PRINT = Object.freeze({ style: 'multi_line', indent: 2, maxItems: 64, maxBytes: 64, precision: 9 });

  /** The printer state machine of gmp::Printer, writing into a string. Options: style ('multi_line' |
   *  'single_line'), indent, maxItems, maxBytes (0 = all), precision (digits of converted values). */
  class Printer {
    constructor(opts) {
      this.o = { ...DEFAULT_PRINT, ...opts };
      this.out = '';
      this.depth = 0;
      this.firsts = [];
      this.noteOpen = false;
    }
    get multiLine() { return this.o.style !== 'single_line'; }
    put(t) { this.out += t; }
    push() { this.firsts[this.depth] = true; this.depth++; }
    pop() { if (this.depth) this.depth--; }
    isFirst() { return this.depth > 0 && this.firsts[this.depth - 1]; }
    clearFirst() { if (this.depth > 0) this.firsts[this.depth - 1] = false; }
    newline() { this.put('\n' + ' '.repeat(this.depth * this.o.indent)); }
    separate() {
      if (this.multiLine) this.newline();
      else this.put(this.isFirst() ? ' ' : ', ');
      this.clearFirst();
    }
    openNote() { this.put(this.noteOpen ? ', ' : ' ('); this.noteOpen = true; }
    closeNote() { if (this.noteOpen) { this.noteOpen = false; this.put(')'); } }
    more(k) { this.put(`... (+${k} more)`); }

    // ---- structure ----
    begin(type) { this.closeNote(); if (type) this.put(type + ' '); this.put('{'); this.push(); }
    end() {
      this.closeNote();
      const empty = this.isFirst();
      this.pop();
      if (empty) this.put('}');
      else if (this.multiLine) { this.newline(); this.put('}'); }
      else this.put(' }');
    }
    key(name) { this.closeNote(); this.separate(); this.put(name + ': '); }
    absent(name) { this.key(name); this.put('<absent>'); }
    unit(u) { if (u) this.put(' ' + u); }
    /** Converted value in the note after a value: `12600 (12.6 V)`. */
    conversion(x, u) { this.openNote(); this.put(formatGeneral(Number(x), this.o.precision)); this.unit(u); }
    note(t) { this.openNote(); this.put(t); }
    text(t) { this.put(t); }
    flush() { this.closeNote(); }

    // ---- values ----
    int(v) { this.put(String(v)); }
    float(v, isF32 = false) { this.put(formatFloat(Number(v), -1, isF32)); }
    bool(v) { this.put(v ? 'true' : 'false'); }
    /** A char: a one-character string or a byte value. */
    char(c) { this.put("'" + escChar(typeof c === 'string' ? c.charCodeAt(0) : c, 39) + "'"); }
    /** An enum value: `name (0xNN)`, `<unknown> (0xNN)` when name is empty; size = bytes of the base type. */
    enumValue(name, raw, size) {
      const bits = BigInt(8 * size);
      const u = BigInt.asUintN(Number(bits), BigInt(raw));
      this.put(`${name || '<unknown>'} (0x${u.toString(16).toUpperCase().padStart(2 * size, '0')})`);
    }
    hex(bytes) {
      const n = bytes.length, shown = this.o.maxBytes && n > this.o.maxBytes ? this.o.maxBytes : n;
      let t = '[';
      for (let i = 0; i < shown; i++) t += (i ? ' ' : '') + hexByte(bytes[i]);
      this.put(t);
      if (shown < n) { this.put(' '); this.more(n - shown); }
      this.put(']');
    }
    /** A quoted string (one character per byte). */
    str(s) {
      let t = '"';
      for (let i = 0; i < s.length; i++) t += escChar(s.charCodeAt(i), 34);
      this.put(t + '"');
    }
    error(code) { this.put(`<error: ${errorText(code)}>`); }
    /** `[a, b, c]`; `each(item)` prints one element; blocks = elements are messages (one per line, multi_line). */
    list(items, each, blocks = false) {
      const n = items.length, shown = this.o.maxItems && n > this.o.maxItems ? this.o.maxItems : n;
      this.put('[');
      if (!n) { this.put(']'); return; }
      const lines = blocks && this.multiLine;
      if (lines) this.push();
      for (let i = 0; i < shown; i++) {
        if (lines) this.newline();
        else if (i) this.put(', ');
        each(items[i], i);
      }
      if (shown < n) {
        if (lines) this.newline();
        else this.put(', ');
        this.more(n - shown);
      }
      if (lines) { this.pop(); this.newline(); }
      this.put(']');
    }
    /** A value with its own printTo (generated classes), or a nested message. */
    value(v) {
      if (v && typeof v.printTo === 'function') v.printTo(this);
      else if (typeof v === 'boolean') this.bool(v);
      else if (typeof v === 'bigint' || (typeof v === 'number' && Number.isInteger(v))) this.int(v);
      else if (typeof v === 'number') this.float(v);
      else if (typeof v === 'string') this.str(v);
      else if (v instanceof Uint8Array) this.hex(v);
      else if (v === undefined || v === null) this.put('<absent>');
      else this.put('<?>');
    }
  }

  /** Text of a generated struct / view (gmp::print). Options as Printer. */
  function print(v, opts) {
    const p = new Printer(opts);
    p.value(v);
    p.flush();
    return p.out;
  }

  // ================================================================================================================
  // helpers used by generated code
  // ================================================================================================================
  /** Run fn; [0, result], or [GmpError code, undefined] when it throws a GmpError. */
  function attempt(fn) {
    try { return [Err.None, fn()]; } catch (e) { if (e instanceof GmpError) return [e.code, undefined]; throw e; }
  }
  /** Element count of a container (0 for an absent one): the value of a derived length / count field. */
  const count = (v) => (v === undefined || v === null ? 0 : v.length);
  /** Concatenate byte arrays. */
  function concat(...parts) {
    const bs = parts.map(toBytes);
    const out = new Uint8Array(bs.reduce((n, b) => n + b.length, 0));
    let at = 0;
    for (const b of bs) { out.set(b, at); at += b.length; }
    return out;
  }
  /** Elements of `size` bytes each: make(bytes of element i). */
  function fixedArray(b, size, make) {
    const out = new Array(Math.floor(b.length / size));
    for (let i = 0; i < out.length; i++) out[i] = make(b.subarray(i * size, (i + 1) * size));
    return out;
  }
  /** Variable-size elements one after another: make(rest) returns a view with wireSize. */
  function seqArray(b, make) {
    const out = [];
    for (let pos = 0; pos < b.length;) { const v = make(b.subarray(pos)); out.push(v); pos += v.wireSize || b.length; }
    return out;
  }
  /** Print a nested message: its own printTo, or that of `cls` for a plain object. */
  function printNested(p, v, cls) {
    if (v && typeof v.printTo === 'function') v.printTo(p);
    else cls.prototype.printTo.call(v, p);
  }
  /** `v` when the strict enum declares it, else GmpError UnknownEnum. */
  function knownEnum(e, v) { if (!e.isKnown(v)) fail(Err.UnknownEnum); return v; }
  /** A char value (a one-character string, or a byte value). */
  function checkChar(v) {
    if (typeof v === 'number') return String.fromCharCode(checkInt(v, 'u8', 'char'));
    if (typeof v !== 'string' || v.length !== 1 || v.charCodeAt(0) > 255) fail(Err.OutOfRange, 'char must be one character');
    return v;
  }

  // ================================================================================================================
  // enums (generated code builds them with gmsg.enumType)
  // ================================================================================================================
  /** A frozen object mapping names to values, plus non-enumerable helpers: nameOf(v) ('' when unknown),
   *  isKnown(v), values, typeName, base, strict. */
  function enumType(typeName, base, entries, strict = false) {
    const e = {};
    const byValue = new Map();
    for (const [k, v] of entries) { e[k] = v; if (!byValue.has(String(v))) byValue.set(String(v), k); }
    const hidden = {
      typeName, base, strict, size: SIZE[base],
      values: Object.freeze(entries.map(([, v]) => v)),
      nameOf: (v) => byValue.get(String(v)) || '',
      isKnown: (v) => byValue.has(String(v)),
    };
    for (const [k, v] of Object.entries(hidden)) Object.defineProperty(e, k, { value: v, enumerable: false });
    return Object.freeze(e);
  }

  return Object.freeze({
    version: '0.1.0',
    Err, GmpError, errorText, errorName, fail,
    BIG, LITTLE, MSB_FIRST, LSB_FIRST, SIZE,
    toBytes, latin1Bytes, latin1String, loadUint, loadUintBig, storeUint, load, checkInt, wrapInt, toInt, llround,
    idiv, imod, stripPadding,
    Reader, Writer,
    extractBits, insertBits, signExtend, getBits, setBits,
    checksum,
    xorEscape, mapEscape, indexEscape, NO_ESCAPE, walkEscaped, unescapeInPlace, unescape, unescapedSize,
    escapeOverhead, escapeRange, escape,
    FrameScan, findPatternAtStart, matchesAt, scanFixed, scanLengthPrefixed, scanDelimited,
    StreamParser,
    formatFloat, formatGeneral, parseFloat: parseFloatText,
    text: Object.freeze({ split: splitTokens, parseInt: parseInt_, parseFloat: parseFloatText, parseChar, parseHexByte, hexValue,
      hexText, TextWriter }),
    attempt, count, concat, fixedArray, seqArray, printNested, knownEnum, checkChar,
    Printer, print, enumType,
  });
});
