// decode-view.js - the "Decode tester" tab: paste raw bytes, see frames and fields highlighted.
(function (G) {
  'use strict';
  const esc = G.esc;
  const D = G.decoder;

  const KIND_CLASS = {
    int: 'int', float: 'float', bool: 'bool', char: 'bool', enum: 'enum', bits: 'bits', bytes: 'bytes', string: 'string',
    array: 'array', message: 'message', custom: 'custom', pad: 'pad', frame: 'frame', check: 'check', error: 'unknown',
  };

  /** Available decode targets: protocols first, then messages. */
  G.decodeTargets = (s) => [
    ...s.protocols.map((p) => ({ key: 'protocol:' + p.name, label: p.name, group: 'Protocols', text: G.protoFormat(s, p) === 'text' })),
    ...s.messages.map((m) => ({ key: 'message:' + m.name, label: m.name, group: 'Messages (no framing)', text: G.msgFormat(s, m) === 'text' })),
  ];

  G.renderDecodeShell = (s, dui) => {
    const targets = G.decodeTargets(s);
    if (!targets.length) {
      return `<div class="viz-empty"><h3>Nothing to decode against</h3><p>Define a message or protocol first, or load the example.</p>
        <button class="btn primary" data-action="loadExample">Load example</button></div>`;
    }
    return `<div class="decode">
      <div class="decode-controls">
        <label class="field-label"><span class="lbl">Decode as</span><select id="decode-target">${G.decodeTargetOptions(s, dui)}</select></label>
        <div class="field-label"><span class="lbl">Input</span>
          <div class="seg" id="decode-format">
            <button data-format="hex" class="${dui.format === 'hex' ? 'on' : ''}">Hex bytes</button>
            <button data-format="text" class="${dui.format === 'text' ? 'on' : ''}">Text</button>
          </div>
        </div>
        ${dui.format === 'text' ? `<label class="field-label" style="min-width:150px"><span class="lbl">Newlines</span><select id="decode-newline">
            <option value="crlf"${dui.newline === 'crlf' ? ' selected' : ''}>as \\r\\n</option>
            <option value="lf"${dui.newline === 'lf' ? ' selected' : ''}>as \\n</option>
            <option value="ignore"${dui.newline === 'ignore' ? ' selected' : ''}>ignore</option></select></label>` : ''}
        <div class="field-label"><span class="lbl">View</span>
          <div class="seg" id="decode-view">
            <button data-view="fields" class="${dui.view !== 'print' && dui.view !== 'line' ? 'on' : ''}">Fields</button>
            <button data-view="print" class="${dui.view === 'print' ? 'on' : ''}">Print</button>
            <button data-view="line" class="${dui.view === 'line' ? 'on' : ''}">One line</button>
          </div>
        </div>
        <div class="decode-actions">
          ${D.exampleInput(s, dui.target) ? '<button class="btn small" data-action="decodeExample">Example input</button>' : ''}
          <button class="btn small" data-action="decodeClear">Clear</button>
        </div>
      </div>
      <textarea id="decode-input" spellcheck="false" placeholder="${dui.format === 'hex'
        ? 'Paste bytes, e.g.  AA 55 01 00 10 5A …   (spaces, commas and 0x are optional; # starts a comment)'
        : 'Paste text, e.g.  $GPGGA,123519,4807.038,N,…*47\\r\\n   (escapes: \\r \\n \\t \\xHH)'}">${esc(dui.input)}</textarea>
      <div id="decode-results"></div>
    </div>`;
  };

  G.decodeTargetOptions = (s, dui) => {
    const targets = G.decodeTargets(s);
    const groups = {};
    targets.forEach((t) => (groups[t.group] = groups[t.group] || []).push(t));
    return Object.entries(groups).map(([g, list]) => `<optgroup label="${esc(g)}">${list.map((t) =>
      `<option value="${esc(t.key)}"${t.key === dui.target ? ' selected' : ''}>${esc(t.label)}${t.text ? ' (text)' : ''}</option>`).join('')}</optgroup>`).join('');
  };

  // ----------------------------------------------------------------------------------------------
  function runDecode(s, dui) {
    let text = dui.input;
    if (dui.format === 'text') {
      // textareas normalise line breaks to \n; map them to what the protocol expects
      if (dui.newline === 'crlf') text = text.replace(/\r?\n/g, '\r\n');
      else if (dui.newline === 'ignore') text = text.replace(/\r?\n/g, '');
    }
    const parsed = dui.format === 'hex' ? D.parseHex(text) : D.parseText(text);
    if (parsed.error) return { parseError: parsed.error, bytes: parsed.bytes };
    const [kind, name] = (dui.target || '').split(/:(.*)/s);
    let result;
    if (kind === 'protocol') {
      const p = s.protocols.find((x) => x.name === name);
      if (!p) return { parseError: 'select something to decode as', bytes: parsed.bytes };
      result = D.decodeProtocol(s, p, parsed.bytes);
    } else {
      const m = s.messages.find((x) => x.name === name);
      if (!m) return { parseError: 'select something to decode as', bytes: parsed.bytes };
      result = D.decodeMessage(s, m, parsed.bytes);
    }
    return { bytes: parsed.bytes, ...result };
  }

  G.renderDecodeResults = (s, dui) => {
    if (!dui.input.trim()) return `<div class="decode-empty">Paste a raw message above. Fields are decoded live as you type.</div>`;
    let r;
    try { r = runDecode(s, dui); } catch (e) { return `<div class="callout bad">Decoder error: ${esc(e.message)}</div>`; }
    if (r.parseError) return `<div class="callout bad">Input error: ${esc(r.parseError)}</div>`;
    const bytes = r.bytes;

    // assign ids; the deepest node wins each byte
    let nid = 0;
    const owner = new Array(bytes.length).fill(null);
    const ids = new Map();
    const walk = (nodes, depth) => nodes.forEach((n) => {
      n.id = 'n' + nid++;
      n.depth = depth;
      ids.set(n.id, n);
      for (let i = n.start; i < n.end && i < bytes.length; i++) if (!owner[i] || owner[i].depth <= depth) owner[i] = n;
      walk(n.children, depth + 1);
    });
    r.items.forEach((it) => {
      if (it.type === 'frame') walk(it.nodes, 0);
      else {
        it.id = 'n' + nid++;
        const g = { id: it.id, kind: it.type === 'garbage' ? 'garbage' : 'incomplete', depth: -1 };
        for (let i = it.start; i < it.end; i++) owner[i] = owner[i] || g;
      }
    });

    const frames = r.items.filter((i) => i.type === 'frame');
    const good = frames.filter((f) => !f.errors.length).length;
    const skipped = r.items.filter((i) => i.type !== 'frame').reduce((a, i) => a + i.end - i.start, 0);
    const summary = `<div class="decode-summary">
      <span class="pill ${good === frames.length && frames.length ? 'ok' : frames.length ? 'warn' : 'bad'}">${frames.length} frame${frames.length === 1 ? '' : 's'}</span>
      <span class="pill ok">${good} valid</span>
      ${frames.length - good ? `<span class="pill bad">${frames.length - good} with errors</span>` : ''}
      ${skipped ? `<span class="pill muted">${skipped} byte${skipped === 1 ? '' : 's'} skipped</span>` : ''}
      <span class="pill muted">${bytes.length} bytes in</span>
    </div>`;

    // byte view grouped per item
    const textView = dui.format === 'text';
    const cell = (i) => {
      const o = owner[i];
      const kind = !o ? 'none' : o.kind === 'garbage' || o.kind === 'incomplete' ? o.kind : KIND_CLASS[o.kind] || 'pad';
      const b = bytes[i];
      const label = textView ? (b === 32 ? '␠' : b === 13 ? '␍' : b === 10 ? '␊' : b >= 33 && b < 127 ? esc(String.fromCharCode(b)) : '·') : b.toString(16).toUpperCase().padStart(2, '0');
      const err = o && o.error ? ' err' : '';
      return `<span class="cell c-${kind}${err}" data-i="${i}" data-n="${o ? o.id : ''}" title="${i}: 0x${b.toString(16).toUpperCase().padStart(2, '0')}${o && o.name ? ' — ' + esc(o.name) : ''}">${label}</span>`;
    };
    const blocks = r.items.map((it, k) => {
      let head;
      if (it.type === 'frame') {
        const ok = !it.errors.length;
        head = `<span class="fh ${ok ? 'ok' : 'bad'}">${ok ? '✓' : '✗'} ${it.message ? esc(it.message) : 'frame'}</span><span class="fr">bytes ${it.start}–${it.end - 1}</span>`;
      } else {
        head = `<span class="fh ${it.type === 'garbage' ? 'muted' : 'warn'}">${it.type === 'garbage' ? 'skipped' : 'incomplete'}</span><span class="fr">${esc(it.error)}</span>`;
      }
      const cells = [];
      for (let i = it.start; i < it.end; i++) cells.push(cell(i));
      return `<div class="bytes-block" data-item="${k}"><div class="bytes-head">${head}</div><div class="cells${textView ? ' text' : ''}">${cells.join('')}</div></div>`;
    }).join('');

    // decoded tree
    const rows = (nodes) => nodes.map((n) => {
      const kind = KIND_CLASS[n.kind] || 'pad';
      const range = n.end > n.start ? (n.end - n.start === 1 ? `${n.start}` : `${n.start}–${n.end - 1}`) : '—';
      return `<tr class="drow${n.error ? ' bad' : ''}" data-n="${n.id}" data-s="${n.start}" data-e="${n.end}">
          <td class="dn" style="padding-left:${10 + n.depth * 16}px"><i class="sw" style="background:var(--k-${kind}-b)"></i>${esc(n.name)}</td>
          <td class="dv">${n.error ? `<span class="derr">${esc(n.error)}</span>${n.value ? ' · ' : ''}` : ''}${esc(n.value)}${n.note ? ` <span class="dnote">${esc(n.note)}</span>` : ''}</td>
          <td class="dr mono">${esc(n.raw)}</td>
          <td class="dt mono">${esc(n.type || '')}</td>
          <td class="dpos mono">${range}</td>
        </tr>${rows(n.children)}`;
    }).join('');
    const tree = r.items.map((it, k) => {
      if (it.type !== 'frame') {
        return `<div class="frame-card skip" data-item="${k}"><div class="frame-title"><span>${it.type === 'garbage' ? 'Skipped bytes' : 'Incomplete'}</span><span class="mono">${it.start}–${it.end - 1}</span></div>
          <div class="frame-errs">${esc(it.error)}: <span class="mono">${esc(D.hex(bytes.slice(it.start, it.end), 24))}</span></div></div>`;
      }
      const ok = !it.errors.length;
      const printing = dui.view === 'print' || dui.view === 'line';
      const printed = printing ? G.printer.frame(s, it, dui.view === 'line' ? 'single' : 'multi') : null;
      const body = !printing ? '' : printed !== null
        ? `<pre class="print-out">${esc(printed)}</pre>`
        : '<div class="frame-note">Nothing to print: decoding this frame fails, so the C++ code never gets a message.</div>';
      return `<div class="frame-card ${ok ? 'ok' : 'bad'}" data-item="${k}">
        <div class="frame-title"><span>${ok ? '✓' : '✗'} ${esc(it.message || 'Frame')}</span><span class="mono">${it.end - it.start} bytes @ ${it.start}</span></div>
        ${it.errors.length ? `<div class="frame-errs">${it.errors.map(esc).join(' · ')}</div>` : ''}
        ${it.notes ? `<div class="frame-note">${it.notes.map(esc).join(' · ')}</div>` : ''}
        ${printing ? body : `<table class="dtable"><thead><tr><th>Field</th><th>Value</th><th>Raw</th><th>Type</th><th>Bytes</th></tr></thead><tbody>${rows(it.nodes)}</tbody></table>`}
      </div>`;
    }).join('');

    return `${summary}<div class="decode-grid"><div class="bytes-view">${blocks}</div><div class="decode-tree">${tree}</div></div>`;
  };
})(window.GMP);
