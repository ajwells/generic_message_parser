// The JavaScript version of main.cpp: 0x80 is reserved as the sync byte, so every 0x80 / 0x81 inside a frame is sent
// as 81 00 / 81 01. Encode a few frames, show the escaping, then cut one frame short and let the stream parser
// resynchronise on the next 0x80.
//   node main.js   (next to the generated motor_link.gmp.js and gmsg.js)
'use strict';

const gmsg = require('./gmsg.js');
const motor = require('./motor_link.gmp.js');

const Link = motor.MotorLink;
const hex2 = (b) => b.toString(16).toUpperCase().padStart(2, '0');
const out = (s) => process.stdout.write(s);

function encode(msg) {
  try {
    return Link.encode(msg);
  } catch (e) {
    out(`encode failed: ${gmsg.errorText(e.code)}\n`);
    return new Uint8Array(0);
  }
}

/** Print a wire frame with escape pairs in brackets. */
function printWire(label, f) {
  let s = label.padEnd(10);
  for (let i = 0; i < f.length; i++) {
    if (i > 0 && f[i] === 0x81 && i + 1 < f.length) {
      s += ` [${hex2(f[i])} ${hex2(f[i + 1])}]`;
      i++;
    } else {
      s += ' ' + hex2(f[i]);
    }
  }
  out(`${s}   (${f.length} bytes)\n`);
}

// --- encoding: reserved bytes in the payload are escaped ----------------------------------------------------------
const setSpeed = encode(new motor.SetSpeed({ rpm: 1920, direction: motor.Direction.forward, ramp_ms: 385 }));

const t = new motor.Telemetry({ seq: 128, rpm: 1920 });  // seq 0x0080
t.current_scaled = 3.21;
t.temperature_scaled = 38.5;  // 385 = 0x0181
const telemetry = encode(t);

const logFrame = encode(new motor.Log({ text: 'rpm limit reached' }));  // `len` is filled in automatically

t.seq = 129;
const telemetry2 = encode(t);

out('Escaped frames (escape pairs in brackets: [81 00] = 0x80, [81 01] = 0x81):\n');
printWire('SetSpeed', setSpeed);
printWire('Telemetry', telemetry);
printWire('Log', logFrame);

// --- whole-buffer decoding (JavaScript unescapes into a copy, so the frame is left as it was) ---------------------
const v = Link.parse(setSpeed);
if (v instanceof motor.SetSpeedView)
  out(`\nparse(): SetSpeed rpm=${v.rpm} direction=${motor.Direction.nameOf(v.direction)} ramp=${v.ramp_ms}ms\n`);

// --- streaming: a frame cut short, then line noise ----------------------------------------------------------------
const wire = Uint8Array.from([...telemetry, ...logFrame.subarray(0, 9), ...telemetry2, 0x13, 0x37, ...logFrame]);

out(`\nStream of ${wire.length} bytes, fed in 3-byte chunks:\n`);
const parser = Link.stream();
parser.on('Telemetry', (m) => out(`  Telemetry seq=${m.seq} rpm=${m.rpm} current=${m.current_scaled.toFixed(2)}A ` +
  `temperature=${m.temperature_scaled.toFixed(1)}C\n`));
parser.on('Log', (m) => out(`  Log "${m.text}"\n`));
parser.on('error', (e) => out(`  dropped ${e.bytes.length} byte(s): ${gmsg.errorText(e.code)}\n`));
for (let off = 0; off < wire.length; off += 3) parser.feed(wire.subarray(off, off + 3));

const st = parser.stats;
out(`frames=${st.frames} errors=${st.frameErrors} skipped=${st.bytesSkipped}\n`);
