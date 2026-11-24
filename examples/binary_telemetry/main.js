// The JavaScript version of main.cpp: encode a few frames, add line noise, and recover the messages with the stream
// parser. Same schema (telemetry.yaml), generated with `gmpc --lang js`; run next to the generated module and gmsg.js:
//   node main.js
'use strict';

const gmsg = require('./gmsg.js');
const demo = require('./telemetry.gmp.js');

const hex4 = (v) => v.toString(16).toUpperCase().padStart(4, '0');
const out = (s) => process.stdout.write(s);

const wire = [];
const append = (msg) => wire.push(...demo.Link.encode(msg));

const ok = new demo.Status({ uptime_s: 3600 });
ok.voltage_scaled = 12.6;
ok.state.armed = true;
ok.state.mode = demo.Mode.run;
append(ok);

wire.push(0x13, 0x37, 0xAA);  // line noise

append(new demo.Samples({ channel: 3, values: [100, -100, 250] }));  // `count` is filled in automatically

const bad = new demo.Status({ uptime_s: ok.uptime_s, voltage: ok.voltage });
bad.state.armed = true;
bad.state.mode = demo.Mode.fault;
bad.fault = new demo.Fault({ code: 0x0042, temperature: 875 });
append(bad);

// Listeners per message name; errors are GmpError objects with .code and .bytes.
const parser = demo.Link.stream();
parser.on('Status', (v) => {
  out(`Status  uptime=${v.uptime_s}s voltage=${v.voltage_scaled.toFixed(3)}V mode=${demo.Mode.nameOf(v.state.mode)}`);
  const f = v.fault;
  if (f) out(` fault=0x${hex4(f.code)} temp=${f.temperature_scaled.toFixed(1)}C`);
  out('\n');
});
// gmsg.print dumps every field (enum names, units, nested messages); see README "Printing"
parser.on('Samples', (v) => out(gmsg.print(v, { style: 'single_line' }) + '\n'));
parser.on('error', (e) => out(`dropped ${e.bytes.length} byte(s): ${gmsg.errorText(e.code)}\n`));

// Deliver the stream in awkward 5-byte chunks, as a UART driver might.
const all = Uint8Array.from(wire);
for (let off = 0; off < all.length; off += 5) parser.feed(all.subarray(off, off + 5));

const st = parser.stats;
out(`frames=${st.frames} errors=${st.frameErrors} skipped=${st.bytesSkipped}\n`);
