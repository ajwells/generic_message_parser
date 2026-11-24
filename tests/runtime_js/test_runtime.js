// Tests of runtime-js/gmsg.js: unit checks, and a cross-check against the C++ runtime using the reference vectors
// printed by js_vectors (built from tests/runtime_js/js_vectors.cpp).
//
//   node tests/runtime_js/test_runtime.js <path to js_vectors executable> [-v]
'use strict';

const path = require('path');
const { execFileSync } = require('child_process');
const gmsg = require(path.join(__dirname, '..', '..', 'runtime-js', 'gmsg.js'));
const T = require('./harness.js');

const { Err } = gmsg;
const hexToBytes = (h) => Uint8Array.from(h.match(/../g) || [], (x) => parseInt(x, 16));
const bytesToHex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const f64 = (hex) => { const d = new DataView(new ArrayBuffer(8)); d.setBigUint64(0, BigInt('0x' + hex)); return d.getFloat64(0); };
const f32 = (hex) => { const d = new DataView(new ArrayBuffer(4)); d.setUint32(0, Number(BigInt('0x' + hex))); return d.getFloat32(0); };
const bitsHex = (x, isF32) => {
  const d = new DataView(new ArrayBuffer(8));
  if (isF32) { d.setFloat32(0, x); return d.getUint32(0).toString(16).padStart(16, '0'); }
  d.setFloat64(0, x); return d.getBigUint64(0).toString(16).padStart(16, '0');
};
/** Run fn and return the GmpError code it throws, or 0 and the result. */
const attempt = (fn) => {
  try { return [0, fn()]; } catch (e) { if (e instanceof gmsg.GmpError) return [e.code, undefined]; throw e; }
};

// ==================================================================================================================
// unit checks
// ==================================================================================================================
T.test('error codes and texts match gmsg::Error', () => {
  T.eq(Err.UnknownEnum, 14);
  T.eq(Err.ValueOutOfLimits, 15);
  T.eq(Err.Overrun, 16);
  T.eq(gmsg.errorText(Err.ValueOutOfLimits), 'value out of limits');
  T.eq(gmsg.errorText(Err.BadChecksum), 'bad checksum');
  T.eq(gmsg.errorText(Err.UnknownType), 'unknown message type');
  const e = new gmsg.GmpError(Err.Truncated);
  T.eq(e.message, 'truncated');
  T.eq(e instanceof Error, true);
  T.eq(new gmsg.GmpError(Err.OutOfRange, 'x').message, 'out of range: x');
});

T.test('Reader / Writer round trip every type in both byte orders', () => {
  for (const e of ['big', 'little']) {
    const w = new gmsg.Writer(1);
    w.u8(0xAB); w.i8(-2); w.u16(0x1234, e); w.i16(-3, e); w.u32(0xDEADBEEF, e); w.i32(-4, e);
    w.u64(0xFFFFFFFFFFFFFFFFn, e); w.i64(-5n, e); w.f32(0.5, e); w.f64(Math.PI, e); w.bool(true); w.char('A');
    const r = new gmsg.Reader(w.toBytes());
    T.eq(r.u8(), 0xAB); T.eq(r.i8(), -2); T.eq(r.u16(e), 0x1234); T.eq(r.i16(e), -3); T.eq(r.u32(e), 0xDEADBEEF);
    T.eq(r.i32(e), -4); T.eq(r.u64(e), 0xFFFFFFFFFFFFFFFFn); T.eq(r.i64(e), -5n); T.eq(r.f32(e), 0.5);
    T.eq(r.f64(e), Math.PI); T.eq(r.bool(), true); T.eq(r.char(), 'A'); T.eq(r.remaining, 0);
    T.eq(attempt(() => r.u8())[0], Err.Truncated);
  }
  const w = new gmsg.Writer();
  w.u16(0x0102, 'big');
  T.eq(bytesToHex(w.toBytes()), '0102');
  T.eq(attempt(() => w.u8(256))[0], Err.OutOfRange);
  T.eq(attempt(() => w.i8(-129))[0], Err.OutOfRange);
  T.eq(attempt(() => w.u16(1.5))[0], Err.OutOfRange);
  T.eq(attempt(() => w.u64(-1n))[0], Err.OutOfRange);
  T.eq(attempt(() => w.u64(5))[0], 0);  // integral Numbers are accepted for 64-bit types
});

T.test('wrapInt / checkInt follow C++ conversions', () => {
  T.eq(gmsg.wrapInt(300, 'u8'), 44);
  T.eq(gmsg.wrapInt(-1, 'u16'), 65535);
  T.eq(gmsg.wrapInt(200, 'i8'), -56);
  T.eq(gmsg.wrapInt(-1, 'u64'), 18446744073709551615n);
  T.eq(gmsg.wrapInt(2.9, 'i32'), 2);
});

T.test('checksum check values (reveng catalogue, "123456789")', () => {
  const data = gmsg.latin1Bytes('123456789');
  const check = {
    crc8: 0xF4, crc8_maxim: 0xA1, crc8_autosar: 0xDF, crc8_nrsc5: 0xF7, crc15_can: 0x059E, crc16_ccitt: 0x29B1,
    crc16_xmodem: 0x31C3, crc16_kermit: 0x2189, crc16_x25: 0x906E, crc16_modbus: 0x4B37, crc16_arc: 0xBB3D,
    crc24_openpgp: 0x21CF02, crc32: 0xCBF43926, crc32c: 0xE3069283, crc32_mpeg2: 0x0376E6E7,
    crc64_ecma: 0x6C40DF5F0B497347n,
  };
  for (const [name, v] of Object.entries(check)) T.eq(gmsg.checksum[name].compute(data), v, name);
  // incremental use gives the same result
  const a = gmsg.checksum.crc32;
  T.eq(a.finish(a.update(a.update(a.init(), data, 0, 4), data, 4)), 0xCBF43926);
});

T.test('float text examples', () => {
  T.eq(gmsg.formatFloat(Math.fround(0.9), -1, true), '0.9');
  T.eq(gmsg.formatFloat(0.1 + 0.2), '0.30000000000000004');
  T.eq(gmsg.formatFloat(1e21), '1e+21');
  T.eq(gmsg.formatFloat(123456), '123456');
  T.eq(gmsg.formatFloat(441382624, -1, true), '441382624');  // exact integer for floats whose gaps are 2+
  T.eq(gmsg.formatFloat(2.5, 0), '2');  // ties to even
  T.eq(gmsg.formatFloat(-0), '-0');
  T.eq(gmsg.formatGeneral(1234567885, 9), '1.23456788e+09');  // exact tie rounds half to even
  T.eq(gmsg.formatGeneral(0.0001, 9), '0.0001');
  T.eq(gmsg.parseFloat('+1.5'), 1.5);
  T.eq(attempt(() => gmsg.parseFloat(''))[0], Err.MissingField);
  T.eq(attempt(() => gmsg.parseFloat('1e400'))[0], Err.OutOfRange);
  T.eq(attempt(() => gmsg.parseFloat(' 1'))[0], Err.ParseError);
});

T.test('printer: enum widths, lists and notes', () => {
  const p = new gmsg.Printer({ style: 'single_line' });
  p.begin('M');
  p.key('e'); p.enumValue('', -1, 2);
  p.key('v'); p.int(5); p.conversion(0.5, 'V'); p.note('computed');
  p.end();
  T.eq(p.out, 'M { e: <unknown> (0xFFFF), v: 5 (0.5 V, computed) }');
});

T.test('enumType', () => {
  const Mode = gmsg.enumType('Mode', 'u8', [['idle', 0], ['run', 1]], true);
  T.eq(Mode.run, 1);
  T.eq(Mode.nameOf(1), 'run');
  T.eq(Mode.nameOf(9), '');
  T.eq(Mode.isKnown(0), true);
  T.eq(Object.keys(Mode).join(','), 'idle,run');
  T.eq(Object.isFrozen(Mode), true);
});

T.test('stream parser: listeners, pull API and iterators', () => {
  // frames: 0x24 <len> <payload...> ; payload byte 0 odd = bad
  class View {
    static messageName = 'Ping';
    constructor(b) { this.b = b; }
    toStruct() { if (this.b[2] === 0xFE) gmsg.fail(Err.OutOfRange); return { n: this.b[2] }; }
  }
  const protocol = {
    maxFrame: 16,
    scan: (b) => gmsg.scanLengthPrefixed(b, { sync: Uint8Array.of(0x24), lengthOffset: 1, lengthSize: 1, add: 2, maxFrame: 16 }),
    process: (f) => { if (f[2] & 1) gmsg.fail(Err.BadChecksum); return { header: { len: f[1] }, message: new View(f) }; },
  };
  const data = Uint8Array.of(0x24, 1, 2, 0x99, 0x24, 1, 3, 0x24, 2, 4, 4, 0x24, 1, 0xFE);
  const s = new gmsg.StreamParser(protocol);
  const seen = [];
  s.on('Ping', (v, h) => seen.push(`P${v.b[2]}/${h.len}`));
  s.on('frame', () => seen.push('f'));
  const off = s.on('error', (e) => seen.push(`E${e.code}:${e.bytes.length}`));
  T.eq(s.feed(data), 3);
  // after the bad checksum one byte is dropped and the parser resyncs on the next 0x24 (BadSync for 01 03)
  T.eq(seen.join(' '), 'P2/1 f E2:1 E3:3 E2:2 P4/2 f P254/1 f');
  T.eq(s.stats.frames, 3);
  T.eq(s.stats.frameErrors, 1);
  off();

  const p = new gmsg.StreamParser(protocol);
  T.eq(Array.from(p.messages(data), (v) => v.b[2]).join(','), '2,4,254');
  T.eq(p.lastError, Err.BadSync);
  const q = new gmsg.StreamParser(protocol);
  T.eq(Array.from(q.events(data), (e) => (e.error ? 'E' + e.error.code : 'M' + e.message.b[2])).join(','), 'M2,E2,E3,E2,M4,M254');
  const r = new gmsg.StreamParser(protocol);
  const errs = [];
  r.on('error', (e) => errs.push(e.code));
  r.push(data);
  T.eq(r.pollMessage().n, 2);
  T.eq(r.pollMessage().n, 4);
  T.eq(r.pollMessage(), null);  // 0xFE: struct decode fails
  T.eq(r.stats.frameErrors, 2);
  // the struct failure is reported, then one byte is dropped and 01 FE is skipped (BadSync)
  T.eqList(errs, [Err.BadSync, Err.BadChecksum, Err.BadSync, Err.OutOfRange, Err.BadSync]);
  T.eq(r.lastError, Err.BadSync);
  const t = new gmsg.StreamParser(protocol);
  const other = [];
  t.on('frame', (v) => other.push(v.b[2]));
  t.push(data);
  T.eq(t.pollFor('Ping').b[2], 2);
  const fr = t.pollFrame();
  T.eq(bytesToHex(fr.bytes), '24020404');
  T.eq(fr.header.len, 2);
  T.eq(fr.offset, 7);  // stream position of the frame

  // push() compacts the buffer when the new bytes would not fit behind the pending ones
  const c = new gmsg.StreamParser(protocol, { capacity: 8 });
  T.eq(c.push(Uint8Array.of(0x24, 1, 2, 0x24, 1)), 5);
  T.eq(c.poll().b[2], 2);
  T.eq(c.push(Uint8Array.of(4, 0x24, 2, 6, 6)), 5);  // 2 pending + 5 new fit only after compaction
  T.eq(c.poll().b[2], 4);
  T.eq(c.poll().b[2], 6);
});

// ==================================================================================================================
// cross-check against the C++ runtime
// ==================================================================================================================
const exe = process.argv.slice(2).find((a) => !a.startsWith('-'));
let V = null;
if (exe) V = JSON.parse(execFileSync(exe, { maxBuffer: 1 << 30 }).toString('latin1'));
const vectors = (name, fn) => T.test(name, () => {
  if (!V) return T.skip('no js_vectors executable given');
  fn();
});

vectors('checksum presets match C++', () => {
  for (const [name, hex, value] of V.checksums) T.eq(String(gmsg.checksum[name].compute(hexToBytes(hex))), value, `${name} ${hex}`);
});

vectors('generic CRCs with random parameters match the C runtime', () => {
  for (const [width, poly, init, refin, refout, xorout, hex, value] of V.crc_generic) {
    const big = width > 32;
    const a = gmsg.checksum.crc({ width, poly: big ? BigInt(poly) : Number(poly), init: big ? BigInt(init) : Number(init), refin, refout,
      xorout: big ? BigInt(xorout) : Number(xorout) });
    T.eq(String(a.compute(hexToBytes(hex))), value, `width ${width} poly ${poly} refin ${refin} refout ${refout}`);
  }
});

vectors('bit extraction, sign extension and insertion match C++', () => {
  for (const [hex, off, n, order, v, sv, value, written] of V.bits) {
    const b = hexToBytes(hex);
    T.eq(String(gmsg.extractBits(b, off, n, order)), v, `extract ${hex} ${off} ${n} ${order}`);
    T.eq(String(gmsg.getBits(b, off, n, order, 'i64')), sv, `signed ${hex} ${off} ${n}`);
    if (n <= 32) T.eq(String(gmsg.getBits(b, off, n, order, n <= 8 ? 'u8' : n <= 16 ? 'u16' : 'u32')), v);
    const w = b.slice();
    gmsg.setBits(w, off, n, order, BigInt(value));
    T.eq(bytesToHex(w), written, `insert ${hex} ${off} ${n} ${order}`);
  }
});

vectors('float formatting matches std::to_chars', () => {
  for (const [kind, hex, mode, precision, text] of V.float_format) {
    const isF32 = kind === 'f32';
    const x = isF32 ? f32(hex.slice(8)) : f64(hex);
    const got = mode === 0 ? gmsg.formatFloat(x, -1, isF32) : mode === 1 ? gmsg.formatFloat(x, precision, isF32) : gmsg.formatGeneral(x, precision);
    T.eq(got, text, `${kind} ${hex} mode ${mode} precision ${precision}`);
  }
});

vectors('float parsing matches std::from_chars', () => {
  for (const [s, de, dbits, fe, fbits] of V.float_parse) {
    const [dc, dv] = attempt(() => gmsg.parseFloat(s, false));
    T.eq(dc, de, `f64 error for "${s}"`);
    if (!dc && !Number.isNaN(dv)) T.eq(bitsHex(dv, false), dbits, `f64 bits for "${s}"`);
    const [fc, fv] = attempt(() => gmsg.parseFloat(s, true));
    T.eq(fc, fe, `f32 error for "${s}"`);
    if (!fc && !Number.isNaN(fv)) T.eq(bitsHex(fv, true), fbits, `f32 bits for "${s}"`);
  }
});

vectors('integer parsing matches gmsg::text::parse_int', () => {
  for (const [type, s, base, err, value] of V.int_parse) {
    const [c, v] = attempt(() => gmsg.text.parseInt(s, type, base));
    T.eq(c, err, `${type} "${s}" base ${base}`);
    if (!c) T.eq(String(v), value, `${type} "${s}" base ${base}`);
  }
});

vectors('TextWriter tokens match C++', () => {
  for (const [kind, value, a, width, upper, text] of V.text_writer) {
    const w = new gmsg.text.TextWriter(',');
    if (kind === 'int') w.intToken(BigInt(value), a, width, upper);
    else w.floatToken(kind === 'f32' ? f32(value.slice(8)) : f64(value), a, width, kind === 'f32');
    T.eq(w.toString(), text, `${kind} ${value} ${a} ${width} ${upper}`);
  }
});

// the scanner configs of js_vectors.cpp
const X = { xor: gmsg.xorEscape(0x7D, 0x20, [0x7E]), index: gmsg.indexEscape(0x81, [0x80, 0x81]),
  map: gmsg.mapEscape(0xDB, [0xC0, 0xDB], [0xDC, 0xDD]) };
const ESC = [X.xor, X.index, X.map];
const P = (...b) => Uint8Array.from(b);
const SCAN = [
  (b) => gmsg.scanFixed(b, { sync: P(0xAA, 0x55), frameSize: 6 }),
  (b) => gmsg.scanFixed(b, { frameSize: 4 }),
  (b) => gmsg.scanFixed(b, { sync: P(0x80), frameSize: 5, escape: X.index }),
  (b) => gmsg.scanLengthPrefixed(b, { sync: P(0xAA, 0x55), lengthOffset: 2, lengthSize: 1, lengthEndian: 'big', add: 5, minFrame: 5, maxFrame: 24 }),
  (b) => gmsg.scanLengthPrefixed(b, { lengthOffset: 0, lengthSize: 2, lengthEndian: 'little', add: -1, minFrame: 3, maxFrame: 20 }),
  (b) => gmsg.scanLengthPrefixed(b, { sync: P(0x7E), lengthOffset: 1, lengthSize: 1, add: 4, minFrame: 4, maxFrame: 32, escape: X.xor }),
  (b) => gmsg.scanLengthPrefixed(b, { sync: P(0x7E), lengthOffset: 1, lengthSize: 1, add: 0, minFrame: 4, maxFrame: 32, escape: X.xor,
    countsWire: true, headerLen: 2, trailerLen: 1 }),
  (b) => gmsg.scanDelimited(b, { start: P(0x7E), end: P(0x7E), escape: X.xor, maxFrame: 24 }),
  (b) => gmsg.scanDelimited(b, { start: P(0x24), end: P(0x0D, 0x0A), maxFrame: 16 }),
];
const KIND = { need_more: 0, frame: 1, skip: 2 };

vectors('framing scanners match C++', () => {
  for (const [which, hex, kind, size, consume, onError, error] of V.scan) {
    const s = SCAN[which](hexToBytes(hex));
    T.eqList([KIND[s.kind], s.size, s.consume, s.consumeOnError, s.error], [kind, size, consume, onError, error], `config ${which} ${hex}`);
  }
});

vectors('escaping matches C++', () => {
  const WALK = { done: 0, need_more: 1, bad: 2 };
  for (const [which, hex, uerr, unesc, esc, count, pickAt, wkind, wire, werr, pick] of V.escape) {
    const cfg = ESC[which], b = hexToBytes(hex);
    const [c, u] = attempt(() => gmsg.unescape(b, cfg));
    T.eq(c, uerr, `unescape ${which} ${hex}`);
    if (!c) T.eq(bytesToHex(u), unesc, `unescape ${which} ${hex}`);
    const padded = new Uint8Array(b.length + 2).fill(0xEE);
    padded.set(b, 2);
    T.eq(bytesToHex(gmsg.escapeRange(padded, 2, b.length, cfg)), esc, `escape ${which} ${hex}`);
    const pk = new Uint8Array(4);
    const w = gmsg.walkEscaped(b, 0, count, cfg, pickAt, pk);
    T.eqList([WALK[w.kind], w.wire, w.error], [wkind, wire, werr], `walk ${which} ${hex} ${count}`);
    if (w.kind === 'done') T.eq(bytesToHex(pk), pick, `walk pick ${which} ${hex}`);
  }
});

vectors('stream parser matches gmsg::StreamParser (events, resync, statistics)', () => {
  class Frame { static messageName = 'Frame'; constructor(b) { this.b = b; } }
  for (const [which, capacity, chunk, hex, events, stats] of V.stream) {
    const protocol = {
      maxFrame: capacity,
      scan: SCAN[which],
      process: (f) => { if (!f.length || (f[f.length - 1] & 1)) gmsg.fail(Err.BadChecksum); return { message: new Frame(f) }; },
    };
    const s = new gmsg.StreamParser(protocol, { capacity });
    let ev = '';
    s.on('frame', (v) => { ev += `F${v.b.length} `; });
    s.on('error', (e) => { ev += `E${e.code}:${e.bytes.length} `; });
    const data = hexToBytes(hex);
    for (let pos = 0; pos < data.length; pos += chunk) s.feed(data.subarray(pos, Math.min(data.length, pos + chunk)));
    T.eq(ev, events, `config ${which} capacity ${capacity} chunk ${chunk}`);
    T.eqList([s.stats.frames, s.stats.frameErrors, s.stats.bytesSkipped, s.stats.overflows], stats, `stats ${which} ${capacity} ${chunk}`);
  }
});

vectors('printer matches gmsg::Printer for every option set', () => {
  const scenario = (p) => {
    const inner = (id) => ({ printTo(q) { q.begin('Inner'); q.key('id'); q.int(id); q.end(); } });
    p.begin('Scenario');
    p.key('u'); p.int(4000000000);
    p.key('i'); p.int(-12);
    p.key('big'); p.int(18446744073709551615n);
    p.key('f32'); p.float(Math.fround(0.9), true);
    p.key('f64'); p.float(1e21);
    p.key('tiny'); p.float(5e-324);
    p.key('flag'); p.bool(true);
    p.key('ch'); p.char("'");
    p.key('ch2'); p.char(7);
    p.key('mode'); p.enumValue('run', 1, 1);
    p.key('bad_mode'); p.enumValue('', 7, 1);
    p.key('raw'); p.int(12600); p.unit('mV');
    p.key('scaled'); p.int(12600); p.conversion(12.6, 'V'); p.conversion(1234567885, 'x'); p.conversion(0.000012345678901, '');
    p.key('bytes'); p.hex(P(0, 1, 0xAB, 0xFF, 0x10));
    p.key('text'); p.str('a"b\\c\n\x01');
    p.absent('maybe');
    p.key('ints'); p.list([1, 2, 3, 4, 5], (v) => p.int(v));
    p.key('empty_list'); p.list([], (v) => p.int(v));
    p.key('nested'); p.list([inner(1), inner(2), inner(3)], (v) => v.printTo(p), true);
    p.key('empty'); p.begin('Empty'); p.end();
    p.key('bits'); p.begin(''); p.key('a'); p.int(1); p.key('b'); p.bool(false); p.end();
    p.key('total'); p.int(42); p.unit('ms'); p.note('computed');
    p.key('err'); p.error(Err.ParseError);
    p.end();
  };
  for (const [style, indent, maxItems, maxBytes, precision, text] of V.print) {
    const got = gmsg.print({ printTo: scenario }, { style, indent, maxItems, maxBytes, precision });
    T.eq(got, text, `${style} indent ${indent} items ${maxItems} bytes ${maxBytes} precision ${precision}`);
  }
});

// ---- ByteRing ------------------------------------------------------------------------------------------------------
T.test('ByteRing: push, wrap, readable parts, full, drop and overrun', () => {
  const r = new gmsg.ByteRing(8);
  T.eq(r.capacity, 8);
  T.eq(r.empty, true);
  for (let i = 0; i < 6; i++) T.eq(r.push(i), true);
  r.consume(5);
  T.eq(r.pushBytes([10, 11, 12, 13, 14, 15, 16]), 7);
  T.eq(r.size, 8);
  T.eq(r.highWater, 8);
  T.eq(r.readable().length, 3);
  T.eqList(Array.from(r.read()), [5, 10, 11, 12, 13, 14, 15, 16]);
  T.eq(r.overrunPending, false);
  for (let i = 0; i < 8; i++) r.push(i);
  T.eq(r.push(99), false);
  T.eq(r.overrunPending, true);
  r.consume(4);
  T.eq(r.push(98), false);
  T.eq(r.pushBytes([1, 2, 3, 4, 5, 6, 7]), 0);
  T.eq(r.dropped, 9);
  r.consume(4);
  r.clearOverrun();
  T.eq(r.push(7), true);
  T.eq(r.pushBytes(new Uint8Array(10)), 7);
  T.eq(r.dropped, 12);
  T.eq(attempt(() => new gmsg.ByteRing(6))[0], Err.OutOfRange);
  // a shared ring attached elsewhere sees the same state
  const s = new gmsg.ByteRing(16, { shared: true });
  T.eq(s.buffer instanceof SharedArrayBuffer, true);
  const t2 = gmsg.ByteRing.attach(s.buffer);
  s.pushBytes([1, 2, 3]);
  T.eqList(Array.from(t2.read()), [1, 2, 3]);
  T.eq(s.empty, true);
  // the free-running indices wrap at 2^32
  const w = new gmsg.ByteRing(4);
  w.h[0] = w.h[1] = -2;  // both indices at 2^32 - 2
  for (let i = 0; i < 10; i++) { T.eq(w.pushBytes([i, i + 1, i + 2]), 3); T.eqList(Array.from(w.read()), [i, i + 1, i + 2]); }
});

/** 0x24 <len> <payload...>: a Ping per frame; payload byte 0 odd = bad checksum. */
const ringProtocol = (() => {
  class Ping { static messageName = 'Ping'; constructor(b) { this.b = b; } }
  return {
    maxFrame: 16,
    scan: (b) => gmsg.scanLengthPrefixed(b, { sync: Uint8Array.of(0x24), lengthOffset: 1, lengthSize: 1, add: 2, maxFrame: 16 }),
    process: (f) => { if (f[2] & 1) gmsg.fail(Err.BadChecksum); return { message: new Ping(f) }; },
  };
})();

T.test('StreamParser drains a ByteRing; an overrun drops the frame in progress', () => {
  const ping = (n) => [0x24, 2, 2 * n, 0];        // 4 bytes
  const ping5 = (n) => [0x24, 3, 2 * n, 0, 0];    // 5 bytes
  const ring = new gmsg.ByteRing(16);
  const s = new gmsg.StreamParser(ringProtocol);
  const seen = [];
  s.on('Ping', (v) => seen.push(`P${v.b[2] / 2}`));
  s.on('error', (e) => seen.push(`E${e.code}:${e.bytes.length}@${e.offset}`));
  // a gap between frames: 16 bytes fill the ring exactly, the next frame is dropped
  T.eq(ring.pushBytes([...ping(1), ...ping(2), ...ping(3), ...ping(4)]), 16);
  T.eq(ring.overrunPending, false);
  T.eq(ring.pushBytes(ping(5)), 0);
  T.eq(ring.overrunPending, true);
  T.eq(s.feed(ring), 4);
  T.eq(seen.join(' '), 'P1 P2 P3 P4 E16:0@16');
  T.eq(s.stats.overruns, 1);
  T.eq(s.lastError, Err.Overrun);
  T.eq(ring.overrunPending, false);
  // a byte limit, then a gap at a frame boundary again (14 + 2 bytes fit, the rest of the push is dropped)
  seen.length = 0;
  T.eq(ring.pushBytes([...ping(6), ...ping(7), ...ping(8), 0x24, 2]), 14);
  T.eq(ring.pushBytes([18, 0, ...ping(10)]), 2);
  T.eq(s.feed(ring, 5), 1);  // only 5 bytes are taken
  T.eq(ring.size, 11);
  T.eq(s.feed(ring), 3);
  T.eq(seen.join(' '), 'P6 P7 P8 P9 E16:0@32');
  // pull API: a gap inside a frame; push(ring) + pollEvent() returns the overrun inline, with the frame's first byte
  T.eq(ring.pushBytes([...ping5(1), ...ping5(2), ...ping5(3), ...ping5(4)]), 16);
  T.eq(s.push(ring), 16);
  T.eq(ring.overrunPending, false);  // the gap is now in the parser
  const ev = [];
  for (let e; (e = s.pollEvent());) ev.push(e.error ? `E${e.error.code}:${e.error.bytes.length}` : `M${e.message.b[2] / 2}`);
  T.eq(ev.join(' '), 'M1 M2 M3 E16:1');
  T.eq(s.stats.overruns, 3);
  T.eq(ring.push(1), true);  // input flows again
  // flush with a gap waiting: the overrun is reported first
  s.reset();
  ring.read();
  ring.pushBytes([...ping5(1), ...ping5(2), ...ping5(3), ...ping5(4)]);
  s.push(ring);
  seen.length = 0;
  T.eq(s.resync(), false);
  T.eq(s.flush(), true);
  T.eq(seen.join(' '), 'E16:16@0');
  T.eq(s.buffered, 0);
});

T.test('StreamParser: bytes pushed after a taken-over gap (scan stops at the gap; compaction moves it)', () => {
  for (const capacity of [40, 16]) {
    const ring = new gmsg.ByteRing(16);
    const s = new gmsg.StreamParser(ringProtocol, { capacity });
    const seen = [];
    s.on('Ping', (v) => seen.push(`P${v.b[2] / 2}`));
    s.on('error', (e) => seen.push(`E${e.code}:${e.bytes.length}`));
    ring.pushBytes([0x24, 2, 2, 0, 0x24, 2, 4, 0]);
    T.eq(ring.pushBytes([0x24, 8, 6, 0, 0, 0, 0, 0, 0, 0]), 8);  // the last 2 bytes are dropped
    T.eq(s.push(ring), 16);
    T.eq(s.next(), true);  // P1
    T.eq(s.push([0x24, 2, 8, 0]), 4);  // capacity 16: compacts
    while (s.next()) { /* deliver */ }
    T.eq(seen.join(' '), 'P1 P2 E16:8 P4', `capacity ${capacity}`);
  }
});

T.test('ByteRing filled by a worker thread (SharedArrayBuffer)', async () => {
  const { Worker } = require('worker_threads');
  const count = 20000;
  const ring = new gmsg.ByteRing(64, { shared: true });
  const done = new Int32Array(new SharedArrayBuffer(4));
  const worker = new Worker(`
    const { workerData } = require('worker_threads');
    const gmsg = require(workerData.gmsg);
    const ring = gmsg.ByteRing.attach(workerData.buffer);
    const done = new Int32Array(workerData.done);
    let seed = 1;
    const frame = (i) => [0x24, 2, (i & 0x7F) << 1, (i >> 7) & 0xFF];
    for (let i = 0; i < workerData.count; i++) {
      let f = frame(i);
      while (f.length) {  // wait for room: a waiting producer never drops
        seed = (seed * 1103515245 + 12345) >>> 0;
        const n = Math.min(f.length, 1 + seed % 4, ring.capacity - ring.size);
        f = f.slice(ring.pushBytes(f.slice(0, n)));
      }
    }
    Atomics.store(done, 0, 1);
  `, { eval: true, workerData: { gmsg: path.join(__dirname, '..', '..', 'runtime-js', 'gmsg.js'), buffer: ring.buffer,
    done: done.buffer, count } });
  const exited = new Promise((resolve) => worker.on('exit', resolve));
  const s = new gmsg.StreamParser(ringProtocol);
  const ids = [];
  s.on('Ping', (v) => ids.push((v.b[2] >> 1) | (v.b[3] << 7)));
  let errors = 0;
  s.on('error', () => errors++);
  while (!Atomics.load(done, 0) || !ring.empty) s.feed(ring, 50);
  await exited;
  T.eq(ring.dropped, 0);
  T.eq(errors, 0);
  T.eq(ids.length, count);
  T.eq(ids.every((id, i) => id === i), true);
});

T.run();
