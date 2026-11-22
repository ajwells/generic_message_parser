// decoder.js - the Quick engine of the decode tester: decodes raw bytes against the schema as it is being edited, with
// no gmpc. Checksums, framers, escaping, number reading and text parsing come from the shared JavaScript runtime
// (runtime-js/gmsg.js, the runtime of `gmpc --lang js` output); this file interprets the schema the way the generated
// code does (field order, lengths, conditions, message_type selection) and builds a tree of nodes with byte ranges
// (in input coordinates) for highlighting. DOM-free.
(function (G) {
  'use strict';

  const D = (G.decoder = {});
  const gmsg = window.gmsg;
  const Err = gmsg.Err;
  /** Run fn; returns [0, value] or [GmpError code, undefined]. */
  const attempt = (fn) => {
    try { return [0, fn()]; } catch (e) { if (e instanceof gmsg.GmpError) return [e.code, undefined]; throw e; }
  };

  // ----------------------------------------------------------------------------------------------
  // input parsing
  // ----------------------------------------------------------------------------------------------
  /** Parse hex input: "AA 55 01", "aa5501", "0xAA, 0x55", with # comments. Returns { bytes, error }. */
  D.parseHex = (text) => {
    const cleaned = text.replace(/#[^\n]*/g, ' ').replace(/0x/gi, ' ').replace(/[\s,;:\-]+/g, ' ').trim();
    if (!cleaned) return { bytes: new Uint8Array(0) };
    const out = [];
    for (const tok of cleaned.split(' ')) {
      if (!/^[0-9a-f]+$/i.test(tok)) return { bytes: new Uint8Array(out), error: `"${tok}" is not hex` };
      if (tok.length % 2) {
        if (tok.length === 1) { out.push(parseInt(tok, 16)); continue; }
        return { bytes: new Uint8Array(out), error: `"${tok}" has an odd number of hex digits` };
      }
      for (let i = 0; i < tok.length; i += 2) out.push(parseInt(tok.slice(i, i + 2), 16));
    }
    return { bytes: new Uint8Array(out) };
  };
  D.parseText = (text) => ({ bytes: new Uint8Array(G.parseEscapedText(text)) });

  D.hex = (bytes, max = 64) => {
    const arr = Array.from(bytes.slice(0, max), (b) => b.toString(16).toUpperCase().padStart(2, '0'));
    return arr.join(' ') + (bytes.length > max ? ` … (+${bytes.length - max})` : '');
  };
  const printable = (b) => (b >= 32 && b < 127 ? String.fromCharCode(b) : b === 13 ? '\\r' : b === 10 ? '\\n' : b === 9 ? '\\t' : '\\x' + b.toString(16).padStart(2, '0'));
  D.ascii = (bytes) => Array.from(bytes, printable).join('');

  // ----------------------------------------------------------------------------------------------
  // checksums (gmsg.checksum)
  // ----------------------------------------------------------------------------------------------
  /** A schema integer literal as BigInt ('0x1021', '4129', '0b1'); 0n when it is not an integer. */
  const bigOf = (v) => { try { return BigInt(String(v == null ? '' : v).trim() || '0'); } catch (_) { return 0n; } };

  /** The gmsg algorithm of a protocol checksum / field crc ({ algo, crc }), or null for a custom function. */
  D.algorithm = (s, ck) => {
    if (ck.algo === '__crc') {
      const c = ck.crc || {};
      const width = G.parseInt(c.width) || 8;
      const n = (v) => (width > 32 ? bigOf(v) : Number(bigOf(v)));
      return gmsg.checksum.crc({ width, poly: n(c.poly), init: n(c.init), refin: !!c.refin, refout: !!c.refout, xorout: n(c.xorout) });
    }
    const a = gmsg.checksum[ck.algo];
    return a && typeof a.compute === 'function' ? a : null;
  };

  /** Compute a checksum. Returns BigInt, or null when it cannot be computed here (custom function). */
  D.checksum = (s, ck, data) => {
    const a = D.algorithm(s, ck);
    return a ? BigInt(a.compute(Uint8Array.from(data))) : null;
  };

  // ----------------------------------------------------------------------------------------------
  // expressions (same grammar as generator/src/schema/expr.cpp)
  // ----------------------------------------------------------------------------------------------
  const exprCache = new Map();
  function parseExpr(text) {
    if (exprCache.has(text)) return exprCache.get(text);
    const toks = [];
    const re = /\s*(0x[0-9a-f]+|0b[01]+|\d+|[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*|\|\||&&|==|!=|<=|>=|<<|>>|[-+*/%&|^!~<>()])/giy;
    let m, pos = 0;
    const t = text.trim();
    while (pos < t.length) {
      re.lastIndex = pos;
      m = re.exec(t);
      if (!m) throw new Error(`unexpected "${t.slice(pos)}"`);
      toks.push(m[1]);
      pos = re.lastIndex;
    }
    let i = 0;
    const peek = () => toks[i];
    const bin = (ops, next) => () => {
      let l = next();
      while (ops.includes(peek())) { const op = toks[i++]; l = { op, l, r: next() }; }
      return l;
    };
    const primary = () => {
      const tk = toks[i++];
      if (tk === undefined) throw new Error('unexpected end');
      if (tk === '(') { const e = or(); if (toks[i++] !== ')') throw new Error('expected )'); return e; }
      if (/^\d|^0[xb]/i.test(tk)) return { num: G.parseInt(tk) };
      if (/^[A-Za-z_]/.test(tk)) return { ref: tk };
      throw new Error(`unexpected "${tk}"`);
    };
    const unary = () => (['!', '-', '~'].includes(peek()) ? { op: 'u' + toks[i++], l: unary() } : primary());
    const mul = bin(['*', '/', '%'], unary), add = bin(['+', '-'], mul), shift = bin(['<<', '>>'], add);
    const band = bin(['&'], shift), bxor = bin(['^'], band), bor = bin(['|'], bxor);
    const cmp = () => { const l = bor(); if (['==', '!=', '<', '<=', '>', '>='].includes(peek())) { const op = toks[i++]; return { op, l, r: bor() }; } return l; };
    const and = bin(['&&'], cmp), or = bin(['||'], and);
    const ast = or();
    if (i !== toks.length) throw new Error(`unexpected "${toks[i]}"`);
    exprCache.set(text, ast);
    return ast;
  }
  /** Bitwise operators on values wider than 32 bits (JS `|`, `&`, `^`, `~` truncate to int32). */
  const big = (x) => BigInt(Math.trunc(Number(x)));
  function evalExpr(ast, lookup) {
    if ('num' in ast) return ast.num;
    if ('ref' in ast) { const v = lookup(ast.ref); if (v === undefined) throw new Error(`"${ast.ref}" is not available`); return v; }
    const l = () => evalExpr(ast.l, lookup), r = () => evalExpr(ast.r, lookup);
    switch (ast.op) {
      case 'u!': return l() ? 0 : 1;
      case 'u-': return -l();
      case 'u~': return Number(~big(l()));
      case '*': return l() * r();
      case '/': { const d = r(); if (!d) throw new Error('division by zero'); return Math.trunc(l() / d); }
      case '%': { const d = r(); if (!d) throw new Error('division by zero'); return l() % d; }
      case '+': return l() + r();
      case '-': return l() - r();
      case '<<': return l() * 2 ** r();
      case '>>': return Math.floor(l() / 2 ** r());
      case '&': return Number(big(l()) & big(r()));
      case '^': return Number(big(l()) ^ big(r()));
      case '|': return Number(big(l()) | big(r()));
      case '==': return l() === r() ? 1 : 0;
      case '!=': return l() !== r() ? 1 : 0;
      case '<': return l() < r() ? 1 : 0;
      case '<=': return l() <= r() ? 1 : 0;
      case '>': return l() > r() ? 1 : 0;
      case '>=': return l() >= r() ? 1 : 0;
      case '&&': return l() && r() ? 1 : 0;
      case '||': return l() || r() ? 1 : 0;
      default: throw new Error('bad operator ' + ast.op);
    }
  }
  D.evalExpr = (text, lookup) => evalExpr(parseExpr(text), lookup);

  // ----------------------------------------------------------------------------------------------
  // primitive reads
  // ----------------------------------------------------------------------------------------------
  /** Unsigned integer of n bytes as BigInt (checksums, length and type fields). */
  const readUint = (buf, off, n, endian) => (n === 8 ? new gmsg.Reader(buf, off).u64(endian) : BigInt(gmsg.loadUint(buf, off, n, endian)));
  /** A primitive value: BigInt for integer types and char, boolean for bool, Number for floats. */
  function readPrim(buf, off, type, endian) {
    const v = new gmsg.Reader(buf, off).read(type === 'char' ? 'u8' : type, endian);
    return typeof v === 'number' && type !== 'f32' && type !== 'f64' ? BigInt(v) : v;
  }
  const extractBits = (buf, off, bitOff, nbits, order) => BigInt(gmsg.extractBits(buf.subarray(off), bitOff, nbits, order));
  const toNum = (v) => (typeof v === 'bigint' ? Number(v) : typeof v === 'boolean' ? (v ? 1 : 0) : v);
  /** Quote text that D.ascii() already escaped (no second round of escaping). */
  const q = (t) => '"' + t + '"';
  const fmtNum = (v) => {
    if (typeof v === 'bigint') {
      const hex = (v < 0n ? '-0x' + (-v).toString(16) : '0x' + v.toString(16)).toUpperCase().replace('0X', '0x');
      return v > 9n || v < -9n ? `${v} (${hex})` : String(v);
    }
    if (typeof v === 'number') return Number.isInteger(v) ? v.toFixed(1) : String(+v.toPrecision(9));
    return String(v);
  };
  const scaled = (f, v) => {
    if (!(f.scale || f.offset)) return null;
    const s = parseFloat(f.scale || '1'), o = parseFloat(f.offset || '0');
    return +(toNum(v) * s + o).toPrecision(10);
  };
  const enumName = (s, enumType, v) => {
    const e = G.findEnum(s, enumType);
    const hit = e && e.values.find((x) => G.parseInt(x.value) === toNum(v));
    return hit ? `${e.name}.${hit.name} (${toNum(v)})` : `${toNum(v)} (not a ${enumType} value)`;
  };
  /** Error text when `v` is not declared in a strict enum (decode fails with UnknownEnum), else ''. */
  const strictEnumError = (s, enumType, v) => {
    const e = G.findEnum(s, enumType);
    if (!e || !G.enumStrict(s, e)) return '';
    return e.values.some((x) => G.parseInt(x.value) === toNum(v)) ? '' : `unknown ${enumType} value (strict enum)`;
  };

  // ----------------------------------------------------------------------------------------------
  // binary message decoding
  // ----------------------------------------------------------------------------------------------
  class Stop extends Error {}

  function node(name, kind, start, end, extra = {}) {
    return { name, kind, start, end, value: '', raw: '', children: [], error: '', note: '', ...extra };
  }

  /**
   * Decode the fields of binary message `m` from buf[pos..limit).
   * env collects decoded scalar values for expression evaluation.
   */
  function decodeBinary(s, m, buf, pos, limit, depth = 0) {
    const nodes = [];
    const env = new Map();
    const lookup = (ref) => {
      const [head, member] = ref.split('.');
      if (env.has(head)) {
        const v = env.get(head);
        if (member !== undefined) return v && typeof v === 'object' ? v[member] : undefined;
        return v;
      }
      const e = G.findEnum(s, head);
      if (e && member) { const val = e.values.find((x) => x.name === member); return val ? G.parseInt(val.value) : undefined; }
      return undefined;
    };
    let error = '';
    const endian = (f) => f.endian || (m.isHeader ? m.endian : G.msgEndian(s, m));
    const bitOrder = (f) => f.bit_order || (m.isHeader ? s.schema.defaults.bit_order : G.msgBitOrder(s, m));
    const need = (n, f, start) => {
      if (limit - pos < n) throw new Stop(`truncated: ${f.name || 'field'} needs ${n} byte${n === 1 ? '' : 's'}, ${Math.max(0, limit - pos)} left`);
      return start;
    };

    try {
      for (const f of m.fields) {
        const kind = G.fieldKind(s, f);
        const label = kind === 'pad' ? 'padding' : f.name;
        if (f.when && f.when.trim()) {
          let present;
          try { present = !!D.evalExpr(f.when, lookup); } catch (e) { throw new Stop(`${f.name}: cannot evaluate "${f.when}": ${e.message}`); }
          if (!present) { nodes.push(node(label, kind, pos, pos, { note: `absent (${f.when} is false)`, type: G.typeLabel(s, f), field: f, absent: true })); continue; }
        }
        const n = decodeField(s, m, f, kind, label, buf, () => pos, (p) => { pos = p; }, limit, env, lookup, endian(f), bitOrder(f), need, depth);
        n.field = f;
        nodes.push(n);
        if (n.fatal) { error = n.error; break; }
      }
    } catch (e) {
      if (!(e instanceof Stop)) throw e;
      error = e.message;
      nodes.push(node('⚠', 'error', pos, limit, { error: e.message }));
    }
    // computed values: evaluated from the decoded fields, no bytes of their own
    if (!error) {
      for (const c of m.computed || []) {
        if (!c.name || !c.value) continue;
        const cn = node(c.name, 'computed', pos, pos, { type: `${c.type || '?'} (computed)`, note: `= ${c.value}` });
        try {
          const v = D.evalExpr(c.value, lookup);
          const isFloat = c.type === 'f32' || c.type === 'f64';
          cn.computed = c;
          cn.v = v;
          cn.value = isFloat ? String(v) : fmtNum(BigInt(Math.trunc(v)));
          if (c.unit) cn.value += ' ' + c.unit;
        } catch (e) { cn.error = e.message; }
        nodes.push(cn);
      }
    }
    return { nodes, pos, error };
  }

  function decodeScalar(s, f, kind, label, buf, at, endian, typeName) {
    const t = kind === 'enum' ? (G.findEnum(s, typeName).type || 'u8') : typeName;
    const n = G.PRIM_SIZE[t];
    const v = readPrim(buf, at, t, endian);
    const nd = node(label, kind, at, at + n, { raw: D.hex(buf.slice(at, at + n)), type: typeName, v });
    nd.value = D.scalarText(s, f, kind, typeName, v);
    if (kind === 'enum') nd.error = strictEnumError(s, typeName, v);
    if (f.const && G.isInt(f.const) && toNum(v) !== G.parseInt(f.const)) nd.error = `expected const ${f.const}`;
    return { nd, v, n };
  }

  /** Display text of a scalar / enum value v (BigInt for integers and chars, Number, boolean), with its scaled
   *  value, unit and conversions. Also used by the exact engine (exact.js) for values of the generated code. */
  D.scalarText = (s, f, kind, typeName, v) => {
    const t = kind === 'enum' ? (G.findEnum(s, typeName).type || 'u8') : typeName;
    let text;
    if (kind === 'enum') text = enumName(s, typeName, v);
    else if (t === 'char') text = `'${printable(Number(v))}'`;
    else text = fmtNum(v);
    const sc = scaled(f, v);
    if (sc !== null) text += `  →  ${sc}${f.unit ? ' ' + f.unit : ''}`;
    else if (f.unit) text += ` ${f.unit}`;
    return text + conversionsText(f, v);
  };
  D.printable = (b) => printable(b);

  /** Extra named conversions (`conversions:`), e.g. " · 77 degF". */
  function conversionsText(f, v) {
    return (f.conversions || []).filter((c) => c.name && (c.scale || c.offset)).map((c) => {
      const val = +(toNum(v) * parseFloat(c.scale || '1') + parseFloat(c.offset || '0')).toPrecision(10);
      return ` · ${val}${c.unit ? ' ' + c.unit : ''}`;
    }).join('');
  }

  /** Field-level checksum right after a value at buf[at, at+n): adds a child node and flags mismatches. */
  function checkFieldCrc(s, f, nd, buf, at, n, endian, need) {
    const c = G.checksumSize(s, f.crc, f.crcParams);
    if (!c) return 0;
    need(n + c, f);
    const stored = readUint(buf, at + n, c, endian);  // stored in the field's byte order
    const computed = D.checksum(s, { algo: f.crc, crc: f.crcParams }, buf.slice(at, at + n));
    const label = f.crc === '__crc' ? `CRC-${f.crcParams.width}` : f.crc;
    const hexw = (v) => '0x' + v.toString(16).toUpperCase().padStart(2 * c, '0');
    const cn = node('crc', 'check', at + n, at + n + c, { raw: D.hex(buf.slice(at + n, at + n + c)), type: label, value: hexw(stored) });
    if (computed === null) cn.note = 'custom checksum, not verified here';
    else if (computed !== stored) { cn.error = `mismatch: computed ${hexw(computed)}`; nd.error = 'bad CRC'; }
    else cn.note = '✓';
    nd.children.push(cn);
    nd.end = at + n + c;
    return c;
  }

  function decodeField(s, m, f, kind, label, buf, getPos, setPos, limit, env, lookup, endian, bitOrder, need, depth) {
    let pos = getPos();
    const start = pos;
    const lenOf = (what) => {
      if (f.lenMode === 'fixed') return G.parseInt(f.lenValue);
      if (f.lenMode === 'expr') {
        let v;
        try { v = D.evalExpr(f.lenValue, lookup); } catch (e) { throw new Stop(`${f.name}: cannot evaluate ${what} "${f.lenValue}": ${e.message}`); }
        if (v < 0) throw new Stop(`${f.name}: ${what} ${v} is negative`);
        return v;
      }
      return null;
    };
    switch (kind) {
      case 'int': case 'float': case 'bool': case 'char': case 'enum': {
        const n = kind === 'enum' ? G.PRIM_SIZE[(G.findEnum(s, f.type).type || 'u8')] : G.PRIM_SIZE[f.type];
        need(n, f);
        const { nd, v } = decodeScalar(s, f, kind, label, buf, pos, endian, f.type);
        env.set(f.name, toNum(v));
        const c = f.crc ? checkFieldCrc(s, f, nd, buf, pos, n, endian, (k) => need(k, f)) : 0;
        setPos(pos + n + c);
        return nd;
      }
      case 'pad': {
        const n = G.parseInt(f.padSize) || 1;
        need(n, f);
        setPos(pos + n);
        return node('padding', 'pad', start, pos + n, { raw: D.hex(buf.slice(start, pos + n)), type: `pad ${n}` });
      }
      case 'bits': {
        const total = G.bitsTotal(f), n = Math.ceil(total / 8);
        need(n, f);
        const nd = node(label, 'bits', pos, pos + n, { raw: D.hex(buf.slice(pos, pos + n)), type: G.typeLabel(s, f) });
        nd.value = Array.from(buf.slice(pos, pos + n), (b) => b.toString(2).padStart(8, '0')).join(' ');
        const members = {};
        for (const b of f.bits) {
          const w = G.parseInt(b.bits) || 1;
          let off = 0;
          for (const x of f.bits) { if (x === b) break; off += G.parseInt(x.bits) || 1; }
          const v = extractBits(buf, pos, off, w, bitOrder);
          const child = node(b.pad ? 'pad' : b.name, b.pad ? 'pad' : 'bits', pos, pos + n, { type: `${w} bit${w > 1 ? 's' : ''} @ ${off}`, raw: '0b' + v.toString(2).padStart(w, '0') });
          let val = v;
          const t = b.pad ? '' : b.type || (w === 1 ? 'bool' : '');
          if (G.findEnum(s, t)) { child.value = enumName(s, t, v); child.error = strictEnumError(s, t, v); }
          else if (t === 'bool') child.value = v ? 'true' : 'false';
          else {
            if (t && t[0] === 'i' && w > 0 && (v >> BigInt(w - 1)) & 1n) val = v - (1n << BigInt(w));
            child.value = fmtNum(val);
            const sc = scaled(b, val);
            if (sc !== null) child.value += `  →  ${sc}`;
          }
          if (b.const && G.isInt(b.const) && toNum(v) !== G.parseInt(b.const)) child.error = `expected const ${b.const}`;
          if (!b.pad) members[b.name] = toNum(val);
          child.bit = b;
          child.bitType = t;
          child.v = val;
          nd.children.push(child);
        }
        env.set(f.name, members);
        setPos(pos + n);
        return nd;
      }
      case 'bytes': case 'string': {
        let n;
        if (f.lenMode === 'remaining') n = limit - pos;
        else if (f.lenMode === 'terminator') {
          const t = G.parseInt(f.terminator) || 0;
          let k = pos;
          while (k < limit && buf[k] !== t) k++;
          if (k >= limit) throw new Stop(`${f.name}: terminator ${G.hex2(t)} not found`);
          n = k - pos + 1;
        } else n = lenOf('length');
        need(n, f);
        const content = f.lenMode === 'terminator' ? buf.slice(pos, pos + n - 1) : buf.slice(pos, pos + n);
        const nd = node(label, kind, pos, pos + n, { raw: D.hex(buf.slice(pos, pos + n)), type: G.typeLabel(s, f) });
        if (kind === 'string') {
          let txt = D.ascii(content);
          if (f.lenMode === 'fixed') {
            const pad = f.padding === '' || f.padding == null ? 0 : G.isInt(f.padding) ? G.parseInt(f.padding) : f.padding.charCodeAt(0);
            let e = content.length;
            while (e > 0 && content[e - 1] === pad) e--;
            txt = D.ascii(content.slice(0, e));
            nd.content = content.slice(0, e);
          } else nd.content = content;
          nd.value = q(txt);
        } else { nd.value = `${n} byte${n === 1 ? '' : 's'}`; nd.content = content; }
        if ((f.lenMode !== 'fixed') && G.isInt(f.max) && content.length > G.parseInt(f.max)) nd.note = `exceeds max ${f.max} (struct decode would fail)`;
        setPos(pos + n);
        return nd;
      }
      case 'array': {
        const ek = G.fieldKind(s, { type: f.of });
        const esz = G.fieldSize(s, { type: f.of }).fixed;
        const nd = node(label, 'array', pos, pos, { type: G.typeLabel(s, f) });
        let count = f.lenMode === 'remaining' ? null : lenOf('count');
        if (count === null && esz) {
          if ((limit - pos) % esz) throw new Stop(`${f.name}: ${limit - pos} remaining bytes is not a multiple of the ${esz}-byte element`);
          count = (limit - pos) / esz;
        }
        const elemField = { ...G.newField({ type: f.of, endian: f.ofEndian || f.endian }), name: '' };
        let i = 0;
        while (count === null ? pos < limit : i < count) {
          const sub = decodeField(s, m, elemField, ek, `[${i}]`, buf, () => pos, (p) => { pos = p; }, limit, new Map(), lookup, f.ofEndian || endian, bitOrder, need, depth + 1);
          sub.field = elemField;
          nd.children.push(sub);
          if (sub.fatal) { nd.error = sub.error; nd.fatal = true; break; }
          i++;
          if (i > 10000) throw new Stop(`${f.name}: too many elements`);
        }
        nd.end = pos;
        nd.value = `${i} element${i === 1 ? '' : 's'}`;
        if (G.isInt(f.max) && i > G.parseInt(f.max)) nd.note = `exceeds max ${f.max} (struct decode would fail)`;
        setPos(pos);
        return nd;
      }
      case 'message': {
        const child = G.findMessage(s, f.type);
        if (depth > 16) throw new Stop('nesting too deep');
        const r = decodeBinary(s, child, buf, pos, limit, depth + 1);
        const nd = node(label, 'message', pos, r.pos, { type: f.type, children: r.nodes, value: child.name, msg: child });
        if (r.error) { nd.error = r.error; nd.fatal = true; }
        setPos(r.pos);
        return nd;
      }
      case 'custom': {
        const c = G.findCustomType(s, f.type);
        const w = G.parseInt(c.wire_size);
        if (Number.isNaN(w)) {
          const nd = node(label, 'custom', pos, limit, { type: f.type, error: 'custom type without wire_size: its size is decided by C++ code, so decoding cannot continue here', fatal: true });
          return nd;
        }
        need(w, f);
        setPos(pos + w);
        return node(label, 'custom', start, pos + w, { type: f.type, raw: D.hex(buf.slice(start, pos + w)), value: `${w} bytes`, note: `custom codec ${c.decode || ''} (not run here)` });
      }
      default:
        throw new Stop(`${f.name}: unknown type "${f.type}"`);
    }
  }

  // ----------------------------------------------------------------------------------------------
  // text message decoding
  // ----------------------------------------------------------------------------------------------
  /** Split buf[start..end) on sep, returning tokens with positions. */
  function tokenize(buf, start, end, sep) {
    const toks = [];
    let s0 = start;
    for (let i = start; i <= end; i++) {
      if (i === end || buf[i] === sep) { toks.push({ start: s0, end: i, text: D.ascii(buf.slice(s0, i)), raw: gmsg.latin1String(buf, s0, i) }); s0 = i + 1; }
    }
    return toks;
  }

  function decodeText(s, m, buf, start, end) {
    const sep = (G.msgSeparator(s, m) || ',').charCodeAt(0);
    const toks = start < end ? tokenize(buf, start, end, sep) : [];
    const nodes = [];
    let ti = 0, error = '';
    for (const f of m.fields) {
      const kind = G.fieldKind(s, f);
      if (kind === 'pad') {
        const n = Math.max(1, G.parseInt(f.padSize) || 1);
        const first = toks[ti], last = toks[Math.min(ti + n, toks.length) - 1];
        nodes.push(node('padding', 'pad', first ? first.start : end, last ? last.end : end, { type: `${n} token${n > 1 ? 's' : ''}`, raw: toks.slice(ti, ti + n).map((t) => t.text).join(',') }));
        ti += n;
        continue;
      }
      const tok = toks[ti] || { start: end, end, text: '', raw: '' };
      const nd = node(f.name, kind, tok.start, tok.end, { raw: q(tok.text), type: f.type + (f.optional ? ' (optional)' : ''), field: f, token: tok.raw });
      ti++;
      if (tok.text === '') {
        if (f.optional) { nd.value = '— (empty, optional)'; nd.absent = true; }
        else if (kind === 'string') nd.value = '""';
        else nd.error = 'missing (empty token)';
        nodes.push(nd);
        continue;
      }
      const base = G.parseInt(f.base) === 16 ? 16 : 10;
      if (kind === 'int' || kind === 'enum') {
        const prim = kind === 'enum' ? G.findEnum(s, f.type).type || 'u8' : f.type;
        const [code, val] = attempt(() => gmsg.text.parseInt(tok.raw, prim, base));
        if (code === Err.OutOfRange) nd.error = `out of range for ${prim}`;
        else if (code) nd.error = `not a base-${base} integer`;
        else {
          const v = BigInt(val);
          nd.v = v;
          nd.value = kind === 'enum' ? enumName(s, f.type, v) : fmtNum(v);
          if (kind === 'enum') nd.error = strictEnumError(s, f.type, v);
          const sc = scaled(f, v);
          if (sc !== null) nd.value += `  →  ${sc}${f.unit ? ' ' + f.unit : ''}`;
          if (kind === 'int') nd.value += conversionsText(f, v);
          if (f.const && G.isInt(f.const) && Number(v) !== G.parseInt(f.const)) nd.error = `expected const ${f.const}`;
        }
      } else if (kind === 'float') {
        const isF32 = f.type === 'f32';
        const [code, v] = attempt(() => gmsg.text.parseFloat(tok.raw, isF32));
        if (code === Err.OutOfRange) nd.error = `out of range for ${f.type}`;
        else if (code) nd.error = 'not a number';
        else {
          nd.v = v;
          nd.value = gmsg.formatFloat(v, -1, isF32);
          const sc = scaled(f, v);
          if (sc !== null) nd.value += `  →  ${sc}${f.unit ? ' ' + f.unit : ''}`;
          nd.value += conversionsText(f, v);
        }
      } else if (kind === 'char') {
        const [code] = attempt(() => gmsg.text.parseChar(tok.raw));
        if (code) nd.error = 'expected a single character'; else { nd.value = `'${printable(tok.raw.charCodeAt(0))}'`; nd.v = tok.raw.charCodeAt(0); }
      } else if (kind === 'bool') {
        const [code, v] = attempt(() => gmsg.text.parseInt(tok.raw, 'i32'));  // as generated code: parse_int<int> != 0
        if (code === Err.OutOfRange) nd.error = 'out of range for int';
        else if (code) nd.error = 'expected an integer';
        else { nd.value = v ? 'true' : 'false'; nd.v = v !== 0; }
      } else if (kind === 'string') {
        nd.value = q(tok.text);
        if (G.isInt(f.max) && tok.text.length > G.parseInt(f.max)) nd.note = `exceeds max ${f.max} (struct decode would fail)`;
      } else if (kind === 'custom') {
        nd.value = q(tok.text);
        nd.note = 'custom codec (not run here)';
      } else nd.error = `type "${f.type}" not supported in text`;
      nodes.push(nd);
    }
    if (toks.length > ti) nodes.push(node(`+${toks.length - ti} extra token${toks.length - ti > 1 ? 's' : ''}`, 'pad', toks[ti].start, toks[toks.length - 1].end, { note: 'ignored by the decoder' }));
    return { nodes, error };
  }

  // ----------------------------------------------------------------------------------------------
  // framing: the gmsg scanners with the configuration gmpc generates (backend emit_framing)
  // ----------------------------------------------------------------------------------------------
  /** The gmsg escape configuration of a protocol (null without escaping). */
  function escapeOf(s, p) {
    if (!['delimited', 'fixed', 'length_prefixed'].includes(p.framing.type)) return null;
    const et = G.escapeTable(s, p);
    if (!et) return null;
    if (et.style === 'xor') return gmsg.xorEscape(et.esc, et.xor, et.reserved.filter((b) => b !== et.esc));
    return gmsg.mapEscape(et.esc, et.table.map(([r]) => r), et.table.map(([, c]) => c));
  }

  /**
   * The frame scanner of protocol p: { scan(bytes) -> { kind, size, consume, onError, want, error }, escape } where
   * error is a description ('' for a silent skip) and want the frame size when known.
   */
  function framer(s, p, L) {
    const fr = p.framing;
    const esc = escapeOf(s, p);
    const escape = esc || gmsg.NO_ESCAPE;
    const maxFrame = L.maxFrame || Infinity;
    const sync = Uint8Array.from(L.prefix), suffix = Uint8Array.from(L.suffix);
    const whole = (b) => ({ kind: 'frame', size: b.length, consume: b.length, onError: b.length });
    if (fr.type === 'none' || fr.type === 'custom') return { scan: whole, escape: esc };
    let raw, lengthInfo = null;
    if (fr.type === 'fixed') {
      const sizes = L.bodies.map((b) => b.layout.size);
      const size = G.parseInt(fr.size) || (sizes.length && sizes.every((x) => x != null) ? L.overhead + Math.max(...sizes) : L.maxFrame);
      if (!size) return { scan: (b) => ({ kind: 'skip', size: b.length, error: 'fixed frame size unknown' }), escape: esc };
      const cfg = { sync, frameSize: size, escape, suffixLen: suffix.length };
      raw = (b) => gmsg.scanFixed(b, cfg);
    } else if (fr.type === 'length_prefixed') {
      const hm = G.headerMessage(s, p);
      const row = G.messageLayout(s, hm).rows.find((x) => x.field.name === fr.lengthField);
      if (!row) return { scan: (b) => ({ kind: 'skip', size: b.length, error: 'length field not found' }), escape: esc };
      const lengthSize = G.PRIM_SIZE[row.field.type] || 1;
      const lengthOffset = L.prefix.length + row.offset;
      const lengthEndian = row.field.endian || G.protoEndian(s, p);
      const adjust = G.parseInt(fr.adjust) || 0;
      const countsWire = !!esc && fr.counts === 'wire';
      let add = adjust + L.suffix.length;
      if (countsWire) add = adjust;  // body wire size = value + adjust
      else if (fr.covers === 'header_body') add += L.prefix.length + L.trailer;
      else if (fr.covers === 'body_checksum') add += L.prefix.length + L.headerSize;
      else if (fr.covers !== 'all') add += L.prefix.length + L.headerSize + L.trailer;
      const cfg = {
        sync, lengthOffset, lengthSize, lengthEndian, add, minFrame: L.overhead, maxFrame, escape, suffixLen: suffix.length,
        countsWire, headerLen: L.headerSize, trailerLen: L.trailer, wireIncludesTrailer: fr.covers === 'body_checksum',
      };
      raw = (b) => gmsg.scanLengthPrefixed(b, cfg);
      lengthInfo = { lengthOffset, lengthSize, lengthEndian, add, countsWire };
    } else {
      const cfg = { start: sync, end: suffix, escape, maxFrame };
      raw = (b) => gmsg.scanDelimited(b, cfg);
    }
    const describe = (b, r) => {
      const at = r.size;
      switch (r.error) {
        case Err.None: return '';
        case Err.BadSync: return 'bad sync';
        case Err.InvalidValue: return `invalid escape code ${G.hex2(b[at + 1])}`;
        case Err.BadDelimiter:
          return fr.type === 'delimited' ? 'start delimiter inside frame (truncated frame)'
            : `reserved byte ${G.hex2(b[at])} inside a frame (truncated frame)`;
        case Err.BadLength:
          if (fr.type === 'delimited') return 'no end delimiter within max_frame';
          if (lengthInfo && !esc && b.length >= lengthInfo.lengthOffset + lengthInfo.lengthSize) {
            const v = gmsg.loadUint(b, lengthInfo.lengthOffset, lengthInfo.lengthSize, lengthInfo.lengthEndian);
            return `bad length (${v} → frame of ${v + lengthInfo.add} bytes)`;
          }
          return lengthInfo && lengthInfo.countsWire ? 'bad length (body wire size out of range, or it ends inside an escape pair)'
            : 'bad length (frame size out of range or larger than max_frame)';
        default: return gmsg.errorText(r.error);
      }
    };
    return {
      escape: esc,
      scan(b) {
        const r = raw(b);
        if (r.kind === 'frame') return { kind: 'frame', size: r.size, consume: r.consume, onError: r.consumeOnError };
        if (r.kind === 'skip') return { kind: 'skip', size: r.size, error: describe(b, r) };
        return { kind: 'need_more', want: r.size > b.length ? r.size : 0 };
      },
    };
  }

  // ----------------------------------------------------------------------------------------------
  // frame processing
  // ----------------------------------------------------------------------------------------------
  /** Map node ranges from a (possibly unescaped) working buffer back to input offsets. */
  /** A node ending at working index e ends where working byte e starts in the input, so escape
   *  bytes inside the field are included. `after` = input offset just past the interior. */
  function remap(nodes, origin, after) {
    const map = (i) => (i >= origin.length ? after : origin[i]);
    for (const n of nodes) {
      const a = map(n.start);
      const b = n.end > n.start ? map(n.end) : a;
      n.start = a; n.end = b;
      remap(n.children, origin, after);
    }
  }

  function processFrame(s, p, L, F, input, fstart, fend) {
    const fr = p.framing, ck = p.checksum;
    const frame = { start: fstart, end: fend, nodes: [], errors: [], message: null };
    const err = (m) => frame.errors.push(m);
    const pre = L.prefix.length, suf = L.suffix.length;
    if (fend - fstart < L.overhead) { err(`frame of ${fend - fstart} bytes is shorter than the ${L.overhead}-byte overhead`); return frame; }
    if (pre) frame.nodes.push(node(fr.type === 'delimited' ? 'start' : 'sync', 'frame', fstart, fstart + pre, { raw: D.hex(input.slice(fstart, fstart + pre)), value: L.text ? q(D.ascii(input.slice(fstart, fstart + pre))) : D.hex(input.slice(fstart, fstart + pre)) }));

    // interior, unescaped if needed (origin[i] = input offset of working byte i)
    let work = Array.from(input.slice(fstart + pre, fend - suf));
    let origin = work.map((_, i) => fstart + pre + i);
    const esc = F.escape;
    if (esc) {
      const w2 = [], o2 = [];
      for (let i = 0; i < work.length; i++) {
        let b = work[i];
        const o = origin[i];
        if (b === esc.esc) {
          if (++i >= work.length) { err('escape byte at end of frame'); return frame; }
          const d = esc.decode(work[i]);
          if (d < 0) { err(`invalid escape code ${G.hex2(work[i])}`); return frame; }
          b = d;
        }
        w2.push(b); o2.push(o);
      }
      work = w2; origin = o2;
    }
    const buf = Uint8Array.from(work);

    if (L.text) {
      // text: [start] body [*HH] [end]
      let bodyEnd = buf.length;
      let ckNode = null;
      if (ck.enabled) {
        const sepc = (ck.separator || '*').charCodeAt(0);
        const star = buf.lastIndexOf(sepc);
        if (star < 0) {
          if (ck.required) err('checksum missing');
        } else {
          const hexTxt = D.ascii(buf.slice(star + 1));
          const stored = /^[0-9a-f]+$/i.test(hexTxt) && hexTxt.length === 2 * L.checksumSize ? BigInt('0x' + hexTxt) : null;
          const computed = D.checksum(s, ck, buf.slice(0, star));
          ckNode = node('checksum', 'check', star, buf.length, { raw: q(D.ascii(buf.slice(star))), type: ck.algo === '__crc' ? 'custom CRC' : ck.algo });
          if (stored === null) { ckNode.error = `expected ${2 * L.checksumSize} hex digits`; err('malformed checksum'); }
          else if (computed === null) ckNode.note = 'custom checksum function - not verified here';
          else if (stored !== computed) { ckNode.error = `mismatch: computed ${computed.toString(16).toUpperCase().padStart(2 * L.checksumSize, '0')}`; err('bad checksum'); }
          else ckNode.value = `✓ ${hexTxt.toUpperCase()}`;
          bodyEnd = star;
        }
      }
      // message_type by token 0
      const sep = (p.separator || s.schema.defaults.separator || ',').charCodeAt(0);
      let idEnd = 0;
      while (idEnd < bodyEnd && buf[idEnd] !== sep) idEnd++;
      const id = D.ascii(buf.slice(0, idEnd));
      const cands = p.messages.map((n) => G.findMessage(s, n)).filter(Boolean);
      let msg = null;
      if (p.message_type.kind === 'token') {
        frame.nodes.push(node('type_id', 'frame', 0, idEnd, { value: q(id), raw: '' }));
        msg = cands.find((m) => (m.type_id || '') === id);
        if (!msg) err(`unknown type_id "${id}"`);
      } else msg = cands[0];
      const bodyStart = p.message_type.kind === 'token' ? Math.min(idEnd + 1, bodyEnd) : 0;
      if (msg) {
        frame.message = msg.name;
        const r = decodeText(s, msg, buf, bodyStart, bodyEnd);
        const mn = node(msg.name, 'message', bodyStart, bodyEnd, { children: r.nodes, value: `type_id ${q(id)}`, type: 'message', msg, top: true });
        frame.nodes.push(mn);
        if (r.nodes.some((n) => n.error)) err(fieldErrors(r.nodes));
      }
      if (ckNode) frame.nodes.push(ckNode);
      remap(frame.nodes.slice(pre ? 1 : 0), origin, fend - suf);
    } else {
      // binary: [prefix] header body [checksum] [suffix]
      const hs = L.headerSize, tr = L.trailer;
      if (buf.length < hs + tr) { err('frame too short for header + checksum'); return frame; }
      const inner = [];
      let hdrVals = null;
      if (p.header.length) {
        const hm = G.headerMessage(s, p);
        const r = decodeBinary(s, hm, buf, 0, hs);
        inner.push(node('header', 'frame', 0, hs, { children: r.nodes, value: `${hs} bytes` }));
        if (r.error || r.nodes.some((n) => n.error)) err('header error');
        hdrVals = r.nodes;
      }
      if (ck.enabled) {
        let covered;
        if (esc && ck.over === 'wire') {
          // computed over the escaped bytes as received: map working offsets back to the input
          const at = (i) => (i >= origin.length ? fend - suf : origin[i]);
          const ckStart = at(buf.length - tr);
          if (ck.covers === 'body') covered = input.slice(at(hs), ckStart);
          else if (ck.covers === 'all') covered = input.slice(fstart, ckStart);
          else covered = input.slice(fstart + pre, ckStart);
        } else if (ck.covers === 'body') covered = buf.slice(hs, buf.length - tr);
        else if (ck.covers === 'all') covered = Uint8Array.from([...input.slice(fstart, fstart + pre), ...buf.slice(0, buf.length - tr)]);
        else covered = buf.slice(0, buf.length - tr);
        const stored = readUint(buf, buf.length - tr, tr, ck.endian || G.protoEndian(s, p));
        const computed = D.checksum(s, ck, covered);
        const cn = node('checksum', 'check', buf.length - tr, buf.length, { raw: D.hex(buf.slice(buf.length - tr)), type: (ck.algo === '__crc' ? 'custom CRC' : ck.algo) + (esc && ck.over === 'wire' ? ' (over wire bytes)' : '') });
        const hexw = 2 * tr;
        if (computed === null) cn.note = 'custom checksum function - not verified here';
        else if (stored !== computed) { cn.error = `mismatch: stored 0x${stored.toString(16).toUpperCase().padStart(hexw, '0')}, computed 0x${computed.toString(16).toUpperCase().padStart(hexw, '0')}`; err('bad checksum'); }
        else cn.value = `✓ 0x${stored.toString(16).toUpperCase().padStart(hexw, '0')}`;
        inner.push(cn);
      }
      // message_type: pick the message
      const cands = p.messages.map((n) => G.findMessage(s, n)).filter(Boolean);
      let msg = null;
      if (p.message_type.kind === 'field') {
        const hn = (hdrVals || []).find((n) => n.name === p.message_type.field);
        const hf = p.header.find((f) => f.name === p.message_type.field);
        const raw = hn ? Number(readUint(buf, hn.start, hn.end - hn.start, hf.endian || G.protoEndian(s, p))) : NaN;
        msg = cands.find((m) => {
          if (G.isInt(m.type_id)) return G.parseInt(m.type_id) === raw;
          const e = hf && G.findEnum(s, hf.type);
          const v = e && e.values.find((x) => x.name === m.type_id);
          return v && G.parseInt(v.value) === raw;
        });
        if (!msg) err(`unknown type_id ${Number.isNaN(raw) ? '?' : raw}`);
      } else msg = cands[0];
      if (msg) {
        frame.message = msg.name;
        const r = decodeBinary(s, msg, buf, hs, buf.length - tr);
        const mn = node(msg.name, 'message', hs, r.pos, { children: r.nodes, type: 'message', value: msg.type_id ? `type_id ${msg.type_id}` : '', msg, top: true });
        const trailing = r.pos < buf.length - tr
          ? node(`+${buf.length - tr - r.pos} trailing`, 'pad', r.pos, buf.length - tr, { note: fr.type === 'fixed' ? 'fixed-frame padding' : 'bytes after the message (ignored)', raw: D.hex(buf.slice(r.pos, buf.length - tr)) })
          : null;
        inner.splice(p.header.length ? 1 : 0, 0, ...(trailing ? [mn, trailing] : [mn]));
        if (r.error) err(r.error);
        else if (hasError(r.nodes)) err(fieldErrors(r.nodes));
      }
      remap(inner, origin, fend - suf);
      frame.nodes.push(...inner);
    }
    if (suf) frame.nodes.push(node('end', 'frame', fend - suf, fend, { raw: D.hex(input.slice(fend - suf, fend)), value: L.text ? q(D.ascii(input.slice(fend - suf, fend))) : D.hex(input.slice(fend - suf, fend)) }));
    if (esc) {
      const escaped = input.slice(fstart + pre, fend - suf).length - buf.length;
      frame.notes = [escaped ? `frame unescaped before decoding (${escaped} escape pair${escaped === 1 ? '' : 's'})` : 'frame unescaped before decoding (no escapes)'];
    }
    return frame;
  }
  const hasError = (nodes) => nodes.some((n) => n.error || hasError(n.children));
  /** Summary for a frame whose fields have errors, e.g. "temperature: bad CRC". */
  const fieldErrors = (nodes) => {
    const out = [];
    const walk = (ns) => ns.forEach((n) => { if (n.error && n.kind !== 'check') out.push(`${n.name}: ${n.error}`); walk(n.children); });
    walk(nodes);
    return out.length ? out.slice(0, 3).join('; ') + (out.length > 3 ? `; +${out.length - 3} more` : '') : 'field errors';
  };

  // ----------------------------------------------------------------------------------------------
  // entry points
  // ----------------------------------------------------------------------------------------------
  /** Decode `input` as a stream of frames of protocol p. */
  D.decodeProtocol = (s, p, input) => {
    input = gmsg.toBytes(input);
    const L = G.protocolLayout(s, p);
    const F = framer(s, p, L);
    const items = [];
    let pos = 0;
    let guard = 0;
    let failedEnd = 0;
    while (pos < input.length && guard++ < 10000) {
      const sc = F.scan(input.subarray(pos));
      if (sc.kind === 'need_more') {
        // A lone closing flag shared with a (not yet received) next frame is not an error.
        const rest = input.length - pos;
        if (p.framing.type === 'delimited' && rest === L.prefix.length && L.prefix.length &&
            L.prefix.every((b, i) => b === L.suffix[i] && input[pos + i] === b)) break;
        items.push({ type: 'incomplete', start: pos, end: input.length, error: sc.want ? `incomplete frame (needs ${sc.want} bytes, have ${input.length - pos})` : 'incomplete frame (no end found)' });
        break;
      }
      if (sc.kind === 'skip') {
        const n = Math.max(1, Math.min(sc.size, input.length - pos));
        // Resync skips inside a frame already reported as bad are not shown again.
        const from = Math.max(pos, failedEnd);
        if (from < pos + n) {
          const last = items[items.length - 1];
          if (last && last.type === 'garbage' && last.end === from && last.error === sc.error) last.end = pos + n;
          else items.push({ type: 'garbage', start: from, end: pos + n, error: sc.error || 'skipped' });
        }
        pos += n;
        continue;
      }
      const fr = processFrame(s, p, L, F, input, pos, pos + sc.size);
      items.push({ type: 'frame', ...fr });
      if (fr.errors.length) failedEnd = Math.max(failedEnd, pos + sc.size);
      pos += fr.errors.length ? sc.onError : sc.consume;
    }
    return { items, layout: L };
  };

  /** Decode `input` as a single message (no framing). */
  D.decodeMessage = (s, m, input) => {
    input = gmsg.toBytes(input);
    if (G.msgFormat(s, m) === 'text') {
      let end = input.length;
      while (end > 0 && (input[end - 1] === 10 || input[end - 1] === 13)) end--;
      const r = decodeText(s, m, input, 0, end);
      return { items: [{ type: 'frame', start: 0, end, message: m.name, errors: hasError(r.nodes) ? [fieldErrors(r.nodes)] : [], nodes: [node(m.name, 'message', 0, end, { children: r.nodes, type: 'message', msg: m, top: true })] }] };
    }
    const r = decodeBinary(s, m, input, 0, input.length);
    const errors = r.error ? [r.error] : hasError(r.nodes) ? [fieldErrors(r.nodes)] : [];
    const items = [{ type: 'frame', start: 0, end: r.pos, message: m.name, errors, nodes: [node(m.name, 'message', 0, r.pos, { children: r.nodes, type: 'message', msg: m, top: true })] }];
    if (r.pos < input.length && !r.error) items.push({ type: 'garbage', start: r.pos, end: input.length, error: 'trailing bytes (ignored by decode)' });
    return { items };
  };

  // ----------------------------------------------------------------------------------------------
  // example input for the built-in example schema
  // ----------------------------------------------------------------------------------------------
  /** Sample input for a decode target of the built-in example schema (null for anything else). */
  const hexLine = (a) => a.map((b) => b.toString(16).toUpperCase().padStart(2, '0')).join(' ');

  /**
   * Sample input for the Sensirion SHT4x example schemas (examples/sensirion_sht4x): 6-byte responses with a
   * CRC-8 after every word, and 1-byte commands. Recognised by structure, not by file name.
   */
  function sht4xExample(s, target) {
    const meas = s.messages.find((m) => m.name === 'Measurement' &&
      ['temperature', 'humidity'].every((n) => m.fields.some((f) => f.name === n && f.crc === 'crc8_nrsc5')));
    if (!meas || !target) return null;
    const [kind, name] = target.split(/:(.*)/s);
    const word = (v) => {
      const w = [(v >> 8) & 0xff, v & 0xff];
      return [...w, Number(D.checksum(s, { algo: 'crc8_nrsc5' }, Uint8Array.from(w)))];
    };
    const tTicks = (c) => Math.round(((c + 45) * 65535) / 175);
    const rhTicks = (rh) => Math.round(((rh + 6) * 65535) / 125);
    const measurement = (c, rh) => [...word(tTicks(c)), ...word(rhTicks(rh))];
    const serial = [...word(0x0F4F), ...word(0x5A3C)];

    if (target === 'protocol:MeasurementResponse' || target === 'message:Measurement') {
      const bad = measurement(25, 54);
      bad[4] ^= 0x01; // flip a humidity bit: its CRC no longer matches
      const lines = [
        '# 25.00 degC, 54.00 %RH (high precision measurement)',
        hexLine(measurement(25, 54)),
      ];
      if (kind === 'message') return lines.join('\n');
      return [...lines,
        '# -10.00 degC, 85.00 %RH (outdoor, winter)',
        hexLine(measurement(-10, 85)),
        '# 72.50 degC, 12.00 %RH (read after a 200 mW / 1 s heater pulse)',
        hexLine(measurement(72.5, 12)),
        '# 4.00 degC, 101.20 %RH (condensing; RH is not cropped to 100)',
        hexLine(measurement(4, 101.2)),
        '# 25.00 degC, humidity word corrupted on the bus (bad CRC)',
        hexLine(bad),
      ].join('\n');
    }
    if (target === 'protocol:SerialNumberResponse' || target === 'message:SerialNumber') {
      const bad = serial.slice();
      bad[2] ^= 0xff;
      return ['# serial number 0x0F4F5A3C', hexLine(serial)]
        .concat(kind === 'protocol' ? ['# same response with the first CRC byte damaged', hexLine(bad)] : [])
        .join('\n');
    }
    if (kind === 'message') {
      const m = s.messages.find((x) => x.name === name);
      // CommandFrame (enum variant): one known command; the decode tester flags unknown values of strict enums
      const cmd = m && m.fields.length === 1 && G.findEnum(s, m.fields[0].type);
      if (cmd) {
        const v = cmd.values.find((x) => x.name === 'measure_high_precision') || cmd.values[0];
        return v ? `# ${cmd.name}.${v.name}\n${hexLine([G.parseInt(v.value) & 0xff])}` : null;
      }
    }
    return null;
  }

  /**
   * Sample frames for a byte-stuffed binary protocol: a message made of fixed-size integer / bytes fields,
   * filled with reserved byte values so every escape pair shows up. Also shows resync after a frame
   * cut short and an unknown escape code.
   */
  function escapedExample(s, target) {
    const [kind, name] = (target || '').split(/:(.*)/s);
    if (kind !== 'protocol') return null;
    const p = s.protocols.find((x) => x.name === name);
    if (!p || G.protoFormat(s, p) !== 'binary' || !['delimited', 'fixed', 'length_prefixed'].includes(p.framing.type)) return null;
    const et = G.escapeTable(s, p);
    if (!et || et.errors.length) return null;
    const L = G.protocolLayout(s, p);
    const fr = p.framing, ck = p.checksum;
    const isInt = (t) => /^[ui](8|16|32|64)$/.test(t);
    const simple = (f) => (isInt(f.type) && !f.when && !f.crc) || (f.type === 'bytes' && f.lenMode === 'fixed' && G.isInt(f.lenValue) && !f.when);
    const m = p.messages.map((n) => G.findMessage(s, n)).find((x) => x && x.fields.length && x.fields.every(simple));
    if (!m) return null;
    // bytes cycling through the reserved values, mixed with ordinary ones
    const plain = [0x12, 0x34, 0x56, 0x78, 0x9A];
    let k = 0;
    const nextByte = () => { const i = k++; return i % 2 === 0 ? et.reserved[(i / 2) % et.reserved.length] : plain[(i >> 1) % plain.length]; };
    const putUint = (out, v, n, endian) => {
      const b = [];
      for (let i = 0; i < n; i++) b.push(Number((BigInt(v) >> BigInt(8 * i)) & 0xFFn));
      out.push(...(endian === 'big' ? b.reverse() : b));
    };
    const body = [];
    const shown = [];
    for (const f of m.fields) {
      if (f.type === 'bytes') {
        const n = G.parseInt(f.lenValue);
        const b = Array.from({ length: n }, nextByte);
        body.push(...b);
        shown.push(`${f.name} ${hexLine(b)}`);
        continue;
      }
      const n = G.PRIM_SIZE[f.type];
      let b;
      if (G.isInt(f.const)) { b = []; putUint(b, G.parseInt(f.const), n, f.endian || G.msgEndian(s, m)); }
      else b = Array.from({ length: n }, nextByte);
      body.push(...b);
      if (!G.isInt(f.const)) shown.push(`${f.name} ${hexLine(b)}`);
    }
    if (fr.type === 'fixed') {
      const size = G.parseInt(fr.size) || L.overhead + body.length;
      while (body.length < size - L.overhead) body.push(0);
    }
    // header
    const hdr = [];
    let typeId = m.type_id;
    for (const f of p.header) {
      const n = G.PRIM_SIZE[f.type] || 1;
      const endian = f.endian || G.protoEndian(s, p);
      let v = 0;
      if (G.isInt(f.const)) v = G.parseInt(f.const);
      else if (p.message_type.kind === 'field' && f.name === p.message_type.field) {
        if (G.isInt(typeId)) v = G.parseInt(typeId);
        else { const e = G.findEnum(s, f.type); const x = e && e.values.find((y) => y.name === typeId); v = x ? G.parseInt(x.value) : 0; }
      } else if (fr.type === 'length_prefixed' && f.name === fr.lengthField) {
        const wire = fr.counts === 'wire';
        const wireBody = wire ? body.length + body.filter((b) => et.isReserved(b)).length : body.length;
        let trailerBytes = L.trailer;
        if (wire && fr.covers === 'body_checksum' && ck.enabled && L.trailer) {
          // the checksum only covers the body here, so it is known before the length
          const eb = body.flatMap((b) => (et.isReserved(b) ? [et.esc, et.encode(b)] : [b]));
          const sum = D.checksum(s, ck, Uint8Array.from(ck.over === 'wire' ? eb : body));
          if (sum === null) return null;
          const c = [];
          putUint(c, sum, L.trailer, ck.endian || G.protoEndian(s, p));
          trailerBytes += c.filter((b) => et.isReserved(b)).length;
        }
        const covered = fr.covers === 'header_body' ? L.headerSize + body.length
          : fr.covers === 'all' ? L.prefix.length + L.headerSize + body.length + L.trailer
            : fr.covers === 'body_checksum' ? wireBody + trailerBytes : wireBody;
        v = covered - (G.parseInt(fr.adjust) || 0);
      }
      if (!isInt(f.type)) return null;
      putUint(hdr, v, n, endian);
    }
    const escape = (bytes) => bytes.flatMap((b) => (et.isReserved(b) ? [et.esc, et.encode(b)] : [b]));
    const interior = [...hdr, ...body];
    let wire;
    if (ck.enabled && L.trailer) {
      const cend = ck.endian || G.protoEndian(s, p);
      let sum;
      if (ck.over === 'wire') {
        const eh = escape(hdr), eb = escape(body);
        const covered = ck.covers === 'body' ? eb : ck.covers === 'all' ? [...L.prefix, ...eh, ...eb] : [...eh, ...eb];
        sum = D.checksum(s, ck, Uint8Array.from(covered));
        if (sum === null) return null;
        const c = [];
        putUint(c, sum, L.trailer, cend);
        wire = [...eh, ...eb, ...escape(c)];
      } else {
        const covered = ck.covers === 'body' ? body : ck.covers === 'all' ? [...L.prefix, ...interior] : interior;
        sum = D.checksum(s, ck, Uint8Array.from(covered));
        if (sum === null) return null;
        const c = [];
        putUint(c, sum, L.trailer, cend);
        wire = escape([...interior, ...c]);
      }
    } else wire = escape(interior);
    const frame = [...L.prefix, ...wire, ...L.suffix];
    const pairs = et.table.map(([r, c]) => `${G.hex2(r)} -> ${G.hex2(et.esc).slice(2)} ${G.hex2(c).slice(2)}`).join(', ');
    const lines = [
      `# ${m.name}: ${shown.join(', ')}`,
      `# reserved bytes are sent as escape + code: ${pairs}`,
      hexLine(frame),
    ];
    const sharedFlag = fr.type === 'delimited' && L.prefix.length === L.suffix.length && L.prefix.every((b, i) => b === L.suffix[i]);
    if (L.prefix.length && et.isReserved(L.prefix[0]) && !sharedFlag) {
      lines.push(`# the same frame cut short: the reserved sync byte ${G.hex2(L.prefix[0])} of the next frame starts a new frame at once`,
        hexLine(frame.slice(0, Math.max(L.prefix.length + 1, Math.floor(frame.length / 2)))) + ' ' + hexLine(frame));
    }
    const at = frame.findIndex((b, i) => i >= L.prefix.length && b === et.esc);
    if (at >= 0 && et.style !== 'xor') {
      const unused = Array.from({ length: 256 }, (_, i) => i).find((c) => !et.isReserved(c) && !et.table.some((x) => x[1] === c));
      const bad = frame.slice();
      bad[at + 1] = unused;
      lines.push(`# unknown code ${G.hex2(unused)} after the escape byte`, hexLine(bad));
    }
    return lines.join('\n');
  }

  /** A message made only of one constant byte (e.g. an SHT4x command) decodes from that byte. */
  function constByteExample(s, target) {
    const [kind, name] = (target || '').split(/:(.*)/s);
    if (kind !== 'message') return null;
    const m = s.messages.find((x) => x.name === name);
    if (!m || m.fields.length !== 1) return null;
    const f = m.fields[0];
    if (f.type !== 'u8' || !G.isInt(f.const)) return null;
    return `# ${m.name}: the command byte\n${hexLine([G.parseInt(f.const) & 0xff])}`;
  }

  D.exampleInput = (s, target) => D.exampleInputFor(s, target) || constByteExample(s, target);

  D.exampleInputFor = (s, target) => {
    const sht = sht4xExample(s, target);
    if (sht) return sht;
    const stuffed = escapedExample(s, target);
    if (stuffed) return stuffed;
    if (target === 'protocol:Nmea' && s.protocols.some((p) => p.name === 'Nmea')) {
      // One sentence per line; the tester's "Newlines" option turns each line break into \r\n.
      return [
        '$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47',
        '$GPGGA,123520,48x7.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*1C',
        '$GPXYZ,1*51',
        '$GPGGA,000000,,,,,0,00,,,,,,,*66',
        '',
      ].join('\n');
    }
    const link = s.protocols.find((p) => p.name === 'Link');
    if (!link || (target && target !== 'protocol:Link')) return null;
    const frame = (id, body) => {
      const hdr = [id, (body.length >> 8) & 0xff, body.length & 0xff];
      const crc = Number(D.checksum(s, link.checksum, Uint8Array.from([...hdr, ...body])));
      return [0xAA, 0x55, ...hdr, ...body, crc >> 8, crc & 0xff];
    };
    const label = Array.from('pump-01 ', (c) => c.charCodeAt(0));
    // Status: magic 5A, uptime 3600, voltage 12.600 V, armed=1 mode=run level=-3, label "pump-01"
    const status = frame(1, [0x5A, 0x00, 0x00, 0x0E, 0x10, 0x31, 0x38, 0xBA, ...label]);
    // Status in fault mode (adds the conditional Fault block)
    const fault = frame(1, [0x5A, 0x00, 0x00, 0x0E, 0x11, 0x2F, 0x44, 0xDA, 0x00, 0x42, 0x03, 0x6B, ...label]);
    // Samples: channel 3, values 100, -100, 250
    const samples = frame(2, [0x03, 0x03, 0x00, 0x64, 0xFF, 0x9C, 0x00, 0xFA]);
    const bad = samples.slice();
    bad[bad.length - 3] ^= 0x01; // corrupt one body byte
    const fmt = (a) => a.map((b) => b.toString(16).toUpperCase().padStart(2, '0')).join(' ');
    return [
      '# Status frame',
      fmt(status),
      '# line noise',
      '13 37',
      '# Status with fault',
      fmt(fault),
      '# Samples',
      fmt(samples),
      '# Samples with a corrupted byte (bad CRC)',
      fmt(bad),
    ].join('\n');
  };
})(window.GMP);
