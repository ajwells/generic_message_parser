// The JavaScript version of main.cpp: talk to a Sensirion SHT4x over I2C. Encode commands, decode the 6-byte responses
// (with per-word CRC-8). The I2C transfers are simulated with captured bytes.
//   node main.js   (next to the generated sht4x.gmp.js and gmsg.js)
'use strict';

const gmsg = require('./gmsg.js');
const sht = require('./sht4x.gmp.js');

const hex = (v, n) => v.toString(16).toUpperCase().padStart(n, '0');
const out = (s) => process.stdout.write(s);
const printBytes = (what, b) => out(what.padEnd(8) + Array.from(b, (x) => ' ' + hex(x, 2)).join('') + '\n');

// 1. Command: measure T & RH with high precision (write 1 byte to 0x44, then wait 8.3 ms).
printBytes('write', new sht.MeasureHighPrecision().encode());

// 2. Response: 6 bytes read back = T word, CRC, RH word, CRC.
const rx = Uint8Array.of(0x66, 0x66, 0x93, 0x7A, 0xE1, 0xA4);
printBytes('read', rx);
let meas;
try {
  meas = sht.MeasurementResponse.decode(rx);  // a Measurement (the protocol's only message)
} catch (e) {
  out(`bad response: ${gmsg.errorText(e.code)}\n`);
  process.exit(1);
}
out(`T = ${meas.temperature_scaled.toFixed(2)} degC (${meas.temperature_degf.toFixed(2)} degF), ` +
    `RH = ${meas.humidity_scaled.toFixed(2)} %RH\n`);

// A corrupted byte is caught by the word CRC.
const bad = rx.slice();
bad[1] ^= 0x01;
const [err] = gmsg.attempt(() => sht.MeasurementResponse.parse(bad));
out(`corrupted: ${gmsg.errorText(err)}\n`);

// 3. Serial number (command 0x89, then read 6 bytes), using the zero-copy view.
const s = sht.SerialNumberView.from(Uint8Array.of(0x0F, 0x4F, 0x0B, 0x5A, 0x3C, 0x30));
// serial is a computed value: (serial_high << 16) | serial_low, declared in the schema.
out(`serial = 0x${hex(s.serial, 8)}\n`);

// The struct can also set it: the setter splits the value back into serial_high / serial_low.
const sn = new sht.SerialNumber();
sn.serial = 0x0F4F5A3C;
printBytes('serial', sn.encode());

// Encoding a response (e.g. for a sensor simulator) writes the CRCs automatically.
const sim = new sht.Measurement();
sim.temperature_scaled = 25.0;
sim.humidity_scaled = 54.0;
printBytes('encoded', sim.encode());
