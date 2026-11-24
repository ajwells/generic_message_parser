// The JavaScript version of main_enum.cpp: SHT4x with commands as an enum (sht4x_enum.yaml). Build a command
// sequence as data, encode each command, pick the response decoder from the command, and decode bytes written by
// someone else back to a command.
//   node main_enum.js   (next to the generated sht4x_enum.gmp.js and gmsg.js)
'use strict';

const gmsg = require('./gmsg.js');
const sht = require('./sht4x_enum.gmp.js');

const { Command } = sht;
const hex = (v, n) => v.toString(16).toUpperCase().padStart(n, '0');
const out = (s) => process.stdout.write(s);

/** What to read back after a command, derived from the enum. */
const replyFor = (c) => (c === Command.soft_reset ? 'none' : c === Command.read_serial_number ? 'serial' : 'measurement');

/** Simulated sensor: the 6 bytes it would return for a command. */
const simulatedRead = (c) => (c === Command.read_serial_number
  ? Uint8Array.of(0x0F, 0x4F, 0x0B, 0x5A, 0x3C, 0x30)
  : Uint8Array.of(0x66, 0x66, 0x93, 0x7A, 0xE1, 0xA4));  // 25.00 degC, 54.00 %RH

// A command sequence is plain data.
for (const c of [Command.soft_reset, Command.read_serial_number, Command.measure_high_precision, Command.heater_20mw_100ms]) {
  const tx = new sht.CommandFrame({ command: c }).encode();
  out(`write ${hex(tx[0], 2)}  ${Command.nameOf(c).padEnd(24)}`);
  try {
    switch (replyFor(c)) {
      case 'none': out('(no response)\n'); break;
      case 'serial': out(`serial = 0x${hex(sht.SerialNumberView.from(simulatedRead(c)).serial, 8)}\n`); break;  // computed value
      default: {
        const m = sht.MeasurementResponse.decode(simulatedRead(c));
        out(`T = ${m.temperature_scaled.toFixed(2)} degC (${m.temperature_degf.toFixed(2)} degF), ` +
            `RH = ${m.humidity_scaled.toFixed(2)} %RH\n`);
      }
    }
  } catch (e) {
    out(`bad response: ${gmsg.errorText(e.code)}\n`);
    process.exit(1);
  }
}

// Decoding a command byte (e.g. in a bus sniffer). Command is `strict: true` in the schema, so a byte that is not a
// known command fails with UnknownEnum.
for (const b of [0xE0, 0x42]) {
  const [e, f] = gmsg.attempt(() => sht.CommandFrame.decode(Uint8Array.of(b)));
  out(e ? `sniffed ${hex(b, 2)} -> error: ${gmsg.errorText(e)}\n` : `sniffed ${hex(b, 2)} -> ${Command.nameOf(f.command)}\n`);
}
