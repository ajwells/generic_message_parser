// The JavaScript version of main.cpp: parse NMEA 0183 sentences (text protocol, XOR checksum, optional fields), print
// one, and encode one. Uses the iterator form of the pull API.
//   node main.js   (next to the generated nmea.gmp.js and gmsg.js)
'use strict';

const gmsg = require('./gmsg.js');
const nm = require('./nmea.gmp.js');

const out = (s) => process.stdout.write(s);
/** The value of a text field, or `fallback` when it fails to parse (text views parse on access and throw). */
const or = (get, fallback) => { const [e, v] = gmsg.attempt(get); return e ? fallback : v; };

const log =
  '$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n' +
  '$GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n' +  // bad checksum
  '$GPTXT,01,01,02,ANTSTATUS=OK*3B\r\n' +
  '$GPGGA,000000,,,,,0,00,,,,,,,*66\r\n';

const parser = nm.Nmea.stream();
let printed = false;
for (const ev of parser.events(log)) {  // strings are bytes, one character per byte
  if (ev.error) {
    out(`rejected: ${gmsg.errorText(ev.error.code)}\n`);
  } else if (ev.message instanceof nm.GGAView) {
    const g = ev.message;
    out(`GGA time=${or(() => g.utc_time, 0).toFixed(0)} quality=${nm.FixQuality.nameOf(or(() => g.quality, nm.FixQuality.invalid))}` +
        ` sats=${or(() => g.satellites, 0)}`);
    const lat = or(() => g.latitude, undefined);  // optional field: undefined when the token is empty
    out(lat !== undefined ? ` lat=${lat.toFixed(3)}` : ' (no fix)');
    out('\n');
    if (!printed) {
      printed = true;
      out(gmsg.print(g) + '\n');  // the whole sentence, every field
    }
  } else if (ev.message instanceof nm.TXTView) {
    out(`TXT "${ev.message.text}"\n`);
  }
}

const txt = new nm.TXT({ total: 1, number: 1, severity: 2, text: 'HELLO' });
out('encoded: ' + gmsg.latin1String(nm.Nmea.encode(txt)));
