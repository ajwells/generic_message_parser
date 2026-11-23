// yaml.js - state <-> YAML. Emission is hand written (no dependency) and follows the house style of
// the schemas in tests/ and examples/: block mappings for structure, flow mappings for fields.
// Import uses js-yaml, loaded on demand from cdnjs.
(function (G) {
  'use strict';

  // ----------------------------------------------------------------------------------------------
  // emitter
  // ----------------------------------------------------------------------------------------------
  class Raw { constructor(s) { this.s = String(s); } }   // emitted verbatim (numbers, hex literals)
  class Flow { constructor(o) { this.o = o; } }          // emitted as { a: 1, b: 2 }
  const raw = (s) => new Raw(s);
  const flow = (o) => new Flow(o);

  const RESERVED = new Set(['true', 'false', 'yes', 'no', 'on', 'off', 'null', '~', 'y', 'n']);
  const NUM_RE = /^-?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?$/i;

  function quote(s) {
    if (s === '') return '""';
    if (/^[A-Za-z_][A-Za-z0-9_.\-/]*( [A-Za-z0-9_.\-/]+)*$/.test(s) && !RESERVED.has(s.toLowerCase())) return s;
    return JSON.stringify(s);
  }
  function scalar(v) {
    if (v instanceof Raw) return v.s;
    if (typeof v === 'number') return String(v);
    if (typeof v === 'boolean') return v ? 'true' : 'false';
    return quote(String(v));
  }
  const isPlain = (v) => v && typeof v === 'object' && !(v instanceof Raw) && !(v instanceof Flow) && !Array.isArray(v);
  const isScalar = (v) => !isPlain(v) && !(v instanceof Flow) && !Array.isArray(v);

  function flowText(o) {
    const parts = Object.entries(o).map(([k, v]) => {
      let t;
      if (v instanceof Flow) t = flowText(v.o);
      else if (Array.isArray(v)) t = '[' + v.map(scalar).join(', ') + ']';
      else if (isPlain(v)) t = flowText(v);
      else t = scalar(v);
      return `${quote(k)}: ${t}`;
    });
    return parts.length ? '{ ' + parts.join(', ') + ' }' : '{}';
  }

  function emitMap(obj, ind, out) {
    for (const [k, v] of Object.entries(obj)) emitEntry(k, v, ind, out);
  }
  function emitEntry(k, v, ind, out) {
    const pad = ' '.repeat(ind);
    const key = quote(k);
    if (v instanceof Flow) out.push(`${pad}${key}: ${flowText(v.o)}`);
    else if (Array.isArray(v)) {
      if (!v.length) out.push(`${pad}${key}: []`);
      else if (v.every(isScalar)) out.push(`${pad}${key}: [${v.map(scalar).join(', ')}]`);
      else { out.push(`${pad}${key}:`); v.forEach((it) => emitItem(it, ind + 2, out)); }
    } else if (isPlain(v)) {
      if (!Object.keys(v).length) { out.push(`${pad}${key}: {}`); return; }
      out.push(`${pad}${key}:`);
      emitMap(v, ind + 2, out);
    } else out.push(`${pad}${key}: ${scalar(v)}`);
  }
  function emitItem(item, ind, out) {
    const pad = ' '.repeat(ind);
    if (item instanceof Flow) out.push(`${pad}- ${flowText(item.o)}`);
    else if (isPlain(item)) {
      const sub = [];
      emitMap(item, ind + 2, sub);
      sub[0] = pad + '- ' + sub[0].slice(ind + 2);
      out.push(...sub);
    } else out.push(`${pad}- ${scalar(item)}`);
  }

  // value helpers: only emit what the user set
  const has = (v) => v !== undefined && v !== null && String(v).trim() !== '';
  const numOrStr = (v) => (G.isInt(v) || NUM_RE.test(String(v).trim()) ? raw(String(v).trim()) : String(v));
  const lenValue = (v) => (G.isInt(v) ? raw(v.trim()) : G.isIdent(v.trim()) ? v.trim() : raw(JSON.stringify(v.trim())));

  function fieldNode(s, f, owner) {
    const kind = G.fieldKind(s, f);
    const text = !owner.isHeader && G.msgFormat(s, owner) === 'text';
    if (kind === 'pad') {
      const o = { pad: raw(G.isInt(f.padSize) ? f.padSize : '1') };
      if (has(f.doc)) o.doc = f.doc;
      return flow(o);
    }
    const o = { name: f.name || 'unnamed', type: f.type };
    const multiByte = ['int', 'float', 'enum'].includes(kind) && (kind !== 'enum' || G.PRIM_SIZE[(G.findEnum(s, f.type) || {}).type] > 1);
    if (!text && has(f.endian) && (multiByte || kind === 'array')) o.endian = f.endian;
    if (kind === 'bits' && has(f.bit_order)) o.bit_order = f.bit_order;

    if (!text && ['bytes', 'string', 'array'].includes(kind)) {
      const key = kind === 'array' ? 'count' : 'length';
      if (f.lenMode === 'fixed') o[key] = raw(G.isInt(f.lenValue) ? f.lenValue.trim() : '0');
      else if (f.lenMode === 'expr') o[key] = lenValue(f.lenValue || '');
      else if (f.lenMode === 'remaining') o[key] = 'remaining';
      else if (f.lenMode === 'terminator') o.terminator = raw(G.isInt(f.terminator) ? f.terminator : '0');
    }
    if (kind === 'array') {
      const el = f.ofLimits || {};
      if ((has(f.ofEndian) && !text) || G.hasLimits(el)) {
        const eo = { type: f.of };
        if (has(f.ofEndian) && !text) eo.endian = f.ofEndian;
        for (const k of G.LIMIT_KEYS) if (has(el[k])) eo[k] = raw(String(el[k]).trim());
        o.of = flow(eo);
      } else o.of = f.of;
    }
    // `max` is exported wherever it is set: gmpc (and validation) report it where it has no effect
    if (has(f.max)) o.max = raw(f.max.trim());
    if (kind === 'string' && f.lenMode === 'fixed' && f.padding !== '' && f.padding != null && !text) o.padding = f.padding;
    if (has(f.const)) o.const = raw(f.const.trim());
    if (!text && has(f.crc) && ['int', 'float', 'bool', 'char', 'enum'].includes(kind)) {
      if (f.crc === '__crc') {
        const c = f.crcParams;
        const p = { width: raw(c.width || '8'), poly: raw(c.poly || '0') };
        if (has(c.init) && G.parseInt(c.init) !== 0) p.init = raw(c.init);
        if (c.refin) p.refin = true;
        if (c.refout) p.refout = true;
        if (has(c.xorout) && G.parseInt(c.xorout) !== 0) p.xorout = raw(c.xorout);
        o.crc = flow({ crc: flow(p) });
      } else o.crc = f.crc;
    }
    if (has(f.scale)) o.scale = numOrStr(f.scale);
    if (has(f.offset)) o.offset = numOrStr(f.offset);
    if (has(f.unit)) o.unit = f.unit;
    for (const k of G.LIMIT_KEYS) if (has(f[k])) o[k] = raw(String(f[k]).trim());
    const convs = (f.conversions || []).filter((c) => has(c.name));
    if (convs.length && ['int', 'float'].includes(kind)) {
      const map = {};
      for (const c of convs) {
        const co = {};
        if (has(c.scale)) co.scale = numOrStr(c.scale);
        if (has(c.offset)) co.offset = numOrStr(c.offset);
        if (has(c.unit)) co.unit = c.unit;
        map[c.name.trim()] = flow(co);
      }
      o.conversions = flow(map);
    }
    if (!text && has(f.when)) o.when = raw(JSON.stringify(f.when.trim()));
    if (text && f.optional) o.optional = true;
    if (text && has(f.base) && ['int', 'enum'].includes(kind)) o.base = raw(f.base);
    if (text && has(f.width)) o.width = raw(f.width);
    if (text && has(f.precision) && kind === 'float') o.precision = raw(f.precision);
    if (has(f.doc)) o.doc = f.doc;

    if (kind === 'bits') {
      // bits groups are emitted in block style for readability
      const block = { ...o };
      block.fields = f.bits.map((b) => {
        if (b.pad) return flow({ pad: raw(G.isInt(b.bits) ? b.bits : '1') });
        const bo = { name: b.name || 'unnamed', bits: raw(G.isInt(b.bits) ? b.bits : '1') };
        if (has(b.type)) {
          if (G.findEnum(s, b.type)) bo.enum = b.type;
          else bo.type = b.type;
        }
        if (has(b.const)) bo.const = raw(b.const);
        if (has(b.scale)) bo.scale = numOrStr(b.scale);
        if (has(b.offset)) bo.offset = numOrStr(b.offset);
        for (const k of G.LIMIT_KEYS) if (has(b[k])) bo[k] = raw(String(b[k]).trim());
        if (has(b.doc)) bo.doc = b.doc;
        return flow(bo);
      });
      return block;
    }
    return flow(o);
  }

  function patternNode(mode, text) {
    if (mode === 'text') return String.fromCharCode(...G.parseEscapedText(text));
    const bytes = G.parseHexBytes(text) || [];
    return bytes.map((b) => raw(G.hex2(b)));
  }

  function protocolNode(s, p) {
    const o = {};
    const fmt = G.protoFormat(s, p);
    if (has(p.format)) o.format = p.format;
    if (has(p.endian)) o.endian = p.endian;
    if (has(p.separator) && fmt === 'text') o.separator = p.separator;
    if (has(p.doc)) o.doc = p.doc;
    if (p.header.length && fmt === 'binary') {
      const hm = G.headerMessage(s, p);
      o.header = p.header.map((f) => fieldNode(s, f, hm));
    }
    const fr = p.framing;
    const fo = { type: fr.type };
    if (fr.type !== 'none' && fr.type !== 'custom') {
      const sync = G.patternBytes(fr.syncMode, fr.sync) || [];
      if (sync.length) fo[fr.type === 'delimited' ? 'start' : 'sync'] = patternNode(fr.syncMode, fr.sync);
      // end delimiter: required for delimited, optional after fixed and length-prefixed frames
      const end = G.patternBytes(fr.endMode, fr.end) || [];
      if (end.length) fo.end = patternNode(fr.endMode, fr.end);
    }
    const escapable = ['delimited', 'fixed', 'length_prefixed'].includes(fr.type);
    const escapeNode = () => {
      const e = { char: raw(fr.escape.char || '0x7D') };
      if (fr.escape.style === 'map') {
        const m = {};
        (fr.escape.map || '').split(',').forEach((pair) => {
          const [a, b] = pair.split(':').map((x) => (x || '').trim());
          if (G.isInt(a) && G.isInt(b)) m[a] = raw(b);
        });
        e.map = flow(m);
        return flow(e);
      }
      if (fr.escape.style === 'index') e.code = 'index';
      const list = G.parseIntList(fr.escape.reserved);
      if (list && list.length) e.reserved = list.map((b) => raw(G.hex2(b)));
      if (fr.escape.style === 'index') { if (G.isInt(fr.escape.base) && G.parseInt(fr.escape.base) !== 0) e.base = raw(fr.escape.base); }
      else e.xor = raw(fr.escape.xor || '0x20');
      return flow(e);
    };
    if (fr.type === 'fixed' && G.isInt(fr.size)) fo.size = raw(fr.size);
    if (fr.type === 'length_prefixed') {
      const l = { field: fr.lengthField || 'length' };
      if (fr.covers && fr.covers !== 'body') l.covers = fr.covers;
      if (fr.counts === 'wire' && fr.escape.enabled) l.counts = 'wire';
      if (G.isInt(fr.adjust) && G.parseInt(fr.adjust) !== 0) l.adjust = raw(fr.adjust);
      fo.length = flow(l);
    }
    if (fr.type === 'custom') fo.framer = fr.framer;
    if (G.isInt(fr.maxFrame)) fo.max_frame = raw(fr.maxFrame);
    const escaping = fr.escape.enabled && escapable;
    if (escaping) fo.escape = escapeNode();
    o.framing = fr.type === 'length_prefixed' || escaping ? fo : flow(fo);

    const c = p.checksum;
    if (c.enabled) {
      const co = {};
      if (c.algo === '__crc') {
        const cp = c.crc;
        const crc = { width: raw(cp.width || '8'), poly: raw(cp.poly || '0x07') };
        if (has(cp.init)) crc.init = raw(cp.init);
        if (cp.refin) crc.refin = true;
        if (cp.refout) crc.refout = true;
        if (has(cp.xorout)) crc.xorout = raw(cp.xorout);
        co.algo = flow({ crc: flow(crc) });
      } else co.algo = c.algo;
      if (fmt === 'binary' && c.covers && c.covers !== 'header_body') co.covers = c.covers;
      if (fmt === 'binary' && has(c.endian)) co.endian = c.endian;
      if (fmt === 'binary' && fr.escape.enabled && c.over === 'wire') co.over = 'wire';
      if (fmt === 'text' && has(c.separator) && c.separator !== '*') co.separator = c.separator;
      if (fmt === 'text' && !c.required) co.required = false;
      o.checksum = flow(co);
    }
    const d = p.message_type;
    if (d.kind === 'field') o.message_type = flow({ field: d.field || 'id' });
    if (d.kind === 'token') o.message_type = flow({ token: raw(G.isInt(d.token) ? d.token : '0') });
    o.messages = p.messages.slice();
    return o;
  }

  G.toYaml = (s) => {
    const root = { schema_version: raw(1) };
    if (has(s.schema.namespace)) root.namespace = s.schema.namespace;
    const d = s.schema.defaults;
    const def = {};
    if (d.endian && d.endian !== 'big') def.endian = d.endian;
    if (d.bit_order && d.bit_order !== 'msb_first') def.bit_order = d.bit_order;
    if (d.format && d.format !== 'binary') def.format = d.format;
    if (d.separator && d.separator !== ',') def.separator = d.separator;
    if (d.access && d.access !== 'both') def.access = d.access;
    if (d.strict_enums) def.strict_enums = true;
    if (Object.keys(def).length) root.defaults = flow(def);

    if (s.enums.length) {
      root.enums = {};
      for (const e of s.enums) {
        const vals = {};
        e.values.forEach((v) => { vals[v.name || 'unnamed'] = raw(G.isInt(v.value) ? v.value : '0'); });
        const eo = {};
        if (e.type && e.type !== 'u8') eo.type = e.type;
        if (e.strict === 'true' || e.strict === 'false') eo.strict = e.strict === 'true';
        eo.values = flow(vals);
        if (has(e.doc)) eo.doc = e.doc;
        root.enums[e.name] = flow(eo);
      }
    }
    if (s.customTypes.length || s.customChecksums.length || s.customFramers.length) {
      const c = {};
      // language blocks other than cpp: kept from the imported YAML (c:, ...)
      const otherLang = (o, t) => {
        for (const [lang, block] of Object.entries(t.lang || {})) {
          const b = {};
          for (const [k, v] of Object.entries(block)) if (has(v)) b[k] = v;
          if (Object.keys(b).length) o[lang] = flow(b);
        }
        return o;
      };
      if (s.customTypes.length) {
        c.types = {};
        s.customTypes.forEach((t) => {
          const o = {};
          if (G.isInt(t.wire_size)) o.wire_size = raw(t.wire_size);
          if (G.isInt(t.max_wire_size)) o.max_wire_size = raw(t.max_wire_size);
          const cpp = {};
          ['header', 'type', 'decode', 'encode'].forEach((k) => { if (has(t[k])) cpp[k] = t[k]; });
          if (Object.keys(cpp).length) o.cpp = flow(cpp);
          otherLang(o, t);
          c.types[t.name] = o;
        });
      }
      if (s.customChecksums.length) {
        c.checksums = {};
        s.customChecksums.forEach((t) => {
          const cpp = {};
          ['header', 'function'].forEach((k) => { if (has(t[k])) cpp[k] = t[k]; });
          const o = { size: raw(G.isInt(t.size) ? t.size : '1') };
          if (Object.keys(cpp).length) o.cpp = flow(cpp);
          c.checksums[t.name] = otherLang(o, t);
        });
      }
      if (s.customFramers.length) {
        c.framers = {};
        s.customFramers.forEach((t) => {
          const cpp = {};
          ['header', 'type'].forEach((k) => { if (has(t[k])) cpp[k] = t[k]; });
          c.framers[t.name] = otherLang(Object.keys(cpp).length ? { cpp: flow(cpp) } : {}, t);
        });
      }
      root.custom = c;
    }
    if (s.messages.length) {
      root.messages = {};
      for (const m of s.messages) {
        const o = {};
        if (has(m.type_id)) o.type_id = G.isInt(m.type_id) ? raw(m.type_id.trim()) : m.type_id.trim();
        if (has(m.format)) o.format = m.format;
        if (has(m.endian)) o.endian = m.endian;
        if (has(m.bit_order)) o.bit_order = m.bit_order;
        if (has(m.separator) && G.msgFormat(s, m) === 'text') o.separator = m.separator;
        if (has(m.access)) o.access = m.access;
        if (has(m.doc)) o.doc = m.doc;
        o.fields = m.fields.map((f) => fieldNode(s, f, m));
        const comps = (m.computed || []).filter((c) => has(c.name));
        if (comps.length && G.msgFormat(s, m) !== 'text') {
          o.computed = {};
          for (const c of comps) {
            const co = { type: c.type || 'u32', value: raw(JSON.stringify((c.value || '').trim())) };
            const sets = G.parseSetList(c.set);
            if (sets.length) co.set = flow(Object.fromEntries(sets.map(([k, v]) => [k, raw(JSON.stringify(v))])));
            if (has(c.unit)) co.unit = c.unit;
            if (has(c.doc)) co.doc = c.doc;
            o.computed[c.name.trim()] = flow(co);
          }
        }
        root.messages[m.name] = o;
      }
    }
    if (s.protocols.length) {
      root.protocols = {};
      for (const p of s.protocols) root.protocols[p.name] = protocolNode(s, p);
    }
    const out = ['# Generated by GMP Schema Studio. Compile with: gmpc --check <file>.yaml'];
    const sections = Object.keys(root);
    sections.forEach((k, i) => {
      if (i > 1 && ['enums', 'custom', 'messages', 'protocols'].includes(k)) out.push('');
      emitEntry(k, root[k], 0, out);
    });
    return out.join('\n') + '\n';
  };

  /** Minimal YAML syntax highlighting for display. */
  G.highlightYaml = (text) =>
    text.split('\n').map((line) => {
      if (/^\s*#/.test(line)) return `<span class="y-com">${G.esc(line)}</span>`;
      let h = G.esc(line);
      h = h.replace(/(&quot;(?:[^&]|&(?!quot;))*?&quot;)/g, '<span class="y-str">$1</span>');
      h = h.replace(/(^|[\s{,\-]\s?)([A-Za-z_][\w.]*)(:)(?=\s|$)/g, '$1<span class="y-key">$2</span><span class="y-pun">$3</span>');
      h = h.replace(/(:\s|\[|,\s)(-?(?:0x[0-9A-Fa-f]+|0b[01]+|\d+(?:\.\d+)?))(?=[,\s\]}]|$)/g, '$1<span class="y-num">$2</span>');
      return h;
    }).join('\n');

  // ----------------------------------------------------------------------------------------------
  // importer (js-yaml object -> state)
  // ----------------------------------------------------------------------------------------------
  G.loadJsYaml = () => new Promise((resolve, reject) => {
    if (window.jsyaml) return resolve(window.jsyaml);
    const sc = document.createElement('script');
    sc.src = 'https://cdnjs.cloudflare.com/ajax/libs/js-yaml/4.1.0/js-yaml.min.js';
    sc.onload = () => resolve(window.jsyaml);
    sc.onerror = () => reject(new Error('Could not load the YAML parser (js-yaml) - importing needs an internet connection.'));
    document.head.appendChild(sc);
  });

  const str = (v) => (v === undefined || v === null ? '' : String(v));
  const hexOrDec = (v) => (typeof v === 'number' && v > 9 ? '0x' + v.toString(16).toUpperCase() : str(v));
  const limitsFrom = (y) => Object.fromEntries(G.LIMIT_KEYS.map((k) => [k, str(y ? y[k] : undefined)]));

  /** js-yaml's default schema, except that integers beyond 2^53 load as exact BigInts instead of rounded Numbers
   *  (u64 limits, consts and CRC parameters). */
  let exactSchema = null;
  function yamlSchema() {
    if (exactSchema) return exactSchema;
    const J = window.jsyaml, base = J.types.int;
    const int = new J.Type('tag:yaml.org,2002:int', {
      kind: 'scalar',
      resolve: base.resolve,
      construct: (data) => {
        const v = base.construct(data);
        if (Number.isSafeInteger(v)) return v;
        let t = String(data).replace(/_/g, '');
        const neg = t[0] === '-';
        if (t[0] === '-' || t[0] === '+') t = t.slice(1);
        const big = BigInt(t);  // decimal, 0x, 0o and 0b
        return neg ? -big : big;
      },
      predicate: (o) => typeof o === 'bigint' || base.predicate(o),
    });  // load only: no represent / style options
    const T = J.types;
    exactSchema = J.FAILSAFE_SCHEMA.extend({
      implicit: [T.null, T.bool, int, T.float, T.timestamp, T.merge],
      explicit: [T.binary, T.omap, T.pairs, T.set],
    });
    return exactSchema;
  }

  function patternFromYaml(v) {
    if (v === undefined || v === null) return { mode: 'hex', text: '' };
    if (Array.isArray(v)) return { mode: 'hex', text: v.map((b) => Number(b).toString(16).toUpperCase().padStart(2, '0')).join(' ') };
    if (typeof v === 'number') {
      const bytes = [];
      let n = v;
      do { bytes.unshift(n & 0xff); n = Math.floor(n / 256); } while (n > 0);
      return { mode: 'hex', text: bytes.map((b) => b.toString(16).toUpperCase().padStart(2, '0')).join(' ') };
    }
    const bytes = Array.from(String(v)).map((c) => c.charCodeAt(0));
    return { mode: 'text', text: G.bytesToEscaped(bytes) };
  }

  function lenFromYaml(f, v) {
    if (typeof v === 'number') { f.lenMode = 'fixed'; f.lenValue = String(v); }
    else if (v === 'remaining') { f.lenMode = 'remaining'; f.lenValue = ''; }
    else { f.lenMode = 'expr'; f.lenValue = str(v); }
  }

  function fieldFromYaml(y) {
    if (typeof y === 'string') return G.newField({ type: y });
    if (y.pad !== undefined && y.type === undefined) return G.newField({ type: 'pad', padSize: str(y.pad), doc: str(y.doc) });
    const f = G.newField({
      name: str(y.name), type: str(y.type || 'u8'), endian: str(y.endian), bit_order: str(y.bit_order),
      max: str(y.max), scale: str(y.scale), offset: str(y.offset), unit: str(y.unit), doc: str(y.doc),
      const: y.const === undefined ? '' : hexOrDec(y.const), when: str(y.when), optional: !!y.optional,
      base: str(y.base), width: str(y.width), precision: str(y.precision),
      padding: y.padding === undefined ? '' : str(y.padding),
      ...limitsFrom(y),
    });
    const len = y.length ?? y.size ?? y.count;
    if (len !== undefined) lenFromYaml(f, len);
    if (y.terminator !== undefined) { f.lenMode = 'terminator'; f.terminator = str(y.terminator); }
    if (f.type === 'pad') f.padSize = str(len ?? 1);
    if (y.of !== undefined) {
      if (typeof y.of === 'string') f.of = y.of;
      else { f.of = str(y.of.type); f.ofEndian = str(y.of.endian); f.ofLimits = limitsFrom(y.of); }
    }
    if (y.crc !== undefined) {
      if (y.crc && typeof y.crc === 'object' && y.crc.crc) {
        const c = y.crc.crc;
        f.crc = '__crc';
        f.crcParams = { width: str(c.width), poly: hexOrDec(c.poly), init: hexOrDec(c.init ?? 0), refin: !!c.refin, refout: !!c.refout, xorout: hexOrDec(c.xorout ?? 0) };
      } else f.crc = str(y.crc);
    }
    if (y.conversions && typeof y.conversions === 'object') {
      f.conversions = Object.entries(y.conversions).map(([name, c]) =>
        G.newConversion({ name, scale: str((c || {}).scale), offset: str((c || {}).offset), unit: str((c || {}).unit) }));
    }
    if (Array.isArray(y.fields)) {
      f.bits = y.fields.map((b) => (b.pad !== undefined
        ? G.newBit({ pad: true, bits: str(b.pad) })
        : G.newBit({ name: str(b.name), bits: str(b.bits), type: str(b.enum || b.type), const: b.const === undefined ? '' : hexOrDec(b.const), scale: str(b.scale), offset: str(b.offset), doc: str(b.doc), ...limitsFrom(b) })));
    }
    return f;
  }

  const accessFromYaml = (v) => {
    if (v === undefined) return '';
    const list = Array.isArray(v) ? v : [v];
    if (list.includes('both') || (list.includes('view') && list.includes('struct'))) return 'both';
    return list[0] || '';
  };

  G.fromYaml = (text) => {
    const y = window.jsyaml.load(text, { schema: yamlSchema() }) || {};
    if (typeof y !== 'object') throw new Error('The YAML root must be a mapping');
    const s = G.emptyState();
    s.schema.namespace = str(y.namespace);
    const d = y.defaults || {};
    Object.assign(s.schema.defaults, {
      endian: d.endian || 'big', bit_order: d.bit_order || 'msb_first', format: d.format || 'binary',
      separator: d.separator || ',', access: accessFromYaml(d.access) || 'both', strict_enums: !!d.strict_enums,
    });
    for (const [name, e] of Object.entries(y.enums || {})) {
      s.enums.push(G.newEnum({
        name, type: e.type || 'u8', doc: str(e.doc), strict: e.strict === undefined ? '' : String(!!e.strict),
        values: Object.entries(e.values || {}).map(([n, v]) => G.newEnumValue(n, str(v))),
      }));
    }
    const c = y.custom || {};
    // Language blocks other than cpp: (c:, ...) are not edited in Studio but kept, so an export preserves them.
    const keepLang = (t, nonLang) => {
      const out = {};
      for (const [k, v] of Object.entries(t || {}))
        if (k !== 'cpp' && !nonLang.includes(k) && v && typeof v === 'object' && !Array.isArray(v))
          out[k] = Object.fromEntries(Object.entries(v).map(([kk, vv]) => [kk, str(vv)]));
      return out;
    };
    for (const [name, t] of Object.entries(c.types || {})) {
      const cpp = t.cpp || {};
      s.customTypes.push(G.newCustomType({ name, wire_size: str(t.wire_size), max_wire_size: str(t.max_wire_size), header: str(cpp.header), type: str(cpp.type), decode: str(cpp.decode), encode: str(cpp.encode), lang: keepLang(t, ['wire_size', 'max_wire_size', 'doc']) }));
    }
    for (const [name, t] of Object.entries(c.checksums || {})) {
      const cpp = t.cpp || {};
      s.customChecksums.push(G.newCustomChecksum({ name, size: str(t.size), header: str(cpp.header), function: str(cpp.function), lang: keepLang(t, ['size', 'doc']) }));
    }
    for (const [name, t] of Object.entries(c.framers || {})) {
      const cpp = t.cpp || {};
      s.customFramers.push(G.newCustomFramer({ name, header: str(cpp.header), type: str(cpp.type), lang: keepLang(t, ['doc']) }));
    }
    for (const [name, m] of Object.entries(y.messages || {})) {
      s.messages.push(G.newMessage({
        name, type_id: m.type_id === undefined ? '' : str(m.type_id), format: str(m.format), endian: str(m.endian),
        bit_order: str(m.bit_order), separator: str(m.separator), access: accessFromYaml(m.access), doc: str(m.doc),
        fields: (m.fields || []).map(fieldFromYaml),
        computed: Object.entries(m.computed || {}).map(([cn, c]) => G.newComputed({
          name: cn, type: str((c || {}).type), value: str((c || {}).value), unit: str((c || {}).unit), doc: str((c || {}).doc),
          set: Object.entries((c || {}).set || {}).map(([k, v]) => `${k} = ${str(v)}`).join('; '),
        })),
      }));
    }
    for (const [name, p] of Object.entries(y.protocols || {})) {
      const proto = G.newProtocol({ name, format: str(p.format), endian: str(p.endian), separator: str(p.separator), doc: str(p.doc) });
      proto.header = (p.header || []).map(fieldFromYaml);
      const fr = p.framing || {};
      const sync = patternFromYaml(fr.sync ?? fr.start);
      const end = patternFromYaml(fr.end);
      Object.assign(proto.framing, {
        type: fr.type || 'none', syncMode: sync.mode, sync: sync.text, endMode: end.mode, end: end.text,
        size: str(fr.size), maxFrame: str(fr.max_frame), framer: str(fr.framer),
        lengthField: str((fr.length || {}).field), covers: (fr.length || {}).covers || 'body', adjust: str((fr.length || {}).adjust),
        counts: (fr.length || {}).counts === 'wire' ? 'wire' : '',
      });
      if (fr.escape) {
        const ex = fr.escape;
        proto.framing.escape = {
          enabled: true, style: ex.code === 'index' ? 'index' : ex.map || ex.code === 'map' ? 'map' : 'xor',
          char: hexOrDec(ex.char), xor: hexOrDec(ex.xor ?? 0x20),
          map: ex.map ? Object.entries(ex.map).map(([a, b]) => `${hexOrDec(Number(a))}:${hexOrDec(b)}`).join(', ') : '0xC0:0xDC, 0xDB:0xDD',
          reserved: Array.isArray(ex.reserved) ? ex.reserved.map((b) => hexOrDec(b)).join(', ') : '',
          base: ex.base != null ? hexOrDec(ex.base) : '',
        };
      }
      if (p.checksum) {
        const ck = p.checksum;
        Object.assign(proto.checksum, {
          enabled: true, covers: ck.covers || 'header_body', endian: str(ck.endian),
          separator: ck.separator || '*', required: ck.required !== false, over: ck.over === 'wire' ? 'wire' : '',
        });
        if (ck.algo && typeof ck.algo === 'object' && ck.algo.crc) {
          const cr = ck.algo.crc;
          proto.checksum.algo = '__crc';
          proto.checksum.crc = { width: str(cr.width), poly: hexOrDec(cr.poly), init: hexOrDec(cr.init ?? 0), refin: !!cr.refin, refout: !!cr.refout, xorout: hexOrDec(cr.xorout ?? 0) };
        } else proto.checksum.algo = str(ck.algo);
      }
      const sel = p.message_type;
      if (sel) {
        if (sel.field !== undefined) proto.message_type = { kind: 'field', field: str(sel.field), token: '0' };
        else proto.message_type = { kind: 'token', field: '', token: str(sel.token ?? 0) };
      }
      proto.messages = (p.messages || []).map(String);
      s.protocols.push(proto);
    }
    return s;
  };
})(window.GMP);
