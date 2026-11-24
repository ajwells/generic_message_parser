// print.js - mirrors gmsg::print (runtime/include/gmp/print.hpp) for the decode tester: the same text the generated
// print_to() methods produce for a decoded message, in multi-line or single-line style.
(function (G) {
  'use strict';

  // The printer state machine and number formatting are the shared runtime's (gmsg.Printer, gmsg.formatFloat /
  // formatGeneral, the same code `gmpc --lang js` output prints with); this file walks the decoded node tree the way
  // the generated print_to() methods walk a message.
  const gmsg = window.gmsg;
  const fmtGeneral = (x, precision) => gmsg.formatGeneral(x, precision);
  const fmtShortest = (x, f32) => gmsg.formatFloat(f32 ? Math.fround(x) : x, -1, f32);
  const STYLE = { multi: 'multi_line', single: 'single_line', multi_line: 'multi_line', single_line: 'single_line' };

  // ---- decoded node tree -> text ------------------------------------------------------------------
  const hasConst = (f) => f.const !== undefined && f.const !== null && String(f.const).trim() !== '';
  const present = (v) => v !== undefined && v !== null && String(v).trim() !== '';

  function enumValue(p, s, type, v) {
    const e = G.findEnum(s, type);
    const size = G.PRIM_SIZE[(e && e.type) || 'u8'] || 1;
    const big = BigInt(typeof v === 'boolean' ? (v ? 1 : 0) : v);
    const hit = e && e.values.find((x) => BigInt(G.parseInt(x.value)) === big);
    p.enumValue(hit ? hit.name : '', big, size);
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
      case 'int': p.int(n.v); break;
      case 'float': p.float(f.type === 'f32' ? Math.fround(n.v) : n.v, f.type === 'f32'); break;
      case 'bool': p.bool(n.v); break;
      case 'char': p.char(num(n.v)); break;
      case 'enum': enumValue(p, s, f.type, n.v); break;
      case 'bits':
        p.begin('');
        for (const c of n.children) {
          const b = c.bit;
          if (!b || b.pad || hasConst(b)) continue;
          p.key(b.name);
          if (G.findEnum(s, c.bitType)) enumValue(p, s, c.bitType, c.v);
          else if (c.bitType === 'bool') p.bool(c.v);
          else p.int(c.v);
          if (present(b.scale) || present(b.offset))
            p.conversion(num(c.v) * parseFloat(present(b.scale) ? b.scale : '1') + parseFloat(present(b.offset) ? b.offset : '0'));
        }
        p.end();
        break;
      case 'bytes': p.hex(n.content || []); break;
      case 'string': p.str(n.content ? gmsg.latin1String(Uint8Array.from(n.content)) : n.token || ''); break;
      case 'array': {
        const items = n.children.filter((c) => c.field);
        const blocks = items.length > 0 && G.fieldKind(s, items[0].field) === 'message';
        p.list(items, (c) => value(p, s, c, c.field, G.fieldKind(s, c.field)), blocks);
        break;
      }
      case 'message': message(p, s, n); break;
      default: p.put('<?>');  // custom codecs print through a gmsg_print() hook in C++
    }
  }

  function message(p, s, n) {
    p.begin(n.msg ? n.msg.name : n.name);
    for (const c of n.children) {
      if (c.computed) {
        p.key(c.name);
        const t = c.computed.type;
        if (t === 'f32' || t === 'f64') p.float(t === 'f32' ? Math.fround(c.v) : c.v, t === 'f32');
        else p.int(BigInt(Math.trunc(c.v)));
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
    /** gmsg::print output for a decoded frame (null when the frame has errors: C++ decode would fail). */
    frame(s, frame, style) {
      if (frame.errors && frame.errors.length) return null;
      // the gmpc engine (exact.js): the generated code's own print text
      if (frame.printed) return style === 'single' || style === 'single_line' ? frame.printed.single : frame.printed.multi;
      const top = (frame.nodes || []).find((n) => n.top);
      if (!top) return null;
      const p = new gmsg.Printer({ style: STYLE[style] || 'multi_line' });
      message(p, s, top);
      p.flush();
      return p.out;
    },
  };
})(window.GMP);
