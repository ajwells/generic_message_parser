// User-supplied JavaScript codecs referenced from the `js:` blocks of meter.yaml (the JavaScript twin of
// meter_codecs.hpp). Loaded by the generated meter.gmp.js with require('./meter_codecs.js').
'use strict';

const gmsg = require('./gmsg.js');

const bcd = (b) => (b >> 4) * 10 + (b & 0x0F);
const toBcd = (v) => (Math.floor(v / 10) << 4) | (v % 10);

/** YY MM DD hh mm ss, BCD encoded -> { year, month, day, hour, minute, second }. */
function decodeBcd(r) {
  const d = [r.u8(), r.u8(), r.u8(), r.u8(), r.u8(), r.u8()];
  const t = { year: 2000 + bcd(d[0]), month: bcd(d[1]), day: bcd(d[2]), hour: bcd(d[3]), minute: bcd(d[4]), second: bcd(d[5]) };
  if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31) gmsg.fail(gmsg.Err.InvalidValue);
  return t;
}

function encodeBcd(w, t) {
  for (const v of [t.year - 2000, t.month, t.day, t.hour, t.minute, t.second]) w.u8(toBcd(v));
}

/** Print hook: 2026-11-20 14:30:05. Without it the field prints as <?>. */
function printDateTime(p, t) {
  const two = (v) => String(v).padStart(2, '0');
  p.text(`${String(t.year).padStart(4, '0')}-${two(t.month)}-${two(t.day)} ${two(t.hour)}:${two(t.minute)}:${two(t.second)}`);
}

/** Custom checksum: modulo-256 sum. */
function mod256(d) {
  let s = 0;
  for (const b of d) s = (s + b) & 0xFF;
  return s;
}

module.exports = { decodeBcd, encodeBcd, printDateTime, mod256 };
