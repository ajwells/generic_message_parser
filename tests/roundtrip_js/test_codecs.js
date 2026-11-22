// JavaScript versions of the user extensions in tests/roundtrip/test_codecs.hpp, referenced from the `js:` blocks of
// tests/schemas/hdlc.yaml and custom.yaml. Copied next to the generated modules with gmsg.js.
'use strict';

const gmsg = require('./gmsg.js');
const { Err, fail } = gmsg;

function fromBcd(b) {
  if ((b >> 4) > 9 || (b & 0xF) > 9) fail(Err.InvalidValue);
  return (b >> 4) * 10 + (b & 0xF);
}

/** Three BCD bytes: HH MM SS -> { h, m, s }. */
function decodeBcdTime(r) {
  return { h: fromBcd(r.u8()), m: fromBcd(r.u8()), s: fromBcd(r.u8()) };
}

function encodeBcdTime(w, t) {
  for (const v of [t.h, t.m, t.s]) {
    if (v > 99) fail(Err.OutOfRange);
    w.u8(((Math.floor(v / 10)) << 4) | (v % 10));
  }
}

/** Print hook: HH:MM:SS. */
function printTime(p, t) {
  const two = (v) => String(Math.floor(v / 10) % 10) + String(v % 10);
  p.text(`${two(t.h)}:${two(t.m)}:${two(t.s)}`);
}

/** Custom checksum: 16-bit sum of all bytes. */
function add16(d) {
  let s = 0;
  for (const b of d) s = (s + b) & 0xFFFF;
  return s;
}

/** NMEA "ddmm.mmmm" -> decimal degrees. */
function decodeCoord(s) {
  const v = gmsg.text.parseFloat(s);
  const deg = Math.floor(v / 100.0);
  return deg + (v - deg * 100.0) / 60.0;
}

function encodeCoord(w, degrees) {
  const deg = Math.floor(degrees);
  const ddmm = deg * 100.0 + (degrees - deg) * 60.0;
  w.raw(gmsg.formatFloat(ddmm, 4));
}

/** Frame = [n][n bytes]; a zero length byte is treated as garbage. */
function lenByteScan(b) {
  if (b.length === 0) return gmsg.FrameScan.needMore(1);
  if (b[0] === 0) return gmsg.FrameScan.skip(1, Err.BadLength);
  const n = 1 + b[0];
  if (b.length < n) return gmsg.FrameScan.needMore(n);
  return gmsg.FrameScan.frame(n, n, n);
}

module.exports = { decodeBcdTime, encodeBcdTime, printTime, add16, decodeCoord, encodeCoord, lenByteScan };
