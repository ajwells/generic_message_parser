// viz.js - vertical, size-proportional rendering of messages and protocol frames.
(function (G) {
  'use strict';
  const esc = G.esc;

  const PX_PER_BYTE = 9;
  const MIN_H = 32;
  const MAX_H = 190;
  const VAR_H = 58;

  const heightFor = (sz) => {
    if (sz.fixed != null) return Math.max(MIN_H, Math.min(MAX_H, 20 + sz.fixed * PX_PER_BYTE));
    if (sz.present != null) return Math.max(MIN_H, Math.min(MAX_H, 20 + sz.present * PX_PER_BYTE));
    return VAR_H;
  };
  const hex = (n, w = 2) => '0x' + n.toString(16).toUpperCase().padStart(w, '0');
  const offCell = (off) => (off == null ? '<div class="off">var</div>' : `<div class="off"><b>${hex(off)}</b>${off}</div>`);
  const colorVars = (kind) => `--f:var(--k-${kind});--b:var(--k-${kind}-b)`;

  function gutter(g) {
    if (!g) return '<div class="gutter"></div>';
    return `<div class="gutter"><i class="${g.crc ? 'crc' : ''}" title="${g.crc ? 'covered by checksum' : ''}"></i><i class="${g.len ? 'len' : ''}" title="${g.len ? 'counted by length field' : ''}"></i></div>`;
  }

  // ----------------------------------------------------------------------------------------------
  // field blocks
  // ----------------------------------------------------------------------------------------------
  function bitsBar(s, f) {
    const total = G.bitsTotal(f) || 1;
    return `<div class="bitbar">${f.bits.map((b) => {
      const w = G.parseInt(b.bits) || 1;
      const label = b.pad ? 'pad' : esc(b.name || '?');
      const ty = b.pad ? '' : b.type ? ` ${esc(b.type)}` : w === 1 ? ' bool' : '';
      return `<div class="bit${b.pad ? ' pad' : ''}${b.const ? ' const' : ''}" style="--w:${w}" title="${label}: ${w} bit${w > 1 ? 's' : ''}${b.const ? ' = ' + esc(b.const) : ''}">
        <span class="bn">${label}${b.const ? ' =' + esc(b.const) : ''}</span><span class="bw">${w}b${ty}</span></div>`;
    }).join('')}</div>${total % 8 ? `<div class="ty" style="color:var(--danger)">${total} bits - not a whole byte</div>` : ''}`;
  }

  function fieldBlock(s, ui, row, owner, ctx, depth) {
    const f = row.field;
    const sz = row.size;
    const kind = sz.kind;
    const color = G.KIND_COLOR[kind];
    const classes = ['blk'];
    if (sz.fixed == null && !(f.when && sz.present != null)) classes.push('var');
    if (f.when) classes.push('cond');
    if (kind === 'pad') classes.push('pad');
    if (ui.field === f.uid) classes.push('sel');
    let style = colorVars(color);
    let inner = '';
    const name = kind === 'pad' ? 'padding' : esc(f.name || '(unnamed)');
    const tags = [];
    if (f.const) tags.push(`= ${esc(f.const)}`);
    if (f.scale || f.offset) tags.push(`×${esc(f.scale || 1)}${f.offset ? (String(f.offset).startsWith('-') ? '' : '+') + esc(f.offset) : ''}`);
    if (f.unit) tags.push(esc(f.unit));
    if (f.when) tags.push(`when ${esc(f.when)}`);
    if (f.optional && ctx.text) tags.push('optional');

    let height = ctx.text ? 40 : heightFor(sz);
    if (kind === 'bits') {
      inner = bitsBar(s, f);
      height = Math.max(height, 64);
    } else if (kind === 'array') {
      const elem = sz.elem || {};
      const cell = Math.max(10, Math.min(40, (elem.fixed || 2) * 7));
      style += `;--cell:${cell}px`;
      classes.push('arr');
      const cnt = f.lenMode === 'fixed' ? f.lenValue : f.lenMode === 'remaining' ? 'rest' : f.lenValue || '?';
      tags.unshift(`× ${esc(cnt)}${f.max ? ` (≤ ${esc(f.max)})` : ''}`);
    } else if (kind === 'message' && depth < 3) {
      const child = G.findMessage(s, f.type);
      if (child && !(ctx.stack || []).includes(child.uid)) {
        const L = G.messageLayout(s, child);
        inner = `<div class="nested">${rowsHtml(s, ui, child, L, { ...ctx, stack: [...(ctx.stack || []), owner.uid], nested: true, gut: null }, depth + 1)}</div>`;
        height = 0;
      }
    }
    const sizeText = ctx.text ? (row.tokens > 1 ? `${row.tokens} tokens` : '') : G.sizeLabel(sz);
    const select = `data-action="selectField" data-kind="${ctx.kind}" data-uid="${ctx.uid}" data-field="${f.uid}"`;
    return `<div class="${classes.join(' ')}" style="${style};${height ? `min-height:${height}px` : ''}" ${select} title="${esc((f.doc || '') + (f.doc ? ' — ' : '') + G.typeLabel(s, f))}">
      <span class="size">${sizeText}</span>
      <div class="nm">${name}${tags.map((t) => `<span class="tag">${t}</span>`).join('')}</div>
      <div class="ty">${esc(G.typeLabel(s, f))}</div>
      ${inner}
    </div>`;
  }

  function rowsHtml(s, ui, owner, L, ctx, depth = 0) {
    const base = ctx.baseOffset || 0;
    const rows = L.rows.map((r) => {
      const off = ctx.text ? null : r.offset == null ? null : base + r.offset;
      const offHtml = ctx.text ? `<div class="off"><b>#${r.token}</b>${r.tokens > 1 ? '…' + (r.token + r.tokens - 1) : ''}</div>` : ctx.nested ? `<div class="off">${r.offset == null ? 'var' : '+' + r.offset}</div>` : offCell(off);
      return `<div class="row">${offHtml}${fieldBlock(s, ui, r, owner, ctx, depth)}${ctx.nested ? '' : gutter(ctx.gut)}</div>`;
    });
    if (!rows.length) return `<div class="callout">No fields yet.</div>`;
    return `<div class="rows">${rows.join('')}</div>`;
  }

  function frameRow(kind, label, detail, size, off, gut, extraStyle = '', inner = '') {
    const h = size != null ? Math.max(MIN_H, Math.min(MAX_H, 20 + size * PX_PER_BYTE)) : VAR_H;
    return `<div class="row">${off === undefined ? '<div class="off"></div>' : offCell(off)}
      <div class="blk${size == null ? ' var' : ''}" style="${colorVars(kind)};min-height:${h}px;${extraStyle}">
        <span class="size">${size != null ? size + ' B' : ''}</span>
        <div class="nm">${label}</div>${detail ? `<div class="ty">${detail}</div>` : ''}${inner}
      </div>${gutter(gut)}</div>`;
  }

  // ----------------------------------------------------------------------------------------------
  // cards
  // ----------------------------------------------------------------------------------------------
  function errorBadge(issues, kind, uid) {
    const n = issues.filter((i) => i.level === 'error' && i.target.kind === kind && i.target.uid === uid).length;
    return n ? `<span class="badge err">${n} issue${n > 1 ? 's' : ''}</span>` : '';
  }

  function messageCard(s, ui, m, issues) {
    const L = G.messageLayout(s, m);
    const fmt = G.msgFormat(s, m);
    const sel = ui.sel.kind === 'message' && ui.sel.uid === m.uid;
    const badges = [
      `<span class="badge">${fmt}</span>`,
      m.id ? `<span class="badge accent">id ${esc(m.id)}</span>` : '',
      L.text ? `<span class="badge">${L.tokens} tokens</span>` : `<span class="badge">${L.fixed ? L.size + ' B fixed' : G.sizeLabel({ fixed: null, min: L.min, max: L.max })}</span>`,
      G.msgAccess(s, m) !== 'both' ? `<span class="badge">${G.msgAccess(s, m)} only</span>` : '',
      errorBadge(issues, 'message', m.uid),
    ].join('');
    const ctx = { kind: 'message', uid: m.uid, text: L.text, gut: null, stack: [] };
    const sub = L.text ? `Tokens separated by “${esc(G.msgSeparator(s, m))}”` : `${G.msgEndian(s, m)} endian${m.doc ? ' · ' + esc(m.doc) : ''}`;
    return `<article class="card${sel ? ' selected' : ''}" data-action="select" data-kind="message" data-uid="${m.uid}" id="card-${m.uid}">
      <div class="card-head"><div class="card-title"><span class="kind">msg</span>${esc(m.name)}</div><div class="badges">${badges}</div></div>
      <div class="card-body"><div class="card-sub">${sub}</div>${rowsHtml(s, ui, m, L, ctx)}</div>
    </article>`;
  }

  function protocolCard(s, ui, p, issues) {
    const L = G.protocolLayout(s, p);
    const sel = ui.sel.kind === 'protocol' && ui.sel.uid === p.uid;
    const fr = p.framing, ck = p.checksum;
    const ctx = { kind: 'protocol', uid: p.uid, text: false, stack: [] };
    const covers = ck.enabled ? (L.text ? 'body' : ck.covers || 'header_body') : null;
    const lenCovers = fr.type === 'length_prefixed' ? fr.covers || 'body' : null;
    const g = (part) => ({
      crc: covers && (part === 'body' || (part === 'header' && covers !== 'body') || (part === 'prefix' && covers === 'all')),
      len: lenCovers && (part === 'body' || (part === 'header' && lenCovers !== 'body') || (lenCovers === 'all' && (part === 'prefix' || part === 'check'))),
    });
    const framingLabel = { none: 'no framing', fixed: 'fixed size', length_prefixed: 'length prefixed', delimited: 'delimited', custom: 'custom framer' }[fr.type];
    const badges = [
      `<span class="badge">${L.text ? 'text' : 'binary'}</span>`,
      `<span class="badge accent">${framingLabel}</span>`,
      L.maxFrame != null ? `<span class="badge">max ${L.maxFrame} B${L.maxFrameDerived ? ' (auto)' : ''}</span>` : fr.type !== 'none' ? '<span class="badge err">max_frame?</span>' : '',
      errorBadge(issues, 'protocol', p.uid),
    ].join('');

    const rows = [];
    let off = 0;
    const patText = (bytes, mode) => (mode === 'text' ? `“${esc(G.bytesToEscaped(bytes))}”` : bytes.map((b) => G.hex2(b)).join(' '));
    if (L.prefix.length) {
      rows.push(frameRow('frame', fr.type === 'delimited' ? 'start delimiter' : 'sync', patText(L.prefix, fr.syncMode), L.prefix.length, off, g('prefix')));
      off += L.prefix.length;
    }
    if (L.text) {
      rows.push(frameRow('message', 'message id', p.dispatch.kind === 'token' ? 'first token selects the message' : 'single message', null, undefined, g('body'),
        '', `<div class="body-alts">${L.bodies.map((b) => `<span class="alt" data-action="select" data-kind="message" data-uid="${b.message.uid}">${esc(b.message.id || b.message.name)}<small>${esc(b.message.name)}</small></span>`).join('')}</div>`));
      rows.push(frameRow('string', 'fields…', `tokens separated by “${esc(p.separator || s.schema.defaults.separator)}”`, null, undefined, g('body')));
      if (ck.enabled) rows.push(frameRow('check', 'checksum', `“${esc(ck.separator || '*')}” + ${L.checksumSize * 2} hex digits · ${esc(ck.algo === '__crc' ? 'custom CRC' : ck.algo)}`, null, undefined, null));
    } else {
      if (p.header.length) {
        const hm = G.headerMessage(s, p);
        const hrows = rowsHtml(s, ui, hm, L.header, { ...ctx, baseOffset: off, gut: g('header') });
        rows.push(hrows.replace(/^<div class="rows">|<\/div>$/g, ''));
        off += L.headerSize;
      }
      const alts = L.bodies.map((b) => {
        const sz = b.layout.size != null ? `${b.layout.size} B` : G.sizeLabel({ fixed: null, min: b.layout.min, max: b.layout.max });
        return `<span class="alt" data-action="select" data-kind="message" data-uid="${b.message.uid}">${esc(b.message.name)}<small>${b.message.id ? 'id ' + esc(b.message.id) + ' · ' : ''}${sz}</small></span>`;
      }).join('');
      const fixedBody = L.bodies.length === 1 && L.bodies[0].layout.size != null ? L.bodies[0].layout.size : null;
      const dispatchText = p.dispatch.kind === 'field' ? `selected by header.${esc(p.dispatch.field || '?')}` : L.bodies.length > 1 ? 'needs dispatch' : 'single message';
      const bodyRange = L.bodies.length ? `${L.bodyMin}${L.bodyMax == null ? '+' : L.bodyMax !== L.bodyMin ? '–' + L.bodyMax : ''} B · ` : '';
      rows.push(frameRow('message', 'body', bodyRange + dispatchText, fixedBody, off, g('body'), '', `<div class="body-alts">${alts || '<span class="ty">no messages selected</span>'}</div>`));
      if (fixedBody == null) off = null;
      else off += fixedBody;
      if (ck.enabled) {
        const label = ck.algo === '__crc' ? `CRC-${esc(ck.crc.width)} poly ${esc(ck.crc.poly)}` : esc(ck.algo);
        rows.push(frameRow('check', 'checksum', `${label} · ${ck.endian || G.protoEndian(s, p)} endian`, L.checksumSize || null, off == null ? null : off, g('check')));
        if (off != null) off += L.checksumSize;
      }
    }
    if (L.suffix.length) rows.push(frameRow('frame', 'end delimiter', patText(L.suffix, fr.endMode), L.suffix.length, L.text ? undefined : off, null));
    if (fr.escape.enabled && fr.type === 'delimited') rows.push(`<div class="callout" style="margin-top:6px">Byte stuffing: ${fr.escape.style === 'xor' ? `${esc(fr.escape.char)} + byte ⊕ ${esc(fr.escape.xor)}` : `${esc(fr.escape.char)} + code (${esc(fr.escape.map)})`}. Escaped frames may be up to 2× longer.</div>`);

    const legend = (covers || lenCovers)
      ? `<div class="gutter-legend">${covers ? '<span><i style="background:var(--k-check-b);opacity:.55"></i>checksum coverage</span>' : ''}${lenCovers ? '<span><i style="background:var(--accent);opacity:.45"></i>length field coverage</span>' : ''}</div>` : '';
    return `<article class="card${sel ? ' selected' : ''}" data-action="select" data-kind="protocol" data-uid="${p.uid}" id="card-${p.uid}">
      <div class="card-head"><div class="card-title"><span class="kind">proto</span>${esc(p.name)}</div><div class="badges">${badges}</div></div>
      <div class="card-body"><div class="card-sub">Frame layout${L.overhead ? ` · ${L.overhead} B overhead` : ''}${p.doc ? ' · ' + esc(p.doc) : ''}</div><div class="rows">${rows.join('')}</div>${legend}</div>
    </article>`;
  }

  G.renderLayout = (s, ui, issues) => {
    if (!s.messages.length && !s.protocols.length) {
      return `<div class="viz-empty"><h3>Nothing to show yet</h3><p>Add a message on the left, or load the example to see how fields are laid out.</p>
        <button class="btn primary" data-action="loadExample">Load example</button></div>`;
    }
    // Stable order (declaration order, protocols then messages) so cards never jump when selected.
    const protos = s.protocols.map((p) => protocolCard(s, ui, p, issues)).join('');
    const msgs = s.messages.map((m) => messageCard(s, ui, m, issues)).join('');
    const sec = (title, html) => (html ? `<div class="viz-section-title">${title}</div>${html}` : '');
    return `<div class="viz-grid">${sec('Protocols', protos)}${sec('Messages', msgs)}</div>`;
  };

  G.renderLegend = () => [
    ['int', 'integer'], ['float', 'float'], ['enum', 'enum'], ['bits', 'bits'], ['string', 'string'], ['bytes', 'bytes'],
    ['array', 'array'], ['message', 'message'], ['custom', 'custom'], ['check', 'checksum'],
  ].map(([k, l]) => `<span><i style="background:var(--k-${k});border-color:var(--k-${k}-b)"></i>${l}</span>`).join('');
})(window.GMP);
