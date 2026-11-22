// exact.js - the gmpc engine of the decode tester: the real generator (gmpc compiled to WebAssembly, gmpc/gmpc.js)
// turns the schema into JavaScript (gmpc --lang js), which runs on the shared runtime (runtime-js/gmsg.js) and decodes
// the input exactly as an application using the generated code would. The result has the same shape as the Quick
// engine's (decoder.js), so the decode tester shows both the same way; byte ranges come from the generated views'
// ranges(). Also: gmpc's own diagnostics for the Issues bar, and generated code for the Code tab.
//
// gmpc.js (2 MB) is loaded only when first needed. It exists when gmpc/gmpc-info.js (tiny, written next to it by
// gmpc/build.py) defined window.GMP_GMPC.
(function (G) {
  'use strict';

  const gmsg = window.gmsg;
  const D = G.decoder;
  const E = (G.exact = {});
  const listeners = [];
  // gmpc/ next to js/ (works from any page that loads this script, e.g. the test pages)
  const BASE = (document.currentScript && document.currentScript.src || '').replace(/js\/exact\.js(\?.*)?$/, '');

  E.available = () => !!window.GMP_GMPC;
  E.ready = () => !!mod;
  E.loading = () => !!loadPromise && !mod;
  E.failed = () => loadError;
  /** Call fn when gmpc finishes loading (or fails). */
  E.onReady = (fn) => listeners.push(fn);

  let mod = null, loadPromise = null, loadError = '';
  E.load = () => {
    if (loadPromise) return loadPromise;
    loadPromise = new Promise((resolve, reject) => {
      const sc = document.createElement('script');
      sc.src = BASE + 'gmpc/gmpc.js';
      sc.onload = () => window.createGmpc().then((m) => { mod = m; resolve(m); }, reject);
      sc.onerror = () => reject(new Error('gmpc/gmpc.js could not be loaded'));
      document.head.appendChild(sc);
    }).catch((e) => { loadError = e.message || String(e); throw e; })
      .finally(() => listeners.forEach((fn) => { try { fn(); } catch (_) { /* ignore */ } }));
    loadPromise.catch(() => {});
    return loadPromise;
  };

  // ---- generation ------------------------------------------------------------------------------------------------
  const genCache = new Map();
  /** gmpc output for YAML text: { ok, files: [{ name, content }], diagnostics: [...] } (cached). */
  E.generate = (yaml, lang, file = 'schema.yaml') => {
    if (!mod) throw new Error('gmpc is not loaded');
    const key = lang + '\u0000' + file + '\u0000' + yaml;
    if (genCache.has(key)) return genCache.get(key);
    const r = JSON.parse(mod.ccall('gmpc_generate', 'string', ['string', 'string', 'string', 'string'], [yaml, file, lang, '']));
    if (genCache.size > 40) genCache.clear();
    genCache.set(key, r);
    return r;
  };
  E.version = () => (mod ? mod.ccall('gmpc_version', 'string', [], []) : '');

  /** Extension modules of custom items are the user's code: they do not run here. */
  const stubModule = (id) => new Proxy({}, {
    get: (_, name) => () => gmsg.fail(gmsg.Err.Custom, `custom code ${String(name)} (${id}) does not run in Studio`),
  });

  const compiled = new Map();
  /** The generated JS module for schema state s: { ok, exports, diagnostics, code }. */
  E.compile = (s) => {
    const yaml = G.toYaml(s);
    if (compiled.has(yaml)) return compiled.get(yaml);
    const r = E.generate(yaml, 'js');
    let out;
    if (!r.ok || !r.files.length) out = { ok: false, diagnostics: r.diagnostics };
    else {
      const code = r.files[0].content;
      const module = { exports: {} };
      const req = (id) => (id === './gmsg.js' ? gmsg : stubModule(id));
      try {
        new Function('module', 'exports', 'require', code)(module, module.exports, req);
        out = { ok: true, exports: module.exports, diagnostics: r.diagnostics, code };
      } catch (e) {
        out = { ok: false, diagnostics: [{ level: 'error', line: 0, col: 0, message: 'generated code failed to load: ' + e.message }] };
      }
    }
    if (compiled.size > 20) compiled.clear();
    compiled.set(yaml, out);
    return out;
  };

  /** gmpc's diagnostics for the schema as Issues-bar entries ({ level, where, msg, gmpc: true }). */
  E.issues = (s) => {
    if (!mod) return [];
    const r = E.generate(G.toYaml(s), '');
    return r.diagnostics.map((d) => ({ level: d.level, where: 'gmpc' + (d.line ? ` (YAML line ${d.line})` : ''), msg: d.message, gmpc: true }));
  };

  // ---- result trees ------------------------------------------------------------------------------------------------
  const node = (name, kind, start, end, extra = {}) =>
    ({ name, kind, start, end, value: '', raw: '', children: [], error: '', note: '', ...extra });
  const errText = (code) => gmsg.errorText(code);
  const attempt = (fn) => {
    try { return [0, fn()]; } catch (e) { if (e instanceof gmsg.GmpError) return [e.code, undefined, e.message]; throw e; }
  };
  /** A generated value as the Quick engine keeps it (BigInt integers, char codes as BigInt). */
  function norm(kind, v) {
    if (typeof v === 'string' && v.length === 1) return BigInt(v.charCodeAt(0));
    if ((kind === 'int' || kind === 'enum') && typeof v === 'number') return BigInt(v);
    return v;
  }

  /**
   * Nodes for the fields of a view. `fields` = the Studio schema fields, `at(i)` maps a byte index of the view's
   * buffer (absolute, see base) to an input position.
   */
  function viewNodes(s, fields, view, base, at, bytes) {
    const out = [];
    for (const r of view.ranges()) {
      const f = fields.find((x) => x.name === r.name) || (r.name === '' ? fields.find((x) => G.fieldKind(s, x) === 'pad') : null);
      const a = view.data.byteOffset - base + r.start, b = view.data.byteOffset - base + r.end;
      const start = at(a), end = b > a ? at(b) : start;
      const raw = D.hex(bytes.slice(a, b));
      if (!f) { out.push(node(r.name || 'padding', 'pad', start, end, { raw })); continue; }
      const kind = G.fieldKind(s, f);
      if (!r.key) {
        // padding, or a constant (checked by the generated code, so the bytes hold the constant)
        const c = node(kind === 'pad' ? 'padding' : f.name, kind === 'pad' ? 'pad' : kind, start, end, { raw, type: G.typeLabel(s, f), field: f });
        if (kind !== 'pad' && G.isInt(f.const)) c.value = D.scalarText(s, f, kind, f.type, BigInt(G.parseInt(f.const)));
        out.push(c);
        continue;
      }
      const [code, v] = attempt(() => view[r.key]);
      const n = node(f.name, kind, start, end, { raw, type: G.typeLabel(s, f), field: f });
      if (code) n.error = errText(code);
      else valueInto(s, f, kind, v, n, base, at, bytes);
      out.push(n);
    }
    return out;
  }

  function valueInto(s, f, kind, v, n, base, at, bytes) {
    switch (kind) {
      case 'int': case 'float': case 'bool': case 'char': case 'enum':
        n.value = D.scalarText(s, f, kind, f.type, norm(kind, v));
        n.v = norm(kind, v);
        break;
      case 'bits': {
        const members = f.bits.filter((b) => !b.pad && !(b.const !== undefined && String(b.const).trim() !== ''));
        const keys = Object.keys(v);
        members.forEach((b, i) => {
          const x = v[keys[i]];
          const t = b.type || (G.parseInt(b.bits) === 1 ? 'bool' : '');
          const child = node(b.name, 'bits', n.start, n.end, { type: `${G.parseInt(b.bits) || 1} bit${G.parseInt(b.bits) > 1 ? 's' : ''}` });
          if (G.findEnum(s, t)) child.value = D.scalarText(s, b, 'enum', t, BigInt(x));
          else if (typeof x === 'boolean') child.value = x ? 'true' : 'false';
          else child.value = D.scalarText(s, b, 'int', t || 'u8', BigInt(x));
          n.children.push(child);
        });
        break;
      }
      case 'bytes': n.value = `${v.length} byte${v.length === 1 ? '' : 's'}`; break;
      case 'string': n.value = '"' + Array.from(v, (c) => D.printable(c.charCodeAt(0))).join('') + '"'; break;
      case 'message': {
        const m = G.findMessage(s, f.type);
        n.value = f.type;
        if (m && v && v.ranges) n.children = viewNodes(s, m.fields, v, base, at, bytes);
        break;
      }
      case 'array': {
        const ek = G.fieldKind(s, { type: f.of });
        const elem = { ...G.newField({ type: f.of }), name: '' };
        n.value = `${v.length} element${v.length === 1 ? '' : 's'}`;
        v.forEach((x, i) => {
          if (x && x.ranges) {
            const m = G.findMessage(s, f.of);
            const a = x.data.byteOffset - base;
            const c = node(`[${i}]`, 'message', at(a), at(a + x.data.length), { value: f.of });
            if (m) c.children = viewNodes(s, m.fields, x, base, at, bytes);
            n.children.push(c);
          } else {
            const c = node(`[${i}]`, ek, n.start, n.end);
            valueInto(s, elem, ek, x, c, base, at, bytes);
            n.children.push(c);
          }
        });
        break;
      }
      case 'custom': n.value = v === undefined ? '' : String(v); break;
      default: n.value = v === undefined ? '' : String(v);
    }
  }

  /** Input position of each unescaped interior byte of a frame at `start` (escaping undone as gmsg does). */
  function originMap(bytes, start, P) {
    const pre = P.prefixSize, end = bytes.length - P.suffixSize;
    const origin = [];
    const esc = P.escape;
    for (let i = pre; i < end; i++) {
      origin.push(start + i);
      if (esc.enabled && bytes[i] === esc.esc) i++;
    }
    origin.push(start + end);  // one past the interior
    return origin;
  }

  const printedOf = (v) => ({ multi: gmsg.print(v), single: gmsg.print(v, { style: 'single_line' }) });

  function frameItem(s, p, P, ev, input) {
    const start = ev.offset, end = ev.offset + ev.bytes.length;
    const name = ev.message.constructor.messageName;
    const item = { type: 'frame', start, end, nodes: [], errors: [], message: name, printed: printedOf(ev.message) };
    const origin = originMap(ev.bytes, start, P);
    const interior = origin.length - 1;
    const at = (i) => origin[Math.min(Math.max(i, 0), interior)];
    if (P.prefixSize) item.nodes.push(node(p.framing.type === 'delimited' ? 'start' : 'sync', 'frame', start, start + P.prefixSize, { raw: D.hex(input.slice(start, start + P.prefixSize)) }));
    const msg = G.findMessage(s, name);
    if (ev.message.ranges) {
      // binary: header view at interior 0, the body after it, the checksum at the end of the interior
      const base = (ev.header ? ev.header.data : ev.message.data).byteOffset;
      const work = new Uint8Array(ev.message.data.buffer, base, interior);
      if (ev.header) {
        const h = node('header', 'frame', at(0), at(P.headerSize), { value: `${P.headerSize} bytes` });
        h.children = viewNodes(s, p.header, ev.header, base, at, work);
        item.nodes.push(h);
      }
      const b0 = ev.message.data.byteOffset - base;
      const mn = node(name, 'message', at(b0), at(b0 + ev.message.data.length), { type: 'message', msg, top: true, value: msg && msg.type_id ? `type_id ${msg.type_id}` : '' });
      mn.children = viewNodes(s, msg ? msg.fields : [], ev.message, base, at, work);
      item.nodes.push(mn);
      if (P.trailerSize) item.nodes.push(node('checksum', 'check', at(interior - P.trailerSize), at(interior), { value: '✓', raw: D.hex(work.slice(interior - P.trailerSize)), type: p.checksum.algo === '__crc' ? 'custom CRC' : p.checksum.algo }));
    } else {
      // text: tokens of the body
      const text = gmsg.latin1String(ev.bytes, P.prefixSize, ev.bytes.length - P.suffixSize);
      const sep = (p.separator || s.schema.defaults.separator || ',');
      const bodyAt = p.message_type.kind === 'token' ? text.indexOf(sep) + 1 : 0;
      item.nodes.push(textMessageNode(s, msg, ev.message, (i) => at(bodyAt + i)));
    }
    if (P.suffixSize) item.nodes.push(node('end', 'frame', end - P.suffixSize, end, { raw: D.hex(input.slice(end - P.suffixSize, end)) }));
    return item;
  }

  function textMessageNode(s, msg, view, at) {
    const V = view.constructor;
    const sep = V.separator;
    const body = view.raw;
    const pos = [0];
    for (let i = 0; i < body.length; i++) if (body[i] === sep) pos.push(i + 1);
    const tokStart = (k) => (k < pos.length ? pos[k] : body.length);
    const tokEnd = (k) => (k + 1 < pos.length ? pos[k + 1] - 1 : body.length);
    const mn = node(msg ? msg.name : V.messageName, 'message', at(0), at(body.length), { type: 'message', msg, top: true });
    for (const fd of V.fields) {
      const f = msg && msg.fields.find((x) => x.name === fd.name);
      const kind = f ? G.fieldKind(s, f) : 'pad';
      const a = tokStart(fd.token), b = tokEnd(fd.token + fd.tokens - 1);
      const n = node(fd.name || 'padding', kind, at(a), at(Math.max(a, b)), { raw: '"' + body.slice(a, Math.max(a, b)) + '"', type: f ? f.type : '', field: f });
      if (fd.key) {
        const [code, v] = attempt(() => view[fd.key]);
        if (code) n.error = errText(code);
        else if (v === undefined) { n.value = '— (empty, optional)'; n.absent = true; }
        else if (f) valueInto(s, f, kind, v, n, 0, at, new Uint8Array(0));
      }
      mn.children.push(n);
    }
    return mn;
  }

  /** Decode `input` as a stream of frames of protocol p with the generated code. Same result shape as decoder.js. */
  E.decodeProtocol = (s, p, input) => {
    input = gmsg.toBytes(input);
    const c = E.compile(s);
    if (!c.ok) return { compileErrors: c.diagnostics, items: [] };
    const P = c.exports[p.name];
    if (!P) return { compileErrors: [{ message: `protocol ${p.name} was not generated` }], items: [] };
    const items = [];
    if (typeof P.stream !== 'function') {
      // no stream support (no framing or no max_frame): the whole input is one frame
      const [code, v] = attempt(() => P.parse(input));
      if (code) items.push({ type: 'frame', start: 0, end: input.length, nodes: [], errors: [errText(code)], message: null });
      else items.push(frameItem(s, p, P, { message: v, header: undefined, bytes: input, offset: 0 }, input));
      return { items };
    }
    const st = new gmsg.StreamParser(P, { capacity: Math.max(P.maxFrame || 0, input.length, 1) });
    st.push(input);
    let failedEnd = 0;
    try {
      for (let ev; (ev = st.pollEvent());) {
        if (ev.message) { items.push(frameItem(s, p, P, ev, input)); continue; }
        const e = ev.error, from = ev.offset, to = ev.offset + e.bytes.length;
        const sc = attempt(() => P.scan(input.subarray(from)))[1];
        if (sc && sc.kind === 'frame' && sc.size === e.bytes.length) {
          // a complete frame that failed validation (checksum, type id, field values...)
          items.push({ type: 'frame', start: from, end: to, nodes: [node('frame', 'error', from, to, { error: e.message, raw: D.hex(e.bytes, 24) })], errors: [e.message], message: null });
          failedEnd = Math.max(failedEnd, to);
          continue;
        }
        const a = Math.max(from, failedEnd);
        if (a >= to) continue;
        const last = items[items.length - 1];
        if (last && last.type === 'garbage' && last.end === a && last.error === e.message) last.end = to;
        else items.push({ type: 'garbage', start: a, end: to, error: e.message });
      }
    } catch (err) {
      items.push({ type: 'garbage', start: st.position, end: input.length, error: 'gmpc engine: ' + err.message });
      return { items };
    }
    const rest = input.length - st.position;
    const lone = rest && rest === P.prefixSize && P.prefixSize === P.suffixSize &&
      P.prefix.every((b, i) => b === P.suffix[i] && input[st.position + i] === b);
    if (rest && !lone) items.push({ type: 'incomplete', start: st.position, end: input.length, error: 'incomplete frame' });
    return { items };
  };

  /** Decode `input` as a single message (no framing) with the generated code. */
  E.decodeMessage = (s, m, input) => {
    input = gmsg.toBytes(input);
    const c = E.compile(s);
    if (!c.ok) return { compileErrors: c.diagnostics, items: [] };
    const V = c.exports[m.name + 'View'], M = c.exports[m.name];
    if (!V) return { compileErrors: [{ message: `${m.name} has no view (access: struct): nothing to decode with` }], items: [] };
    const text = G.msgFormat(s, m) === 'text';
    let end = input.length;
    if (text) while (end > 0 && (input[end - 1] === 10 || input[end - 1] === 13)) end--;
    const data = input.subarray(0, end);
    const [code, v, detail] = attempt(() => V.from(text ? gmsg.latin1String(data) : data));
    if (code) return { items: [{ type: 'frame', start: 0, end, nodes: [node(m.name, 'error', 0, end, { error: detail })], errors: [detail], message: m.name }] };
    const item = { type: 'frame', start: 0, end: text ? end : v.wireSize, nodes: [], errors: [], message: m.name };
    if (text) item.nodes.push(textMessageNode(s, m, v, (i) => i));
    else {
      const mn = node(m.name, 'message', 0, v.wireSize, { type: 'message', msg: m, top: true });
      mn.children = viewNodes(s, m.fields, v, v.data.byteOffset, (i) => i, input);
      item.nodes.push(mn);
    }
    const bad = [];
    const walk = (ns) => ns.forEach((n) => { if (n.error) bad.push(`${n.name}: ${n.error}`); walk(n.children); });
    walk(item.nodes);
    if (M) {
      const [sc, , sd] = attempt(() => M.decode(text ? data : data));
      if (sc && !bad.length) bad.push(`struct decode: ${sd}`);
    }
    if (bad.length) item.errors.push(bad.slice(0, 3).join('; '));
    else item.printed = printedOf(v);
    const items = [item];
    if (!text && v.wireSize < input.length) items.push({ type: 'garbage', start: v.wireSize, end: input.length, error: 'trailing bytes (ignored by decode)' });
    return { items };
  };

  /** Differences between the Quick result `q` and the gmpc result `x` (empty when they agree). */
  E.compare = (s, q, x) => {
    const out = [];
    const valid = (r, printed) => r.items.filter((i) => i.type === 'frame' && !i.errors.length).map(printed);
    const qs = valid(q, (f) => ({ start: f.start, text: G.printer.frame(s, f, 'multi') }));
    const xs = valid(x, (f) => ({ start: f.start, text: f.printed ? f.printed.multi : null }));
    if (qs.length !== xs.length) out.push(`Quick finds ${qs.length} valid frame${qs.length === 1 ? '' : 's'}, gmpc ${xs.length}`);
    for (const f of xs) {
      const g = qs.find((y) => y.start === f.start);
      if (!g) out.push(`gmpc decodes a frame at byte ${f.start} that Quick rejects`);
      else if (g.text !== null && f.text !== null && g.text !== f.text) out.push(`the frame at byte ${f.start} prints differently`);
    }
    for (const f of qs) if (!xs.some((y) => y.start === f.start)) out.push(`Quick decodes a frame at byte ${f.start} that gmpc rejects`);
    return [...new Set(out)];
  };
})(window.GMP);
