// layout.js - wire layout computation (sizes, offsets) mirroring generator/src/ir/lowering.cpp.
// Kept free of DOM code so the future decode tester can reuse it to walk raw bytes.
(function (G) {
  'use strict';

  /** Classify a field by its declared type. */
  G.fieldKind = (s, f) => {
    const t = f.type;
    if (t === 'pad') return 'pad';
    if (G.COMPOSITES.includes(t)) return t;
    if (G.PRIM_SIZE[t]) {
      if (t === 'f32' || t === 'f64') return 'float';
      if (t === 'bool') return 'bool';
      if (t === 'char') return 'char';
      return 'int';
    }
    if (G.findEnum(s, t)) return 'enum';
    if (G.findMessage(s, t)) return 'message';
    if (G.findCustomType(s, t)) return 'custom';
    return 'unknown';
  };

  G.KIND_COLOR = {
    int: 'int', float: 'float', bool: 'bool', char: 'bool', enum: 'enum', bits: 'bits', bytes: 'bytes',
    string: 'string', array: 'array', message: 'message', custom: 'custom', pad: 'pad', unknown: 'unknown',
  };

  const enumSize = (s, name) => G.PRIM_SIZE[(G.findEnum(s, name) || {}).type || 'u8'] || 1;

  /** Size of a single (non-array) value of type name `t`. */
  function typeSize(s, t, stack) {
    if (G.PRIM_SIZE[t]) return { fixed: G.PRIM_SIZE[t], min: G.PRIM_SIZE[t], max: G.PRIM_SIZE[t] };
    if (G.findEnum(s, t)) { const n = enumSize(s, t); return { fixed: n, min: n, max: n }; }
    const m = G.findMessage(s, t);
    if (m) {
      if (stack.includes(m.uid)) return { fixed: null, min: 0, max: null, recursive: true };
      const l = G.messageLayout(s, m, stack);
      return { fixed: l.size, min: l.min, max: l.max };
    }
    const c = G.findCustomType(s, t);
    if (c) {
      const w = G.parseInt(c.wire_size);
      const mx = G.parseInt(c.max_wire_size);
      if (!Number.isNaN(w)) return { fixed: w, min: w, max: w };
      return { fixed: null, min: 0, max: Number.isNaN(mx) ? null : mx };
    }
    return { fixed: null, min: 0, max: null, unknown: true };
  }

  G.lenFixed = (f) => (f.lenMode === 'fixed' ? G.parseInt(f.lenValue) : NaN);

  G.bitsTotal = (f) => (f.bits || []).reduce((a, b) => a + (G.parseInt(b.bits) || 0), 0);

  /** Size information for a field: { fixed, min, max } in bytes (null = unknown/unbounded). */
  G.fieldSize = (s, f, stack = []) => {
    const kind = G.fieldKind(s, f);
    let r = { fixed: null, min: 0, max: null };
    const max = G.parseInt(f.max);
    switch (kind) {
      case 'int': case 'float': case 'bool': case 'char': case 'enum': case 'message': case 'custom':
        r = typeSize(s, f.type, stack);
        break;
      case 'pad': {
        const n = G.parseInt(f.padSize);
        r = { fixed: Number.isNaN(n) ? 1 : n, min: 0, max: null };
        break;
      }
      case 'bits': {
        const total = G.bitsTotal(f);
        const n = total % 8 === 0 ? total / 8 : Math.ceil(total / 8);
        r = { fixed: n, min: n, max: n };
        break;
      }
      case 'bytes': case 'string': {
        const n = G.lenFixed(f);
        if (f.lenMode === 'fixed' && !Number.isNaN(n)) r = { fixed: n };
        else if (f.lenMode === 'terminator') r = { fixed: null, min: 1, max: Number.isNaN(max) ? null : max + 1 };
        else r = { fixed: null, min: 0, max: Number.isNaN(max) ? null : max };
        break;
      }
      case 'array': {
        const e = typeSize(s, f.of, stack);
        const emax = e.fixed ?? e.max;
        const n = G.lenFixed(f);
        if (f.lenMode === 'fixed' && !Number.isNaN(n)) {
          r = { fixed: e.fixed == null ? null : e.fixed * n, min: (e.min || 0) * n, max: emax == null ? null : emax * n };
        } else {
          r = { fixed: null, min: 0, max: emax == null || Number.isNaN(max) ? null : emax * max };
        }
        r.elem = e;
        break;
      }
      default: break;
    }
    if (r.fixed != null) { r.min = r.fixed; r.max = r.fixed; }
    if (f.when && f.when.trim()) {
      r = { ...r, max: r.fixed ?? r.max, present: r.fixed, fixed: null, min: 0 };
    }
    r.kind = kind;
    return r;
  };

  /** Human readable type label for a field. */
  G.typeLabel = (s, f) => {
    const k = G.fieldKind(s, f);
    const len = () => {
      if (f.lenMode === 'fixed') return f.lenValue || '?';
      if (f.lenMode === 'remaining') return 'remaining';
      if (f.lenMode === 'terminator') return 'until ' + (f.terminator || '0');
      return f.lenValue || '?';
    };
    switch (k) {
      case 'bits': return `bits:${G.bitsTotal(f)}`;
      case 'bytes': return `bytes[${len()}]`;
      case 'string': return `string[${len()}]`;
      case 'array': return `${f.of || '?'}[${len()}]`;
      case 'pad': return `pad ${f.padSize || 1}`;
      default: return f.type || '?';
    }
  };

  G.sizeLabel = (sz) => {
    if (sz.fixed != null) return `${sz.fixed} B`;
    if (sz.max != null) return sz.min === sz.max ? `${sz.max} B` : `${sz.min}–${sz.max} B`;
    return `${sz.min}+ B`;
  };

  /**
   * Layout of a message.
   * rows: [{ field, size, offset }] where offset is the static byte offset (null once a variable field precedes).
   * Text messages report token indices instead.
   */
  G.messageLayout = (s, m, stack = []) => {
    const text = !m.isHeader && G.msgFormat(s, m) === 'text';
    const inner = stack.concat([m.uid]);
    const rows = [];
    if (text) {
      let tok = 0;
      for (const f of m.fields) {
        const n = f.type === 'pad' ? Math.max(1, G.parseInt(f.padSize) || 1) : 1;
        rows.push({ field: f, size: G.fieldSize(s, f, inner), token: tok, tokens: n });
        tok += n;
      }
      return { text: true, rows, tokens: tok, size: null, min: 0, max: null, fixed: false };
    }
    let off = 0, staticOff = true, min = 0, max = 0, fixed = true;
    for (const f of m.fields) {
      const sz = G.fieldSize(s, f, inner);
      rows.push({ field: f, size: sz, offset: staticOff ? off : null });
      if (sz.fixed != null && staticOff) off += sz.fixed;
      else staticOff = false;
      if (sz.fixed == null) fixed = false;
      min += sz.min || 0;
      max = max == null || sz.max == null ? null : max + sz.max;
    }
    return { text: false, rows, fixed, size: fixed ? off : null, min, max };
  };

  /** Frame layout of a protocol: prefix / header / body alternatives / checksum / suffix. */
  G.protocolLayout = (s, p) => {
    const text = G.protoFormat(s, p) === 'text';
    const fr = p.framing;
    const prefix = G.patternBytes(fr.syncMode, fr.sync) || [];
    const suffix = G.patternBytes(fr.endMode, fr.end) || [];
    const header = G.messageLayout(s, G.headerMessage(s, p));
    const headerSize = text ? 0 : header.size || 0;
    let checksumSize = 0;
    if (p.checksum.enabled) {
      if (p.checksum.algo === '__crc') checksumSize = Math.ceil((G.parseInt(p.checksum.crc.width) || 0) / 8);
      else if (G.CHECKSUMS[p.checksum.algo]) checksumSize = G.CHECKSUMS[p.checksum.algo];
      else {
        const c = s.customChecksums.find((x) => x.name === p.checksum.algo);
        checksumSize = c ? G.parseInt(c.size) || 0 : 0;
      }
    }
    const trailer = text ? 0 : checksumSize;
    const bodies = p.messages
      .map((n) => G.findMessage(s, n))
      .filter(Boolean)
      .map((m) => ({ message: m, layout: G.messageLayout(s, m) }));
    let bodyMin = null, bodyMax = 0;
    for (const b of bodies) {
      const mx = b.layout.size ?? b.layout.max;
      bodyMin = bodyMin == null ? b.layout.min : Math.min(bodyMin, b.layout.min);
      bodyMax = bodyMax == null || mx == null ? null : Math.max(bodyMax, mx);
    }
    const overhead = prefix.length + headerSize + trailer + suffix.length;
    let maxFrame = G.parseInt(fr.maxFrame);
    let maxFrameDerived = false;
    if (Number.isNaN(maxFrame)) {
      maxFrame = null;
      if (fr.type === 'fixed' && !Number.isNaN(G.parseInt(fr.size))) { maxFrame = G.parseInt(fr.size); maxFrameDerived = true; }
      else if (fr.type === 'fixed' && bodies.length && bodies.every((b) => b.layout.size != null)) {
        maxFrame = overhead + Math.max(...bodies.map((b) => b.layout.size)); maxFrameDerived = true;
      } else if (fr.type !== 'none' && !text && bodyMax != null && bodies.length) {
        let interior = headerSize + bodyMax + trailer;
        if (fr.escape.enabled && fr.type === 'delimited') interior *= 2;
        maxFrame = prefix.length + interior + suffix.length;
        maxFrameDerived = true;
      }
    }
    return { text, prefix, suffix, header, headerSize, checksumSize, trailer, bodies, bodyMin: bodyMin ?? 0, bodyMax, overhead, maxFrame, maxFrameDerived };
  };
})(window.GMP);
