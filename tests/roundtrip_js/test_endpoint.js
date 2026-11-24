// gmsg.Endpoint (runtime-js/gmsg.js) on generated protocols: receive / attach, waitFor / waitForReply / transact,
// send with both send policies and every kind of sink, the idle resync, close(), and StreamParser.pushGap().
//
//   node tests/roundtrip_js/test_endpoint.js <directory with the generated .gmp.js> [-v]
'use strict';

const path = require('path');
const { PassThrough, Writable } = require('stream');
const T = require('../runtime_js/harness.js');

const genDir = path.resolve(process.argv.slice(2).filter((a) => !a.startsWith('-'))[0]);
const gmsg = require(path.join(genDir, 'gmsg.js'));
const tel = require(path.join(genDir, 'telemetry.gmp.js'));
const nmea = require(path.join(genDir, 'nmea.gmp.js'));
const { Err } = gmsg;
const Link = tel.BinaryLink;

/** Timers that only move when advance() is called. */
function fakeTimers() {
  let now = 0, seq = 0;
  const pending = new Map();
  return {
    now: () => now,
    setTimeout: (fn, ms) => { const id = ++seq; pending.set(id, { at: now + Math.max(0, ms), fn }); return id; },
    clearTimeout: (id) => pending.delete(id),
    /** Run the timers due within `ms`, in time order. */
    advance(ms) {
      const end = now + ms;
      for (;;) {
        let next = null;
        for (const [id, t] of pending) if (t.at <= end && (!next || t.at < next[1].at)) next = [id, t];
        if (!next) break;
        pending.delete(next[0]);
        now = next[1].at;
        next[1].fn();
      }
      now = end;
    },
    get count() { return pending.size; },
  };
}
const ticks = () => new Promise((r) => setImmediate(r));
/** Await `p` while moving fake time forward 1 ms at a time (at most `limit` ms). */
async function drive(p, timers, limit = 1000) {
  let done = false, value, error;
  p.then((v) => { done = true; value = v; }, (e) => { done = true; error = e; });
  for (let i = 0; i < limit && !done; ++i) {
    await ticks();
    if (!done) timers.advance(1);
  }
  await ticks();
  if (!done) throw new Error('drive: still pending');
  if (error) throw error;
  return value;
}
async function codeOf(p) {
  try { await p; return 0; } catch (e) { if (!(e instanceof gmsg.GmpError)) throw e; return e.code; }
}

const status = (voltage) => new tel.Status({ voltage, label: 's' });
const command = (opcode) => new tel.Command({ opcode });
const logMsg = (level) => new tel.Log({ level, text: 't' });
const frame = (m, h) => Link.encode(m, h);
const cat = (...parts) => { const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0)); let o = 0; for (const p of parts) { out.set(p, o); o += p.length; } return out; };

function recorder(ep) {
  const events = [];
  ep.on(tel.Status, (v) => events.push(`status ${v.voltage}`));
  ep.on(tel.Command, (v) => events.push(`command ${v.opcode}`));
  ep.on(tel.Log, (v) => events.push(`log ${v.level}`));
  ep.on('error', (e) => events.push(`error ${e.text}`));
  return events;
}

T.test('receive(): chunks in any size, every frame to the listeners, errors too', () => {
  const ep = Link.endpoint({ timers: fakeTimers() });
  const ev = recorder(ep);
  const w = cat(frame(status(100)), Uint8Array.of(0x13, 0x37), frame(command(7)), frame(logMsg(2)));
  let frames = 0;
  for (let i = 0; i < w.length; i += 5) frames += ep.receive(w.subarray(i, i + 5));
  T.eq(frames, 3);
  T.eqList(ev, ['status 100', 'error bad sync', 'command 7', 'log 2']);
  T.eq(ep.stats.frames, 3);
});

T.test('waitFor(): resolves with the decoded message; others go to the listeners; match; Timeout', async () => {
  const timers = fakeTimers();
  const ep = Link.endpoint({ timers });
  const ev = recorder(ep);
  const c = ep.waitFor(tel.Command, { timeout: 100 });
  ep.receive(cat(frame(status(1)), frame(logMsg(4)), frame(command(42)), frame(status(2))));
  const got = await c;
  T.eq(got instanceof tel.Command, true, 'struct');
  T.eq(got.opcode, 42);
  T.eqList(ev, ['status 1', 'log 4', 'status 2'], 'the awaited command is not delivered');
  // match on the view; a non-matching message goes to the listeners
  const s = ep.waitFor('Status', { timeout: 100, match: (v) => v.voltage === 9 });
  ep.receive(cat(frame(status(8)), frame(status(9))));
  T.eq((await s).voltage, 9);
  T.eq(ev[ev.length - 1], 'status 8');
  // nothing arrives: Timeout after the limit (a number is the timeout)
  const t = ep.waitFor(tel.Status, 30);
  timers.advance(29);
  await ticks();
  let settled = false;
  t.then(() => { settled = true; }, () => { settled = true; });
  await ticks();
  T.eq(settled, false, 'still waiting at 29 ms');
  timers.advance(1);
  T.eq(await codeOf(t), Err.Timeout);
  T.eq(ep.waiters.length, 0);
  // two waiters for the same type: first come, first served
  const a = ep.waitFor(tel.Log), b = ep.waitFor(tel.Log);
  ep.receive(cat(frame(logMsg(1)), frame(logMsg(2))));
  T.eq((await a).level, 1);
  T.eq((await b).level, 2);
});

T.test('waitForReply(): header and message; match sees the header', async () => {
  const ep = Link.endpoint({ timers: fakeTimers() });
  let headers = 0;
  const r = ep.waitForReply(tel.Status, { match: (v, h) => { headers++; T.eq(h.msg_id, tel.MsgId.status); return v.voltage === 7; } });
  ep.receive(cat(frame(status(6)), frame(status(7))));
  const got = await r;
  T.eq(headers, 2);
  T.eq(got.message.voltage, 7);
  T.eq(got.header.msg_id, tel.MsgId.status);
  T.eq(got.header.len, frame(status(7)).length - 2 - 3 - 2);
});

T.test('transact(): request out, reply back through a simulated device; send errors reject', async () => {
  const timers = fakeTimers();
  let ep = null;
  const device = (bytes) => {  // answers a Command synchronously, while it is being sent
    const m = Link.decode(bytes);
    ep.receive(frame(status(m.opcode)));
  };
  ep = Link.endpoint({ write: device, timers });
  const ev = recorder(ep);
  ep.receive(frame(status(999)));  // an old reply: delivered on arrival
  const r = await ep.transact(command(17), tel.Status, { timeout: 20 });
  T.eq(r.voltage, 17);
  T.eqList(ev, ['status 999']);
  // header values
  const r2 = await ep.transact(command(23), tel.Status, { timeout: 20, header: new tel.BinaryLinkHeader({}) });
  T.eq(r2.voltage, 23);
  // silent device: Timeout counted from the send
  const silent = Link.endpoint({ write: () => {}, timers });
  const t = silent.transact(command(1), tel.Status, 5);
  T.eq(await drive(codeOf(t), timers), Err.Timeout);
  // no sink: BufferFull, no waiter left behind
  const none = Link.endpoint({ timers });
  T.eq(await codeOf(none.transact(command(1), tel.Status, 5)), Err.BufferFull);
  T.eq(none.waiters.length, 0);
});

T.test('send(): whole frame or nothing; retry; retry timeout; raw bytes', async () => {
  const timers = fakeTimers();
  const out = [];
  const roomy = { room: 64, writable() { return this.room; }, write(b) { out.push(...b); this.room -= b.length; return b.length; } };
  const ep = Link.endpoint({ write: roomy, timers });
  const f = frame(command(9));
  T.eq(await ep.send(command(9)), f.length);
  T.eqList(out, Array.from(f));
  roomy.room = 4;
  T.eq(await codeOf(ep.send(command(9))), Err.BufferFull);
  T.eq(out.length, f.length, 'nothing written');
  // a sink taking 3 bytes per call: 'whole' fails after a short write, 'retry' finishes over time
  const slow = { got: [], write(b) { const n = Math.min(3, b.length); this.got.push(...b.subarray(0, n)); return n; } };
  const ep2 = Link.endpoint({ write: slow, timers });
  T.eq(await codeOf(ep2.send(status(3))), Err.BufferFull);
  T.eq(slow.got.length, 3);
  slow.got = [];
  ep2.setSendPolicy('retry');
  T.eq(await drive(ep2.send(status(3)), timers), frame(status(3)).length);
  T.eqList(slow.got, Array.from(frame(status(3))));
  // a sink that stops: Timeout after sendTimeout
  const stuck = { n: 0, write(b) { if (this.n >= 4) return 0; const k = Math.min(2, b.length); this.n += k; return k; } };
  const ep3 = Link.endpoint({ write: stuck, timers, sendPolicy: 'retry', sendTimeout: 50 });
  const t0 = timers.now();
  T.eq(await drive(codeOf(ep3.send(command(1))), timers), Err.Timeout);
  T.eq(stuck.n, 4);
  T.eq(timers.now() - t0 >= 50, true, 'waited 50 ms');
  // raw bytes; function sinks returning a Promise
  const seen = [];
  const ep4 = Link.endpoint({ write: async (b) => { seen.push(...b); }, timers });
  T.eq(await ep4.sendRaw(Uint8Array.of(0xFD)), 1);
  T.eqList(seen, [0xfd]);
  T.eq(await codeOf(Promise.resolve().then(() => ep4.setSendPolicy('later'))), Err.InvalidValue);
});

T.test('send(): Node Writable (drain) and WritableStream (ready)', async () => {
  // Node: a full stream refuses 'whole'; 'retry' waits for 'drain'
  const chunks = [];
  let release = null;
  const node = new Writable({ highWaterMark: 8, write(c, _e, cb) { chunks.push(...c); release = cb; } });
  const ep = Link.endpoint({ write: node });
  await ep.send(command(1));  // accepted, but the stream is now over its high water mark
  T.eq(node.writableNeedDrain, true);
  T.eq(await codeOf(ep.send(command(2))), Err.BufferFull);
  ep.setSendPolicy('retry', 1000);
  const p = ep.send(command(3));
  await ticks();
  release();  // the first chunk is written: 'drain'
  await ticks();
  release && release();
  T.eq(await p, frame(command(3)).length);
  // WHATWG WritableStream
  if (typeof WritableStream === 'function') {
    const got = [];
    const ws = new WritableStream({ write(c) { got.push(...c); } });
    const ep2 = Link.endpoint({ write: ws });
    await ep2.send(command(5));
    await ticks();
    T.eqList(got, Array.from(frame(command(5))));
  }
});

T.test('attach(): Node stream, ReadableStream, async iterable; detach', async () => {
  const ep = Link.endpoint();
  const ev = recorder(ep);
  const node = new PassThrough();
  const detach = ep.attach(node);
  node.write(frame(status(1)));
  await ticks();
  T.eqList(ev, ['status 1']);
  detach();
  node.write(frame(status(2)));
  await ticks();
  T.eq(ev.length, 1, 'detached');
  if (typeof ReadableStream === 'function') {
    let ctrl;
    const rs = new ReadableStream({ start(c) { ctrl = c; } });
    ep.attach(rs);
    ctrl.enqueue(frame(status(3)));
    await ticks();
    T.eq(ev[ev.length - 1], 'status 3');
  }
  async function* chunks() { yield frame(status(4)); yield frame(status(5)); }
  ep.attach(chunks());
  await ticks();
  T.eqList(ev.slice(-2), ['status 4', 'status 5']);
  T.eq(await codeOf(Promise.resolve().then(() => ep.attach(42))), Err.InvalidValue);
  ep.close();
});

T.test('idle timeout: a stuck frame is given up without new input', async () => {
  const timers = fakeTimers();
  const ep = Link.endpoint({ timers, idleTimeout: 10 });
  const ev = recorder(ep);
  const bad = frame(status(5)).slice(0, 10);
  bad[3] = 0; bad[4] = 200;  // a length that never arrives
  ep.receive(cat(bad, frame(status(6))));
  T.eq(ev.length, 0);
  timers.advance(9);
  T.eq(ev.length, 0);
  timers.advance(1);
  T.eq(ev[0], 'error truncated');
  T.eq(ev[ev.length - 1], 'status 6');
  T.eq(timers.count, 0, 'no timer once the buffer is empty');
  ep.setIdleTimeout(0);
});

T.test('close(): rejects pending waits and stops timers; text protocol endpoint', async () => {
  const timers = fakeTimers();
  const ep = Link.endpoint({ timers });
  const w = ep.waitFor(tel.Status, 1000);
  ep.close();
  T.eq(await codeOf(w), Err.InvalidValue);
  T.eq(timers.count, 0);
  T.eq(await codeOf(ep.waitFor(tel.Status)), Err.InvalidValue);
  // NMEA
  const out = [];
  const n = nmea.Nmea.endpoint({ write: (b) => out.push(...b), timers });
  let ggas = 0;
  n.on(nmea.GGA, () => ggas++);
  const txt = n.waitFor(nmea.TXT, 10);
  const gga = '$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n';
  const t = '$GPTXT,01,01,02,ANTSTATUS=OK*3B\r\n';
  n.receive(gga + 'noise' + t);
  const m = await txt;
  T.eq(ggas, 1);
  await n.send(m);
  T.eq(gmsg.latin1String(Uint8Array.from(out)), t);
});

T.test('StreamParser.pushGap(): the frame in progress is dropped as an overrun', () => {
  const p = Link.stream();
  const ev = [];
  p.on('frame', (v) => ev.push(`status ${v.voltage}`));
  p.on('error', (e) => ev.push(e.text));
  const a = frame(status(1)), b = frame(status(2));
  T.eq(p.space, p.capacity);
  p.push(a);
  p.push(b.subarray(0, 4));
  T.eq(p.pushGap(), true);
  T.eq(p.pushGap(), true);
  p.push(b);
  T.eq(p.pushGap(), false);
  while (p.next()) { /* deliver */ }
  T.eqList(ev, ['status 1', 'overrun', 'status 2']);
});

T.run();
