// The JavaScript version of main.cpp: decode a bit-packed (Intel / lsb_first) CAN payload, and build one from values.
//   node main.js   (next to the generated can.gmp.js and gmsg.js)
'use strict';

const gmsg = require('./gmsg.js');
const vc = require('./can.gmp.js');

const hex = (v, n) => v.toString(16).toUpperCase().padStart(n, '0');
const out = (s) => process.stdout.write(s);

// As received from a CAN controller: 8 data bytes.
const payload = Uint8Array.of(0xE8, 0xC3, 0x82, 0x00, 0x02, 0x19, 0xEF, 0xBE);

let view;
try {
  view = vc.EngineDataView.from(payload);
} catch (e) {
  out(`invalid frame: ${gmsg.errorText(e.code)}\n`);
  process.exit(1);
}
const s = view.signals;
out(`rpm=${s.rpm_scaled.toFixed(1)} gear=${vc.Gear.nameOf(s.gear)} coolant=${s.coolant_scaled.toFixed(0)}C ` +
    `throttle=${s.throttle_scaled.toFixed(1)}% counter=${s.counter} flags=0x${hex(s.flags, 4)}\n`);

const m = new vc.EngineData();
m.signals.rpm = 3000 / 0.5;
m.signals.gear = vc.Gear.drive;
m.signals.coolant = 95 + 40;
const frame = m.encode();
out('encoded:' + Array.from(frame, (b) => ' ' + hex(b, 2)).join('') + '\n');
