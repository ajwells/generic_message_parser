// validate.js - client-side checks mirroring the generator's diagnostics (loader, lowering, validator).
// Not exhaustive: `gmpc --check` remains the source of truth.
(function (G) {
  'use strict';

  const BUILTIN_NAMES = new Set([...G.PRIMS, ...G.COMPOSITES, 'uint8', 'uint16', 'uint32', 'uint64', 'int8', 'int16', 'int32', 'int64', 'float', 'double']);

  /** Identifiers referenced by an expression (dotted refs are returned whole). */
  G.exprRefs = (text) => {
    const refs = [];
    const re = /\b([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?)\b/g;
    const cleaned = (text || '').replace(/\b0[xb][0-9a-f]+\b/gi, ' ');
    let m;
    while ((m = re.exec(cleaned))) refs.push(m[1]);
    return refs;
  };

  G.exprSyntaxOk = (text) => {
    const t = (text || '').trim();
    if (!t) return false;
    if (/[^A-Za-z0-9_.\s+\-*/%&|^!~<>=()]/.test(t)) return false;
    let depth = 0;
    for (const c of t) { if (c === '(') depth++; if (c === ')' && --depth < 0) return false; }
    if (depth !== 0) return false;
    return !/[+\-*/%&|^<>=]\s*$/.test(t);
  };

  G.validate = (s) => {
    const issues = [];
    const add = (level, target, where, msg) => issues.push({ level, target, where, msg });

    // ---- names --------------------------------------------------------------------------------
    if (s.schema.namespace && !s.schema.namespace.split('::').every(G.isIdent))
      add('error', { kind: 'schema' }, 'namespace', `"${s.schema.namespace}" must be identifiers separated by ::`);

    const typeNames = new Map();
    const claim = (name, target, what) => {
      if (!G.isIdent(name)) { add('error', target, name || '(unnamed)', `${what} name is not a valid identifier`); return; }
      if (BUILTIN_NAMES.has(name)) add('error', target, name, `${what} name shadows a built-in type`);
      if (typeNames.has(name)) add('error', target, name, `${what} "${name}" conflicts with another declaration`);
      typeNames.set(name, target);
    };
    s.enums.forEach((e) => {
      const t = { kind: 'enum', uid: e.uid };
      claim(e.name, t, 'Enum');
      if (!e.values.length) add('error', t, e.name, 'enum has no values');
      const seen = new Set();
      e.values.forEach((v) => {
        if (!G.isIdent(v.name)) add('error', t, e.name, `enum value "${v.name}" is not a valid identifier`);
        if (seen.has(v.name)) add('error', t, e.name, `duplicate enum value "${v.name}"`);
        seen.add(v.name);
        if (!G.isInt(v.value)) add('error', t, `${e.name}.${v.name}`, 'enum value must be an integer');
      });
    });
    s.customTypes.forEach((c) => {
      const t = { kind: 'custom', uid: c.uid };
      claim(c.name, t, 'Custom type');
      const cb = (c.lang && c.lang.c) || {};
      const jb = (c.lang && c.lang.js) || {};
      // a binding for at least one language (cpp:, c: or js:); a started binding must be complete
      if (!c.type && !cb.type && !jb.decode) add('error', t, c.name, 'custom type needs a C++, C or JavaScript binding');
      if (jb.decode && !jb.module) add('error', t, c.name, 'custom type needs a JavaScript module');
      if (c.type && !c.decode) add('error', t, c.name, 'custom type needs a C++ decode function');
      if (cb.type && !cb.decode) add('error', t, c.name, 'custom type needs a C decode function');
      if (!c.type && c.decode) add('error', t, c.name, 'custom type needs a C++ type');
      if (!cb.type && cb.decode) add('error', t, c.name, 'custom type needs a C type');
    });
    s.customChecksums.forEach((c) => {
      const t = { kind: 'custom', uid: c.uid };
      if (!G.isIdent(c.name)) add('error', t, c.name || '(unnamed)', 'checksum name is not a valid identifier');
      if (!c.function && !((c.lang && c.lang.c) || {}).function && !((c.lang && c.lang.js) || {}).function)
        add('error', t, c.name, 'custom checksum needs a C++, C or JavaScript function');
      if (!G.isInt(c.size)) add('error', t, c.name, 'custom checksum needs a size in bytes');
    });
    s.customFramers.forEach((c) => {
      const t = { kind: 'custom', uid: c.uid };
      if (!G.isIdent(c.name)) add('error', t, c.name || '(unnamed)', 'framer name is not a valid identifier');
      if (!c.type && !((c.lang && c.lang.c) || {}).scan && !((c.lang && c.lang.js) || {}).scan)
        add('error', t, c.name, 'custom framer needs a C++ type or a C or JavaScript scan function');
    });
    s.messages.forEach((m) => claim(m.name, { kind: 'message', uid: m.uid }, 'Message'));
    s.protocols.forEach((p) => claim(p.name, { kind: 'protocol', uid: p.uid }, 'Protocol'));

    // ---- messages -----------------------------------------------------------------------------
    s.messages.forEach((m) => { checkFields(s, m, { kind: 'message', uid: m.uid }, add); checkComputed(s, m, { kind: 'message', uid: m.uid }, add); });

    // ---- protocols ----------------------------------------------------------------------------
    s.protocols.forEach((p) => checkProtocol(s, p, add));
    return issues;
  };

  /** Value limits of a field / bit field / element `o` with value type `prim` (`width` raw bits); mirrors
   *  resolve_limits() in generator/src/ir/lowering.cpp. */
  function checkLimits(o, prim, width, scaled, what, add, ft, where) {
    const integer = G.INT_PRIMS.includes(prim);
    const range = integer ? G.intRange(prim[0] === 'i', width) : null;
    const lim = {};
    for (const k of G.LIMIT_KEYS) {
      const txt = String(o[k] ?? '').trim();
      if (!txt) continue;
      const l = G.parseLimit(txt);
      if (!l) { add('error', ft, where, `${k} of ${what}: expected a number, got '${txt}'`); continue; }
      lim[k] = l;
      const raw = k.endsWith('_raw');
      if (!integer || (!raw && scaled)) continue;
      if (!l.integer) {
        if (raw) add('error', ft, where, `${k} of ${what} must be an integer (the raw value is ${prim})`);
      } else if (l.big < range[0] || l.big > range[1]) {
        add('error', ft, where, `${k} ${txt} of ${what} is outside the range of the raw value (${range[0]}..${range[1]})`);
      }
    }
    const less = (a, b) => (a.integer && b.integer ? a.big < b.big : a.value < b.value);
    for (const [lo, hi] of [['min_value', 'max_value'], ['min_raw', 'max_raw']])
      if (lim[lo] && lim[hi] && less(lim[hi], lim[lo])) add('error', ft, where, `${lo} ${lim[lo].text} of ${what} is greater than ${hi} ${lim[hi].text}`);
  }

  /** 'max' only sets the capacity of variable-length bytes / string / array data (mirrors check_max()). */
  function checkMax(f, kind, text, ft, where, add) {
    if (!String(f.max || '').trim()) return;
    const container = ['bytes', 'string', 'array'].includes(kind);
    if (container && (text || f.lenMode !== 'fixed')) return;
    if (container) {
      if (G.parseInt(f.max) !== G.parseInt(f.lenValue))
        add('error', ft, where, `fixed ${kind === 'array' ? 'count' : 'size'} ${f.lenValue}: 'max: ${f.max}' does not match it ('max' is the capacity of variable-length fields; clear it)`);
      return;
    }
    add('error', ft, where, "'max' is not a value limit: it only sets the capacity of variable-length bytes, string and array fields." +
      (['int', 'float'].includes(kind) ? ' Use min_value / max_value to limit the value (or min_raw / max_raw for the raw wire value)' : ' Clear it'));
  }

  function checkFields(s, m, target, add) {
    const text = !m.isHeader && G.msgFormat(s, m) === 'text';
    const wantStruct = m.isHeader || G.wantsStruct(s, m);
    const wantView = m.isHeader || G.wantsView(s, m);
    const names = new Set();
    m.fields.forEach((f, i) => {
      const ft = { ...target, fieldUid: f.uid };
      const where = `${m.name}.${f.name || '#' + (i + 1)}`;
      const kind = G.fieldKind(s, f);
      if (kind !== 'pad') {
        if (!G.isIdent(f.name)) add('error', ft, where, 'field name is not a valid identifier');
        else if (names.has(f.name)) add('error', ft, where, `duplicate field "${f.name}"`);
        names.add(f.name);
      }
      if (kind === 'unknown') { add('error', ft, where, `unknown type "${f.type}"`); return; }
      if (kind === 'message' && f.type === m.name) add('error', ft, where, 'a message cannot contain itself');

      if (text) {
        if (['bits', 'bytes', 'array', 'message'].includes(kind)) add('error', ft, where, `type "${f.type}" is not supported in text messages`);
        if (f.when) add('error', ft, where, "'when' is not supported in text messages (use optional)");
        if (kind === 'string' && wantStruct && !G.isInt(f.max)) add('error', ft, where, "text string needs 'max' (struct capacity)");
      } else if (f.optional) {
        add('warning', ft, where, "'optional' only applies to text messages (ignored)");
      }

      if (kind === 'bits') {
        const total = G.bitsTotal(f);
        if (!f.bits.length) add('error', ft, where, 'bits group has no bit fields');
        else if (total % 8) add('error', ft, where, `bits group spans ${total} bits; add { pad: ${8 - (total % 8)} }`);
        const bn = new Set();
        f.bits.forEach((b) => {
          const w = G.parseInt(b.bits);
          if (Number.isNaN(w) || w < 1 || w > 64) add('error', ft, where, 'bit width must be 1..64');
          if (b.pad) return;
          if (!G.isIdent(b.name)) add('error', ft, where, `bit field "${b.name}" is not a valid identifier`);
          if (bn.has(b.name)) add('error', ft, where, `duplicate bit field "${b.name}"`);
          bn.add(b.name);
          if (b.type && G.PRIM_SIZE[b.type] && b.type !== 'bool' && G.PRIM_SIZE[b.type] * 8 < w)
            add('error', ft, where, `${b.name}: ${b.type} is too small for ${w} bits`);
          if (b.type && !G.PRIM_SIZE[b.type] && !G.findEnum(s, b.type)) add('error', ft, where, `${b.name}: unknown type "${b.type}"`);
          if (G.hasLimits(b)) {
            const t = b.type || (w === 1 ? 'bool' : 'u64');
            if (String(b.const || '').trim() || G.findEnum(s, t) || t === 'bool')
              add('error', ft, where, `bit field "${b.name}": value limits need a numeric bit field (not ${String(b.const || '').trim() ? 'const' : G.findEnum(s, t) ? 'an enum: use strict' : 'a bool'})`);
            else if (w >= 1 && w <= 64) checkLimits(b, t, w, !!(b.scale || b.offset), `bit field "${b.name}"`, add, ft, where);
          }
        });
      }

      const variable = f.lenMode === 'expr' || f.lenMode === 'remaining' || f.lenMode === 'terminator';
      if (!text && (kind === 'bytes' || kind === 'string' || kind === 'array')) {
        if (f.lenMode === 'fixed' && !G.isInt(f.lenValue)) add('error', ft, where, `${kind === 'array' ? 'count' : 'length'} must be a number`);
        if (f.lenMode === 'expr') checkExpr(s, m, i, f.lenValue, ft, where, add);
        if (kind === 'array' && f.lenMode === 'terminator') add('error', ft, where, 'arrays cannot use a terminator');
        if (variable && wantStruct && !G.isInt(f.max)) add('error', ft, where, "variable length needs 'max' for the struct (or access: view)");
        if (f.lenMode === 'remaining' && i !== m.fields.length - 1) add('error', ft, where, "'remaining' is only allowed on the last field");
      }
      if (kind === 'array') {
        const ek = G.fieldKind(s, { type: f.of });
        if (ek === 'unknown') add('error', ft, where, `unknown element type "${f.of}"`);
        if (['bits', 'bytes', 'string', 'array', 'pad'].includes(ek)) add('error', ft, where, 'array elements must be scalar, enum, message or custom');
        if (ek === 'custom' && wantView) add('error', ft, where, 'arrays of custom types require access: struct');
      }
      if (f.const && !G.isInt(f.const)) add('error', ft, where, 'const must be an integer literal');
      if (f.const && (kind === 'float' || !['int', 'enum', 'bool', 'char'].includes(kind))) add('error', ft, where, "'const' is only valid on integer, enum, bool or char fields");
      if ((f.scale || f.offset) && !['int', 'float'].includes(kind)) add('error', ft, where, "'scale'/'offset' need a numeric field");
      checkMax(f, kind, text, ft, where, add);
      if (G.hasLimits(f)) {
        if (!['int', 'float'].includes(kind)) add('error', ft, where, `value limits need a numeric scalar field${kind === 'enum' ? ' (for enums use strict)' : ''}`);
        else if (String(f.const || '').trim()) add('error', ft, where, "'const' field: value limits do not apply");
        else checkLimits(f, f.type, G.PRIM_SIZE[f.type] * 8, !!(f.scale || f.offset), `"${f.name}"`, add, ft, where);
      }
      if (kind === 'array' && G.hasLimits(f.ofLimits)) {
        const ek = G.fieldKind(s, { type: f.of });
        if (!['int', 'float'].includes(ek)) add('error', ft, where, 'element value limits need a numeric element type');
        else checkLimits(f.ofLimits, f.of, G.PRIM_SIZE[f.of] * 8, false, 'the array element', add, ft, where);
      }
      if (f.crc) {
        if (text) add('error', ft, where, "'crc' is not supported in text messages");
        else if (!['int', 'float', 'enum', 'bool', 'char'].includes(kind)) add('error', ft, where, "'crc' is only valid on scalar and enum fields");
        else if (!G.checksumSize(s, f.crc, f.crcParams)) add('error', ft, where, `unknown CRC "${f.crc}"`);
        if (f.crc === '__crc') {
          const w = G.parseInt(f.crcParams.width);
          if (!(w >= 1 && w <= 64)) add('error', ft, where, 'crc width must be 1..64');
          if (!G.isInt(f.crcParams.poly)) add('error', ft, where, 'crc poly must be an integer');
        }
      }
      const convs = f.conversions || [];
      if (convs.length && !['int', 'float'].includes(kind)) add('error', ft, where, "'conversions' are only valid on numeric scalar fields");
      const seenConv = new Set();
      convs.forEach((c) => {
        if (!G.isIdent(c.name || '')) add('error', ft, where, `conversion name "${c.name || ''}" is not a valid identifier`);
        else if (c.name === 'scaled' || c.name === 'value') add('error', ft, where, `conversion name "${c.name}" is reserved`);
        else if (m.fields.some((o) => o.name === `${f.name}_${c.name}`)) add('error', ft, where, `conversion accessor "${f.name}_${c.name}" clashes with a field`);
        if (seenConv.has(c.name)) add('error', ft, where, `duplicate conversion "${c.name}"`);
        seenConv.add(c.name);
        if (!c.scale && !c.offset) add('error', ft, where, `conversion "${c.name}" needs a scale and/or offset`);
      });
      if (f.when && !text) checkExpr(s, m, i, f.when, ft, where, add);

      if (kind === 'message') {
        const child = G.findMessage(s, f.type);
        if (child && !m.isHeader) {
          if (G.msgFormat(s, child) !== G.msgFormat(s, m)) add('error', ft, where, `nested message "${child.name}" has a different format`);
          if (wantView && !G.wantsView(s, child)) add('error', ft, where, `nested "${child.name}" needs view access`);
          if (wantStruct && !G.wantsStruct(s, child)) add('error', ft, where, `nested "${child.name}" needs struct access`);
        }
      }
    });
  }

  /** Mirrors resolve_computed() in generator/src/ir/lowering.cpp and the validator's name checks. */
  function checkComputed(s, m, target, add) {
    const list = m.computed || [];
    if (!list.length) return;
    if (G.msgFormat(s, m) === 'text') { add('error', target, m.name, 'computed values are only supported in binary messages'); return; }
    const accessors = new Set();
    m.fields.forEach((f) => {
      if (f.name) accessors.add(f.name);
      if (f.scale || f.offset) accessors.add(`${f.name}_scaled`);
      (f.conversions || []).forEach((c) => c.name && accessors.add(`${f.name}_${c.name}`));
    });
    const seen = new Set();
    list.forEach((c) => {
      const where = `${m.name}.${c.name || '(computed)'}`;
      if (!G.isIdent(c.name || '')) add('error', target, where, 'computed value name is not a valid identifier');
      else if (accessors.has(c.name)) add('error', target, where, `computed value "${c.name}" clashes with a field or accessor`);
      if (seen.has(c.name)) add('error', target, where, `duplicate computed value "${c.name}"`);
      seen.add(c.name);
      if (!['u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64', 'f32', 'f64'].includes(c.type)) add('error', target, where, 'type must be numeric');
      if (!(c.value || '').trim()) add('error', target, where, 'needs a value expression');
      else checkExpr(s, m, m.fields.length, c.value, target, where, add);
      const sets = G.parseSetList(c.set);
      const parts = String(c.set || '').split(/[;\n]/).filter((x) => x.trim());
      if (parts.length !== sets.length) add('error', target, where, 'setter must be "field = expression; …"');
      const targets = new Set();
      sets.forEach(([fname, e]) => {
        const f = m.fields.find((x) => x.name === fname);
        if (!f) { add('error', target, where, `set target "${fname}" is not a field`); return; }
        if (!['int', 'enum', 'float'].includes(G.fieldKind(s, f)) || f.const || f.when) add('error', target, where, `set target "${fname}" must be a plain scalar or enum field`);
        if (targets.has(fname)) add('error', target, where, `field "${fname}" is set twice`);
        targets.add(fname);
        if (!G.exprSyntaxOk(e)) add('error', target, where, `invalid expression "${e}"`);
      });
    });
  }

  function checkExpr(s, m, index, text, ft, where, add) {
    if (!G.exprSyntaxOk(text)) { add('error', ft, where, `invalid expression "${text || ''}"`); return; }
    const earlier = m.fields.slice(0, index);
    for (const r of G.exprRefs(text)) {
      const [head, member] = r.split('.');
      const f = earlier.find((x) => x.name === head);
      if (!f) {
        const e = G.findEnum(s, head);
        if (e && member) {
          if (!e.values.some((v) => v.name === member)) add('error', ft, where, `enum ${head} has no value "${member}"`);
          continue;
        }
        add('error', ft, where, `"${r}" is not an earlier field or enum constant`);
        continue;
      }
      if (f.when) add('error', ft, where, `cannot reference conditional field "${head}"`);
      if (f.type === 'bits') {
        if (!member || !f.bits.some((b) => !b.pad && b.name === member)) add('error', ft, where, `use a member of bits group "${head}" (e.g. ${head}.x)`);
      } else if (member) {
        add('error', ft, where, `"${r}": only bits groups have members`);
      } else if (!['int', 'enum', 'bool', 'char'].includes(G.fieldKind(s, f))) {
        add('error', ft, where, `"${head}" cannot be used in an expression`);
      }
    }
  }

  function checkProtocol(s, p, add) {
    const t = { kind: 'protocol', uid: p.uid };
    const fmt = G.protoFormat(s, p);
    const hdr = G.headerMessage(s, p);
    const fr = p.framing;
    checkFields(s, hdr, t, add);
    if (fmt === 'text' && p.header.length) add('error', t, p.name, 'text protocols do not support a binary header');
    p.header.forEach((f) => {
      const sz = G.fieldSize(s, f);
      if (f.when) add('error', { ...t, fieldUid: f.uid }, `${p.name}.${f.name}`, 'header fields cannot be conditional');
      else if (sz.fixed == null) add('error', { ...t, fieldUid: f.uid }, `${p.name}.${f.name}`, 'header fields must have a fixed size');
    });

    if (!p.messages.length) add('error', t, p.name, 'protocol has no messages');
    const msgs = p.messages.map((n) => G.findMessage(s, n));
    p.messages.forEach((n, i) => {
      const m = msgs[i];
      if (!m) { add('error', t, p.name, `unknown message "${n}"`); return; }
      if (G.msgFormat(s, m) !== fmt) add('error', t, p.name, `message "${n}" format does not match the protocol`);
      if (!G.wantsView(s, m)) add('error', t, p.name, `message "${n}" needs view access to be identified by message_type`);
      if (fmt === 'text' && (p.separator || s.schema.defaults.separator) !== G.msgSeparator(s, m))
        add('error', t, p.name, `message "${n}" separator differs from the protocol`);
    });

    // message_type
    const d = p.message_type;
    if (d.kind === 'none' && p.messages.length > 1) add('error', t, p.name, 'several messages need a message_type rule');
    if (d.kind === 'field') {
      if (fmt !== 'binary') add('error', t, p.name, "text protocols identify messages by 'token'");
      const f = p.header.find((x) => x.name === d.field);
      if (!f) add('error', t, p.name, `message_type field "${d.field || ''}" is not a header field`);
      else if (!['int', 'enum'].includes(G.fieldKind(s, f))) add('error', t, p.name, 'message_type field must be an integer or enum');
      const seen = new Set();
      msgs.forEach((m) => {
        if (!m) return;
        if (!m.type_id) { add('error', { kind: 'message', uid: m.uid }, m.name, `needs a type_id to be identified in ${p.name}`); return; }
        if (!G.isInt(m.type_id) && f && G.fieldKind(s, f) === 'enum') {
          const e = G.findEnum(s, f.type);
          if (!e.values.some((v) => v.name === m.type_id)) add('error', { kind: 'message', uid: m.uid }, m.name, `type_id "${m.type_id}" is not a value of ${e.name}`);
        } else if (!G.isInt(m.type_id)) add('error', { kind: 'message', uid: m.uid }, m.name, `type_id "${m.type_id}" must be an integer`);
        if (seen.has(m.type_id)) add('error', t, p.name, `duplicate type_id ${m.type_id}`);
        seen.add(m.type_id);
      });
    }
    if (d.kind === 'token') {
      if (fmt !== 'text') add('error', t, p.name, "message_type by 'token' requires a text protocol");
      const seen = new Set();
      msgs.forEach((m) => {
        if (!m) return;
        if (!m.type_id) add('error', { kind: 'message', uid: m.uid }, m.name, `needs a type_id to be identified in ${p.name}`);
        if (seen.has(m.type_id)) add('error', t, p.name, `duplicate type_id "${m.type_id}"`);
        seen.add(m.type_id);
      });
    }

    // framing
    const sync = G.patternBytes(fr.syncMode, fr.sync);
    const end = G.patternBytes(fr.endMode, fr.end);
    if (sync === null) add('error', t, p.name, 'sync/start bytes must be hex like "AA 55"');
    if (end === null) add('error', t, p.name, 'end bytes must be hex like "0D 0A"');
    if ((sync || []).length > 8 || (end || []).length > 8) add('error', t, p.name, 'patterns are limited to 8 bytes');
    if (fr.type === 'none' && ((sync || []).length || (end || []).length)) add('error', t, p.name, "framing 'none' takes no sync/end bytes");
    if (fr.type === 'delimited' && !(end || []).length) add('error', t, p.name, "delimited framing needs 'end'");
    if (fr.type === 'length_prefixed') {
      const f = p.header.find((x) => x.name === fr.lengthField);
      if (!f) add('error', t, p.name, 'length_prefixed framing needs a header length field');
      else if (!['u8', 'u16', 'u32', 'u64'].includes(f.type) || f.const) add('error', t, p.name, 'length field must be an unsigned integer');
      if (fr.counts === 'wire' && !fr.escape.enabled) add('error', t, p.name, 'a length counting wire bytes needs byte stuffing');
      else if (fr.counts === 'wire' && fr.covers !== 'body' && fr.covers !== 'body_checksum')
        add('error', t, p.name, 'a length counting wire bytes must cover the body (or body + checksum) only (a length that counts its own escaped bytes cannot always be encoded)');
      else if (fr.counts === 'wire' && fr.covers === 'body_checksum' && p.checksum.enabled && p.checksum.covers !== 'body')
        add('error', t, p.name, 'a length counting the checksum on the wire needs the checksum to cover the body only (a checksum over the length field changes with the length)');
    }
    if (fr.type === 'custom' && !s.customFramers.some((c) => c.name === fr.framer)) add('error', t, p.name, 'select a custom framer');
    if (fr.escape.enabled) {
      if (!['delimited', 'fixed', 'length_prefixed'].includes(fr.type)) add('error', t, p.name, 'escaping needs delimited, fixed or length_prefixed framing');
      else if (fmt === 'binary' || fr.type === 'delimited') {
        const et = G.escapeTable(s, p);
        et.errors.forEach((m) => add('error', t, p.name, `escape: ${m}`));
        if (!et.errors.length) {
          if (fr.type === 'delimited' && end && end.length && !et.isReserved(end[0])) add('error', t, p.name, `escape: the end delimiter byte ${G.hex2(end[0])} must be reserved`);
          if (fr.type !== 'delimited' && sync && sync.length && !et.isReserved(sync[0]))
            add('warning', t, p.name, `escape: sync byte ${G.hex2(sync[0])} is not reserved, so it can appear inside frames and resynchronisation is slower`);
        }
      }
    }
    if (p.checksum.enabled && p.checksum.over === 'wire' && !fr.escape.enabled) add('error', t, p.name, 'checksum over wire bytes needs byte stuffing');
    if (fmt === 'text' && !['delimited', 'none', 'custom'].includes(fr.type)) add('error', t, p.name, 'text protocols need delimited framing');
    const L = G.protocolLayout(s, p);
    if (fr.type === 'fixed' && L.maxFrame == null) add('error', t, p.name, "fixed framing needs 'size' unless all messages share one fixed size");
    if (fr.type === 'fixed' && p.exact_size) add('error', t, p.name, 'exact_size does not apply to fixed framing (shorter messages are zero padded to the frame size)');
    if (fr.type !== 'none' && L.maxFrame == null) add('error', t, p.name, "set max_frame (message sizes are not bounded)");
    if (p.checksum.enabled && p.checksum.algo === '__crc') {
      const w = G.parseInt(p.checksum.crc.width);
      if (Number.isNaN(w) || w < 1 || w > 64) add('error', t, p.name, 'CRC width must be 1..64');
      if (!G.isInt(p.checksum.crc.poly)) add('error', t, p.name, 'CRC poly must be an integer');
    }
    if (fr.type === 'fixed' && sync && sync.length && !p.checksum.enabled)
      add('warning', t, p.name, 'fixed framing with a sync word but no checksum cannot detect false sync matches');
  }
})(window.GMP);
