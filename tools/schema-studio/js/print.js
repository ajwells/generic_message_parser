// print.js - mirrors gmp::print (runtime/include/gmp/print.hpp) for the decode tester: the same text the generated
// print_to() methods produce for a decoded message, in multi-line or single-line style.
(function (G) {
  'use strict';

  // ---- number formatting (std::to_chars) ---------------------------------------------------------
  const trimZeros = (t) => (t.includes('.') ? t.replace(/0+$/, '').replace(/\.$/, '') : t);
  const pad2 = (n) => String(Math.abs(n)).padStart(2, '0');

  function special(x) {
    if (Number.isNaN(x)) return 'nan';
    if (!Number.isFinite(x)) return x < 0 ? '-inf' : 'inf';
    if (x === 0) return Object.is(x, -0) ? '-0' : '0';
    return null;
  }

  /** std::to_chars(x, chars_format::general, precision) — like printf("%.*g"). */
  function fmtGeneral(x, precision) {
    const sp = special(x);
    if (sp) return sp;
    const [mant, ex] = x.toExponential(precision - 1).split('e');
    const e = parseInt(ex, 10);
    if (e < -4 || e >= precision) return trimZeros(mant) + 'e' + (e < 0 ? '-' : '+') + pad2(e);
    return trimZeros(x.toFixed(precision - 1 - e));
  }

  /** std::to_chars(x) — shortest round-trip text, fixed unless scientific is shorter. */
  function fmtShortest(x, f32) {
    const sp = special(x);
    if (sp) return sp;
    let p = 1;
    for (; p < 17; p++) {
      const t = Number(x.toPrecision(p));
      if ((f32 ? Math.fround(t) : t) === x) break;
    }
    const neg = x < 0 ? '-' : '';
    const [mant, ex] = Math.abs(x).toExponential(p - 1).split('e');
    const e = parseInt(ex, 10);
    const digits = mant.replace('.', '');
    const sci = digits[0] + (digits.length > 1 ? '.' + digits.slice(1) : '') + 'e' + (e < 0 ? '-' : '+') + pad2(e);
    let fixed;
    if (e >= 0) fixed = digits.length <= e + 1 ? digits + '0'.repeat(e + 1 - digits.length) : digits.slice(0, e + 1) + '.' + digits.slice(e + 1);
    else fixed = '0.' + '0'.repeat(-e - 1) + digits;
    return neg + (sci.length < fixed.length ? sci : fixed);
  }

  const HEX = '0123456789ABCDEF';
  const hexByte = (b) => HEX[(b >> 4) & 15] + HEX[b & 15];

  function escChar(c, quote) {
    if (c === 10) return '\\n';
    if (c === 13) return '\\r';
    if (c === 9) return '\\t';
    if (c === 92) return '\\\\';
    if (c === quote) return '\\' + String.fromCharCode(c);
    if (c < 0x20 || c >= 0x7f) return '\\x' + hexByte(c);
    return String.fromCharCode(c);
  }
  const quoted = (bytes) => '"' + Array.from(bytes, (c) => escChar(c, 34)).join('') + '"';
  const charText = (c) => "'" + escChar(c, 39) + "'";

  // ---- the printer (same state machine as gmp::Printer) ------------------------------------------
  class Printer {
    constructor(opts) {
      this.o = { style: 'multi', indent: 2, maxItems: 64, maxBytes: 64, precision: 9, ...opts };
      this.out = '';
      this.depth = 0;
      this.first = [];
      this.noteOpen = false;
    }
    get multi() { return this.o.style === 'multi'; }
    put(t) { this.out += t; }
    push() { this.first[this.depth] = true; this.depth++; }
    pop() { if (this.depth) this.depth--; }
    isFirst() { return this.depth > 0 && this.first[this.depth - 1]; }
    clearFirst() { if (this.depth > 0) this.first[this.depth - 1] = false; }
    newline() { this.put('\n' + ' '.repeat(this.depth * this.o.indent)); }
    separate() {
      if (this.multi) this.newline();
      else this.put(this.isFirst() ? ' ' : ', ');
      this.clearFirst();
    }
    openNote() { this.put(this.noteOpen ? ', ' : ' ('); this.noteOpen = true; }
    closeNote() { if (this.noteOpen) { this.noteOpen = false; this.put(')'); } }
    begin(type) { this.closeNote(); if (type) this.put(type + ' '); this.put('{'); this.push(); }
    end() {
      this.closeNote();
      const empty = this.isFirst();
      this.pop();
      if (empty) this.put('}');
      else if (this.multi) { this.newline(); this.put('}'); }
      else this.put(' }');
    }
    key(name) { this.closeNote(); this.separate(); this.put(name + ': '); }
    unit(u) { if (u) this.put(' ' + u); }
    conversion(x, u) { this.openNote(); this.put(fmtGeneral(x, this.o.precision)); this.unit(u); }
    note(t) { this.openNote(); this.put(t); }
    more(k) { this.put(`... (+${k} more)`); }
    hex(bytes) {
      const n = bytes.length, shown = this.o.maxBytes && n > this.o.maxBytes ? this.o.maxBytes : n;
      this.put('[' + Array.from(bytes.slice(0, shown), hexByte).join(' '));
      if (shown < n) { this.put(' '); this.more(n - shown); }
      this.put(']');
    }
    list(items, blocks, each) {
      const n = items.length, shown = this.o.maxItems && n > this.o.maxItems ? this.o.maxItems : n;
      this.put('[');
      if (!n) { this.put(']'); return; }
      const lines = blocks && this.multi;
      if (lines) this.push();
      for (let i = 0; i < shown; i++) {
        if (lines) this.newline();
        else if (i) this.put(', ');
        each(items[i]);
      }
      if (shown < n) {
        if (lines) this.newline();
        else this.put(', ');
        this.more(n - shown);
      }
      if (lines) { this.pop(); this.newline(); }
      this.put(']');
    }
  }

  // ---- decoded node tree -> text ------------------------------------------------------------------
  const hasConst = (f) => f.const !== undefined && f.const !== null && String(f.const).trim() !== '';
  const present = (v) => v !== undefined && v !== null && String(v).trim() !== '';

  function enumText(s, type, v) {
    const e = G.findEnum(s, type);
    const size = G.PRIM_SIZE[(e && e.type) || 'u8'] || 1;
    const big = BigInt(typeof v === 'boolean' ? (v ? 1 : 0) : v);
    const hit = e && e.values.find((x) => BigInt(G.parseInt(x.value)) === big);
    const bits = BigInt(8 * size);
    const raw = ((big % (1n << bits)) + (1n << bits)) % (1n << bits);
    return `${hit ? hit.name : '<unknown>'} (0x${raw.toString(16).toUpperCase().padStart(2 * size, '0')})`;
  }

  const num = (v) => (typeof v === 'bigint' ? Number(v) : typeof v === 'boolean' ? (v ? 1 : 0) : v);

  /** Unit / conversions after a scalar value (emit_print_extras in the C++ backend). */
  function extras(p, f, v) {
    const scaled = present(f.scale) || present(f.offset);
    if (f.unit && !scaled) p.unit(f.unit);
    const conv = (sc, of, u) => p.conversion(num(v) * parseFloat(present(sc) ? sc : '1') + parseFloat(present(of) ? of : '0'), u);
    if (scaled) conv(f.scale, f.offset, f.unit);
    for (const c of f.conversions || []) if (c.name) conv(c.scale, c.offset, c.unit);
  }

  function value(p, s, n, f, kind) {
    switch (kind) {
      case 'int': p.put(String(n.v)); break;
      case 'float': p.put(fmtShortest(f.type === 'f32' ? Math.fround(n.v) : n.v, f.type === 'f32')); break;
      case 'bool': p.put(n.v ? 'true' : 'false'); break;
      case 'char': p.put(charText(num(n.v))); break;
      case 'enum': p.put(enumText(s, f.type, n.v)); break;
      case 'bits':
        p.begin('');
        for (const c of n.children) {
          const b = c.bit;
          if (!b || b.pad || hasConst(b)) continue;
          p.key(b.name);
          if (G.findEnum(s, c.bitType)) p.put(enumText(s, c.bitType, c.v));
          else if (c.bitType === 'bool') p.put(c.v ? 'true' : 'false');
          else p.put(String(c.v));
          if (present(b.scale) || present(b.offset))
            p.conversion(num(c.v) * parseFloat(present(b.scale) ? b.scale : '1') + parseFloat(present(b.offset) ? b.offset : '0'));
        }
        p.end();
        break;
      case 'bytes': p.hex(n.content || []); break;
      case 'string': p.put(quoted(n.content || Array.from(n.token || '', (c) => c.charCodeAt(0)))); break;
      case 'array': {
        const items = n.children.filter((c) => c.field);
        const blocks = items.length > 0 && G.fieldKind(s, items[0].field) === 'message';
        p.list(items, blocks, (c) => value(p, s, c, c.field, G.fieldKind(s, c.field)));
        break;
      }
      case 'message': message(p, s, n); break;
      default: p.put('<?>');  // custom codecs print through a gmp_print() hook in C++
    }
  }

  function message(p, s, n) {
    p.begin(n.msg ? n.msg.name : n.name);
    for (const c of n.children) {
      if (c.computed) {
        p.key(c.name);
        const t = c.computed.type;
        p.put(t === 'f32' || t === 'f64' ? fmtShortest(t === 'f32' ? Math.fround(c.v) : c.v, t === 'f32') : String(BigInt(Math.trunc(c.v))));
        p.unit(c.computed.unit);
        p.note('computed');
        continue;
      }
      const f = c.field;
      if (!f) continue;  // padding, trailing bytes, extra tokens
      const kind = G.fieldKind(s, f);
      if (kind === 'pad' || hasConst(f)) continue;
      p.key(f.name);
      if (c.absent) { p.put('<absent>'); continue; }
      value(p, s, c, f, kind);
      if (['int', 'float', 'bool', 'char'].includes(kind)) extras(p, f, c.v);
    }
    p.end();
  }

  G.printer = {
    fmtGeneral,
    fmtShortest,
    /** gmp::print output for a decoded frame (null when the frame has errors: C++ decode would fail). */
    frame(s, frame, style) {
      if (frame.errors && frame.errors.length) return null;
      const top = (frame.nodes || []).find((n) => n.top);
      if (!top) return null;
      const p = new Printer({ style });
      message(p, s, top);
      return p.out;
    },
  };
})(window.GMP);
