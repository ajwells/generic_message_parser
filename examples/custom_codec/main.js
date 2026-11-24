// The JavaScript version of main.cpp: a custom field codec and a custom checksum, wired in from the schema's `js:`
// blocks (meter.yaml -> meter_codecs.js).
//   node main.js   (next to the generated meter.gmp.js, gmsg.js and meter_codecs.js)
'use strict';

const gmsg = require('./gmsg.js');
const meter = require('./meter.gmp.js');

const hex2 = (b) => b.toString(16).toUpperCase().padStart(2, '0');
const two = (v) => String(v).padStart(2, '0');
const out = (s) => process.stdout.write(s);

const r = new meter.Reading({ timestamp: { year: 2026, month: 11, day: 20, hour: 14, minute: 30, second: 5 } });
r.kwh_scaled = 12345.67;

const frame = meter.MeterBus.encode(r);
out('frame:' + Array.from(frame, (b) => ' ' + hex2(b)).join('') + '\n');

let v;
try {
  v = meter.MeterBus.parse(frame);
} catch (e) {
  out(`parse failed: ${gmsg.errorText(e.code)}\n`);
  process.exit(1);
}
const ts = v.timestamp;  // custom-type accessors decode on access (and throw GmpError)
out(`${String(ts.year).padStart(4, '0')}-${two(ts.month)}-${two(ts.day)} ${two(ts.hour)}:${two(ts.minute)}:${two(ts.second)}` +
    `  ${v.kwh_scaled.toFixed(2)} kWh\n`);

// print uses the `print` hook of the custom type (meter_codecs.js)
out(gmsg.print(v) + '\n');
