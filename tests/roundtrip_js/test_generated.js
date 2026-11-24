// Cross-check of the generated JavaScript (gmpc --lang js) against the generated C++: the reference results come from
// js_crossref (tests/roundtrip_js/js_crossref.cpp). Every case must give the same errors, the same gmsg::print text
// and the same re-encoded bytes.
//
//   node tests/roundtrip_js/test_generated.js <js_crossref executable> <directory with the generated .gmp.js> [-v]
'use strict';

const path = require('path');
const { execFileSync } = require('child_process');
const T = require('../runtime_js/harness.js');

const [exe, genDir] = process.argv.slice(2).filter((a) => !a.startsWith('-')).map((p) => path.resolve(p));
const gmsg = require(path.join(genDir, 'gmsg.js'));
const ref = JSON.parse(execFileSync(exe, { maxBuffer: 1 << 30 }).toString('latin1'));
const modules = {};
const load = (schema) => (modules[schema] = modules[schema] || require(path.join(genDir, schema + '.gmp.js')));

const hexToBytes = (h) => Uint8Array.from(h.match(/../g) || [], (x) => parseInt(x, 16));
const toHex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const attempt = (fn) => gmsg.attempt(fn);
const single = (v) => gmsg.print(v, { style: 'single_line' });

for (const msg of ref.messages) {
  T.test(`message ${msg.schema}.${msg.name} (${msg.cases.length} inputs)`, () => {
    const mod = load(msg.schema);
    const M = mod[msg.name], V = mod[msg.name + 'View'];
    for (const c of msg.cases) {
      const bytes = hexToBytes(c.hex);
      const what = `${msg.name} ${c.hex}`;
      if (c.decode) {
        const [code, m] = attempt(() => M.decode(bytes));
        T.eq(code, c.decode.err || 0, `${what}: decode error`);
        if (!code && !c.decode.err) {
          T.eq(gmsg.print(m), c.decode.multi, `${what}: print`);
          T.eq(single(m), c.decode.single, `${what}: single-line print`);
          const [ec, enc] = attempt(() => M.encode(m));
          if (c.decode.encode !== undefined) {
            T.eq(ec, 0, `${what}: encode error`);
            // JavaScript turns a signalling f32 NaN into a quiet NaN (its payload cannot round-trip): compare those
            // re-encodings by what they decode to
            if (!ec && toHex(enc) !== c.decode.encode && /\bnan\b/.test(c.decode.multi))
              T.eq(gmsg.print(M.decode(enc)), gmsg.print(M.decode(hexToBytes(c.decode.encode))), `${what}: re-encoded NaN`);
            else if (!ec) T.eq(toHex(enc), c.decode.encode, `${what}: re-encoded bytes`);
          } else {
            T.eq(ec, c.decode.encodeErr, `${what}: encode error code`);
          }
        }
      }
      const [vc, v] = attempt(() => V.from(bytes));
      T.eq(vc, c.view.err || 0, `${what}: view error`);
      if (!vc && !c.view.err) {
        T.eq(gmsg.print(v), c.view.multi, `${what}: view print`);
        if (c.view.size !== undefined) T.eq(v.wireSize, c.view.size, `${what}: view size`);
        if (c.view.extra !== undefined) T.eq(gmsg.extraOf(v), c.view.extra, `${what}: data after the message`);
      }
    }
  });
}

for (const proto of ref.protocols) {
  T.test(`protocol ${proto.schema}.${proto.name} (${proto.frames.length} frames, ${proto.streams.length} streams)`, () => {
    const P = load(proto.schema)[proto.name];
    for (const c of proto.frames) {
      const bytes = hexToBytes(c.hex);
      const what = `${proto.name} ${c.hex}`;
      const [pc, v] = attempt(() => P.parse(bytes));
      T.eq(pc, c.parse.err || 0, `${what}: parse error`);
      if (!pc && !c.parse.err) T.eq(single(v), c.parse.single, `${what}: parsed view`);
      if (c.decode) {
        const [dc, m] = attempt(() => P.decode(bytes));
        T.eq(dc, c.decode.err || 0, `${what}: decode error`);
        if (!dc && !c.decode.err) {
          T.eq(single(m), c.decode.single, `${what}: decoded message`);
          const [ec, enc] = attempt(() => P.encode(m));
          if (c.decode.encode !== undefined) {
            T.eq(ec, 0, `${what}: encode error`);
            if (!ec) T.eq(toHex(enc), c.decode.encode, `${what}: encoded frame`);
          } else {
            T.eq(ec, c.decode.encodeErr, `${what}: encode error code`);
          }
        }
      }
    }
    for (const c of proto.streams) {
      const s = P.stream();
      let ev = '';
      s.on('frame', (view, header) => {
        if (header !== undefined) ev += 'H:' + single(header) + '\n';
        ev += 'M:' + single(view) + '\n';
      });
      s.on('error', (e) => { ev += `E${e.code}:${e.bytes.length}\n`; });
      const wire = hexToBytes(c.hex);
      let ring = null;
      if (c.ring) {
        // ring run (js_crossref.cpp): the recorded pushes into a 32-byte ByteRing and the drains
        ring = new gmsg.ByteRing(32);
        let pos = 0;
        const drain = (how, max) => {
          const limit = max || Infinity;
          if (how === 'F') s.feed(ring, limit);
          else { s.push(ring, limit); while (s.next()) { /* deliver */ } }
        };
        for (const tok of c.ring.trim().split(' ')) {
          const n = Number(tok.slice(1));
          if (tok[0] === 'P') { ring.pushBytes(wire.subarray(pos, pos + n)); pos += n; }
          else if (tok[0] === 'B') { for (let i = 0; i < n; i++) ring.push(wire[pos + i]); pos += n; }
          else drain(tok[0], n);
        }
        const flushed = s.flush();  // before reading ev: the flush appends its error event
        ev += `R${flushed ? 1 : 0}\n`;
      } else if (c.chunk) {
        for (let pos = 0; pos < wire.length; pos += c.chunk) s.feed(wire.subarray(pos, Math.min(wire.length, pos + c.chunk)));
      } else {
        // recovery run (js_crossref.cpp): chunks of 3, then the recorded resync / flush / tick ops
        s.setIdleTimeout(2);
        let now = 0, k = 0;
        const op = (c2) => {
          now++;
          const did = c2 === 'r' ? s.resync() : c2 === 'f' ? s.flush() : c2 === 't' ? s.tick(now) : false;
          ev += `R${did ? 1 : 0}\n`;
          while (s.next()) { /* deliver */ }
        };
        for (let pos = 0; pos < wire.length; pos += 3) {
          s.feed(wire.subarray(pos, Math.min(wire.length, pos + 3)));
          op(c.ops[k++]);
        }
        while (k < c.ops.length) op(c.ops[k++]);
      }
      T.eq(ev, c.events, `${proto.name} stream ${c.hex} chunk ${c.chunk}`);
      T.eqList([s.stats.frames, s.stats.frameErrors, s.stats.bytesSkipped, s.stats.overflows, s.stats.resyncs, s.stats.flushes,
        s.stats.framesWithExtra, s.stats.overruns], c.stats, `${proto.name} stream stats`);
      if (ring) T.eqList([ring.dropped, ring.highWater], c.ringStats, `${proto.name} ring stats`);
    }
  });
}

T.run();
