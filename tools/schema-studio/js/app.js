// app.js - wiring: state, selection, generic edit handling, rendering, import / export.
(function (G) {
  'use strict';

  let state = G.load() || G.exampleState();
  const ui = { sel: { kind: 'schema' }, field: null, tab: 'layout', issuesOpen: false };
  if (state.messages.length) ui.sel = { kind: 'message', uid: state.messages[state.messages.length > 1 ? 1 : 0].uid };

  const $ = (sel) => document.querySelector(sel);
  const left = $('#left-pane');
  const tabs = { layout: $('#tab-layout'), yaml: $('#tab-yaml'), decode: $('#tab-decode') };
  let issues = [];

  // ----------------------------------------------------------------------------------------------
  // rendering
  // ----------------------------------------------------------------------------------------------
  function renderLeft() {
    const scroll = left.scrollTop;
    left.innerHTML = G.renderEditor(state, ui, issues);
    left.scrollTop = scroll;
  }

  function renderRight() {
    if (ui.tab === 'layout') {
      const scroll = tabs.layout.scrollTop;  // keep the view still across re-renders
      tabs.layout.innerHTML = G.renderLayout(state, ui, issues);
      tabs.layout.scrollTop = scroll;
    }
    if (ui.tab === 'yaml') {
      const text = G.toYaml(state);
      tabs.yaml.innerHTML = `<div class="yaml-wrap"><div class="yaml-tools">
          <span class="badge" style="align-self:center">${text.split('\n').length - 1} lines</span>
          <button class="btn small" data-action="copyYaml">Copy</button>
          <button class="btn small primary" data-action="downloadYaml">Download</button></div>
        <pre class="yaml">${G.highlightYaml(text)}</pre></div>`;
    }
    if (ui.tab === 'decode') renderDecode();
    renderIssues();
  }

  // ---- decode tester ------------------------------------------------------------------------------
  // dui = the active target's input; `saved` remembers the input of every other target.
  const DECODE_KEY = 'gmp-schema-studio-decoder-v2';
  const dui = (() => {
    const base = { target: '', format: 'hex', input: '', newline: 'lf', saved: {} };
    try { return { ...base, ...JSON.parse(localStorage.getItem(DECODE_KEY) || '{}') }; } catch (_) { return base; }
  })();
  const saveDecode = () => { try { localStorage.setItem(DECODE_KEY, JSON.stringify(dui)); } catch (_) { /* ignore */ } };

  /** Default input settings for a target: format from the schema, CRLF if the frame ends in \r\n. */
  function decodeDefaults(key) {
    const t = G.decodeTargets(state).find((x) => x.key === key);
    let newline = 'lf';
    if (key.startsWith('protocol:')) {
      const p = state.protocols.find((x) => 'protocol:' + x.name === key);
      const end = p ? G.patternBytes(p.framing.endMode, p.framing.end) || [] : [];
      if (end.length >= 2 && end[end.length - 2] === 13 && end[end.length - 1] === 10) newline = 'crlf';
    }
    return { format: t && t.text ? 'text' : 'hex', newline, input: G.decoder.exampleInput(state, key) || '' };
  }

  function switchDecodeTarget(key, { remember = true, reset = false } = {}) {
    if (remember && dui.target) dui.saved[dui.target] = { format: dui.format, input: dui.input, newline: dui.newline };
    Object.assign(dui, (!reset && dui.saved[key]) || decodeDefaults(key));
    dui.target = key;
    saveDecode();
  }

  /** Keep the selected target valid; pick a sensible default. */
  function ensureDecodeTarget() {
    const targets = G.decodeTargets(state);
    if (targets.some((t) => t.key === dui.target)) return;
    if (targets[0]) switchDecodeTarget(targets[0].key, { remember: false });
    else dui.target = '';
  }

  /** The shell (controls + textarea) is built once so typing never loses focus; results re-render. */
  function renderDecode(rebuild = false) {
    ensureDecodeTarget();
    const ta = tabs.decode.querySelector('#decode-input');
    if (!ta || rebuild) {
      tabs.decode.innerHTML = G.renderDecodeShell(state, dui);
    } else {
      const sel = tabs.decode.querySelector('#decode-target');
      if (sel) sel.innerHTML = G.decodeTargetOptions(state, dui);
    }
    const res = tabs.decode.querySelector('#decode-results');
    if (res) res.innerHTML = G.renderDecodeResults(state, dui);
  }

  function highlight(start, end, nodeId) {
    tabs.decode.querySelectorAll('.cell.hl, .drow.hl').forEach((e) => e.classList.remove('hl'));
    if (start == null) return;
    tabs.decode.querySelectorAll('.cell').forEach((c) => {
      const i = Number(c.dataset.i);
      if (i >= start && i < end) c.classList.add('hl');
    });
    if (nodeId) tabs.decode.querySelectorAll(`.drow[data-n="${nodeId}"]`).forEach((r) => r.classList.add('hl'));
  }

  tabs.decode.addEventListener('input', (e) => {
    if (e.target.id !== 'decode-input') return;
    dui.input = e.target.value;
    saveDecode();
    const res = tabs.decode.querySelector('#decode-results');
    if (res) res.innerHTML = G.renderDecodeResults(state, dui);
  });
  tabs.decode.addEventListener('change', (e) => {
    if (e.target.id === 'decode-target') {
      switchDecodeTarget(e.target.value);
      renderDecode(true);
    } else if (e.target.id === 'decode-newline') {
      dui.newline = e.target.value;
      saveDecode();
      renderDecode();
    }
  });
  tabs.decode.addEventListener('click', (e) => {
    const b = e.target.closest('#decode-format button');
    if (!b) return;
    dui.format = b.dataset.format;
    saveDecode();
    renderDecode(true);
  });
  tabs.decode.addEventListener('mouseover', (e) => {
    const row = e.target.closest('.drow');
    if (row) { highlight(Number(row.dataset.s), Number(row.dataset.e), row.dataset.n); return; }
    const cell = e.target.closest('.cell');
    if (cell && cell.dataset.n) {
      const r = tabs.decode.querySelector(`.drow[data-n="${cell.dataset.n}"]`);
      if (r) highlight(Number(r.dataset.s), Number(r.dataset.e), r.dataset.n);
      else highlight(Number(cell.dataset.i), Number(cell.dataset.i) + 1, null);
      return;
    }
    highlight(null);
  });
  tabs.decode.addEventListener('mouseleave', () => highlight(null), true);

  function renderIssues() {
    const errs = issues.filter((i) => i.level === 'error').length;
    const warns = issues.length - errs;
    const el = $('#issues');
    el.classList.toggle('collapsed', !ui.issuesOpen);
    const summary = !issues.length
      ? '<span class="ok">✓ No problems found</span><span style="color:var(--faint);font-weight:400">— run <code>gmpc --check</code> for the full validation</span>'
      : `${errs ? `<span class="bad">${errs} error${errs > 1 ? 's' : ''}</span>` : ''}${warns ? `<span class="warnc">${warns} warning${warns > 1 ? 's' : ''}</span>` : ''}<span style="color:var(--faint);font-weight:400">${ui.issuesOpen ? 'click to hide' : 'click to show'}</span>`;
    el.innerHTML = `<div class="issues-head" data-action="toggleIssues">${summary}</div>` +
      issues.map((i, n) => `<div class="issue" data-action="gotoIssue" data-index="${n}"><span class="lvl ${i.level}">${i.level}</span><span class="where">${G.esc(i.where)}</span><span>${G.esc(i.msg)}</span></div>`).join('');
  }

  let rightTimer = 0;
  function refresh({ leftToo = false, immediate = false } = {}) {
    issues = G.validate(state);
    G.save(state);
    if (leftToo) renderLeft();
    clearTimeout(rightTimer);
    if (immediate || leftToo) renderRight();
    else rightTimer = setTimeout(renderRight, 90);
  }

  function setTab(t) {
    ui.tab = t;
    document.querySelectorAll('.tab').forEach((b) => b.classList.toggle('active', b.dataset.tab === t));
    Object.entries(tabs).forEach(([k, el]) => el.classList.toggle('hidden', k !== t));
    renderRight();
  }

  function toast(msg) {
    const t = $('#toast');
    t.textContent = msg;
    t.classList.add('show');
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => t.classList.remove('show'), 2200);
  }

  // ----------------------------------------------------------------------------------------------
  // selection helpers
  // ----------------------------------------------------------------------------------------------
  const listOf = (kind) => ({ enum: state.enums, message: state.messages, protocol: state.protocols }[kind]);
  /** Select an item. Cards keep their position; when the selection came from the left pane and the
   *  card is off-screen, the layout scrolls just enough to reveal it. */
  function select(kind, uid, field = null, fromRight = false) {
    ui.sel = { kind, uid };
    ui.field = field;
    renderLeft();
    renderRight();
    if (field) {
      const el = left.querySelector(`[data-field-uid="${field}"]`);
      if (el) el.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    }
    if (!fromRight && ui.tab === 'layout' && uid) {
      const card = tabs.layout.querySelector(`#card-${CSS.escape(uid)}`);
      if (card) {
        const box = tabs.layout.getBoundingClientRect(), r = card.getBoundingClientRect();
        if (r.top < box.top || r.top > box.bottom - 60) card.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
      }
    }
  }
  function uniqueName(base, list) {
    let n = base, i = 2;
    while (list.some((x) => x.name === n)) n = base + i++;
    return n;
  }
  function allTypeNames() {
    return [...state.enums, ...state.messages, ...state.protocols, ...state.customTypes, ...state.customChecksums, ...state.customFramers];
  }
  function deepCloneWithNewUids(obj) {
    const c = JSON.parse(JSON.stringify(obj));
    const walk = (o) => {
      if (Array.isArray(o)) o.forEach(walk);
      else if (o && typeof o === 'object') {
        if (o.uid) o.uid = G.uid();
        Object.values(o).forEach(walk);
      }
    };
    walk(c);
    return c;
  }

  // ----------------------------------------------------------------------------------------------
  // actions
  // ----------------------------------------------------------------------------------------------
  const actions = {
    select: (el) => select(el.dataset.kind, el.dataset.uid, null, !!el.closest('.pane.right')),
    selectField: (el) => select(el.dataset.kind, el.dataset.uid, el.dataset.field, true),
    toggleField: (el) => { ui.field = ui.field === el.dataset.uid ? null : el.dataset.uid; renderLeft(); renderRight(); },
    addMessage: () => {
      const m = G.newMessage({ name: uniqueName('Message', allTypeNames()) });
      m.fields.push(G.newField({ name: 'value', type: 'u16' }));
      state.messages.push(m);
      ui.sel = { kind: 'message', uid: m.uid };
      ui.field = m.fields[0].uid;
      refresh({ leftToo: true });
    },
    addProtocol: () => {
      const p = G.newProtocol({ name: uniqueName('Link', allTypeNames()) });
      state.protocols.push(p);
      ui.sel = { kind: 'protocol', uid: p.uid };
      refresh({ leftToo: true });
    },
    addEnum: () => {
      const e = G.newEnum({ name: uniqueName('Kind', allTypeNames()), values: [G.newEnumValue('a', '0'), G.newEnumValue('b', '1')] });
      state.enums.push(e);
      ui.sel = { kind: 'enum', uid: e.uid };
      refresh({ leftToo: true });
    },
    addCustomType: () => { const c = G.newCustomType({ name: uniqueName('custom_type', allTypeNames()) }); state.customTypes.push(c); ui.sel = { kind: 'custom', uid: c.uid }; refresh({ leftToo: true }); },
    addCustomChecksum: () => { const c = G.newCustomChecksum({ name: uniqueName('my_checksum', allTypeNames()) }); state.customChecksums.push(c); ui.sel = { kind: 'custom', uid: c.uid }; refresh({ leftToo: true }); },
    addCustomFramer: () => { const c = G.newCustomFramer({ name: uniqueName('my_framer', allTypeNames()) }); state.customFramers.push(c); ui.sel = { kind: 'custom', uid: c.uid }; refresh({ leftToo: true }); },
    addField: (el) => {
      const list = G.getPath(state, el.dataset.path);
      const preset = el.dataset.preset;
      const names = list.map((f) => f.name);
      let f;
      if (preset === 'bits') f = G.newField({ name: uniqueName('flags', names.map((n) => ({ name: n }))), type: 'bits', bits: [G.newBit({ name: 'flag', bits: '1' }), G.newBit({ pad: true, bits: '7' })] });
      else if (preset === 'pad') f = G.newField({ type: 'pad', padSize: '1' });
      else f = G.newField({ name: uniqueName('field', names.map((n) => ({ name: n }))), type: 'u8' });
      list.push(f);
      ui.field = f.uid;
      refresh({ leftToo: true });
    },
    addBit: (el) => {
      const list = G.getPath(state, el.dataset.path);
      if (el.dataset.pad) list.push(G.newBit({ pad: true, bits: el.dataset.pad }));
      else list.push(G.newBit({ name: uniqueName('bit', list), bits: '1' }));
      refresh({ leftToo: true });
    },
    addEnumValue: (el) => {
      const list = G.getPath(state, el.dataset.path);
      const next = list.length ? Math.max(...list.map((v) => G.parseInt(v.value) || 0)) + 1 : 0;
      list.push(G.newEnumValue(uniqueName('value', list), String(next)));
      refresh({ leftToo: true });
    },
    delete: (el) => {
      const keys = G.splitPath(el.dataset.path);
      const idx = keys.pop();
      const list = G.getPath(state, keys.join('.'));
      list.splice(idx, 1);
      refresh({ leftToo: true });
    },
    move: (el) => {
      const keys = G.splitPath(el.dataset.path);
      const idx = keys.pop();
      const list = G.getPath(state, keys.join('.'));
      const to = idx + Number(el.dataset.dir);
      if (to < 0 || to >= list.length) return;
      [list[idx], list[to]] = [list[to], list[idx]];
      refresh({ leftToo: true });
    },
    duplicate: (el) => {
      const keys = G.splitPath(el.dataset.path);
      const idx = keys.pop();
      const list = G.getPath(state, keys.join('.'));
      const copy = deepCloneWithNewUids(list[idx]);
      if (copy.name) copy.name = uniqueName(copy.name + '_copy', list);
      list.splice(idx + 1, 0, copy);
      ui.field = copy.uid;
      refresh({ leftToo: true });
    },
    duplicateItem: (el) => {
      const list = listOf(el.dataset.kind);
      const i = list.findIndex((x) => x.uid === el.dataset.uid);
      const copy = deepCloneWithNewUids(list[i]);
      copy.name = uniqueName(copy.name + 'Copy', allTypeNames());
      list.splice(i + 1, 0, copy);
      ui.sel = { kind: el.dataset.kind, uid: copy.uid };
      refresh({ leftToo: true });
    },
    deleteItem: (el) => {
      const { kind, uid } = el.dataset;
      const lists = kind === 'custom' ? [state.customTypes, state.customChecksums, state.customFramers] : [listOf(kind)];
      for (const list of lists) {
        const i = list.findIndex((x) => x.uid === uid);
        if (i < 0) continue;
        if (!confirm(`Delete "${list[i].name}"?`)) return;
        list.splice(i, 1);
      }
      ui.sel = { kind: 'schema' };
      ui.field = null;
      refresh({ leftToo: true });
    },
    toggleProtoMsg: (el) => {
      const p = G.getPath(state, el.dataset.path);
      const n = el.dataset.name;
      p.messages = p.messages.includes(n) ? p.messages.filter((x) => x !== n) : [...p.messages, n];
      if (p.messages.length > 1 && p.message_type.kind === 'none') {
        p.message_type.kind = G.protoFormat(state, p) === 'text' ? 'token' : 'field';
        if (p.message_type.kind === 'field' && !p.message_type.field && p.header.length) p.message_type.field = p.header[0].name;
      }
      refresh({ leftToo: true });
    },
    toggleIssues: () => { ui.issuesOpen = !ui.issuesOpen; renderIssues(); },
    gotoIssue: (el) => {
      const i = issues[Number(el.dataset.index)];
      if (!i) return;
      if (i.target.kind === 'schema') select('schema');
      else select(i.target.kind, i.target.uid, i.target.fieldUid || null);
    },
    newSchema: () => {
      if (!confirm('Start a new, empty schema? The current one will be replaced.')) return;
      state = G.emptyState();
      ui.sel = { kind: 'schema' };
      ui.field = null;
      refresh({ leftToo: true });
    },
    loadExample: () => {
      if ((state.messages.length || state.protocols.length) && !confirm('Replace the current schema with the example?')) return;
      state = G.exampleState();
      ui.sel = { kind: 'protocol', uid: state.protocols[0].uid };
      ui.field = null;
      dui.saved = {};
      switchDecodeTarget('protocol:Link', { remember: false, reset: true });
      if (tabs.decode.querySelector('#decode-input')) tabs.decode.innerHTML = '';
      refresh({ leftToo: true });
    },
    copyYaml: async () => {
      const text = G.toYaml(state);
      try { await navigator.clipboard.writeText(text); toast('YAML copied to clipboard'); }
      catch (_) { toast('Clipboard not available - use Download'); }
    },
    downloadYaml: () => {
      const blob = new Blob([G.toYaml(state)], { type: 'text/yaml' });
      const a = document.createElement('a');
      const base = (state.schema.namespace || 'schema').split('::').pop() || 'schema';
      a.href = URL.createObjectURL(blob);
      a.download = `${base}.yaml`;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
      const errs = issues.filter((i) => i.level === 'error').length;
      toast(errs ? `Downloaded (with ${errs} unresolved issue${errs > 1 ? 's' : ''})` : `Downloaded ${a.download}`);
    },
    importYaml: () => $('#import-file').click(),
    decodeExample: () => {
      if (!G.decoder.exampleInput(state, dui.target)) return;
      switchDecodeTarget(dui.target, { remember: false, reset: true });
      renderDecode(true);
    },
    decodeClear: () => {
      dui.input = '';
      saveDecode();
      renderDecode(true);
      const ta = tabs.decode.querySelector('#decode-input');
      if (ta) ta.focus();
    },
  };

  // ----------------------------------------------------------------------------------------------
  // events
  // ----------------------------------------------------------------------------------------------
  document.addEventListener('click', (e) => {
    const tab = e.target.closest('.tab');
    if (tab) { setTab(tab.dataset.tab); return; }
    const el = e.target.closest('[data-action]');
    if (!el || el.disabled) return;
    // let inputs inside clickable regions work normally
    if (e.target.closest('input, select, textarea, label.check') && el.contains(e.target.closest('input, select, textarea, label.check'))) return;
    const fn = actions[el.dataset.action];
    if (fn) { e.preventDefault(); e.stopPropagation(); fn(el, e); }
  });

  const renameOld = new WeakMap();
  left.addEventListener('focusin', (e) => {
    if (e.target.dataset && e.target.dataset.rename) renameOld.set(e.target, e.target.value);
  });

  function applyInput(el) {
    const path = el.dataset.path;
    const value = el.type === 'checkbox' ? el.checked : el.value;
    G.setPath(state, path, value);
    return path;
  }

  left.addEventListener('input', (e) => {
    const el = e.target;
    if (!el.dataset || !el.dataset.path || el.tagName === 'SELECT' || el.type === 'checkbox') return;
    const path = applyInput(el);
    left.querySelectorAll(`[data-mirror="${CSS.escape(path)}"]`).forEach((m) => { m.textContent = el.value || 'unnamed'; });
    refresh();
  });

  left.addEventListener('change', (e) => {
    const el = e.target;
    if (!el.dataset || !el.dataset.path) return;
    const path = applyInput(el);
    if (el.dataset.rename) {
      const old = renameOld.get(el);
      if (el.dataset.rename === 'type' && old && old !== el.value) G.renameType(state, old, el.value);
      renameOld.set(el, el.value);
    }
    // a type change resets the selection-dependent options; keep the field open
    if (/\.type$/.test(path) && /\.(fields|header)\.\d+\.type$/.test(path)) {
      const f = G.getPath(state, path.replace(/\.type$/, ''));
      if (f.type === 'bits' && !f.bits.length) f.bits.push(G.newBit({ name: 'flag', bits: '1' }), G.newBit({ pad: true, bits: '7' }));
    }
    refresh({ leftToo: el.tagName === 'SELECT' || el.type === 'checkbox' || !!el.dataset.rerender || !!el.dataset.rename });
  });

  $('#import-file').addEventListener('change', async (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (!file) return;
    try {
      const text = await file.text();
      await G.loadJsYaml();
      state = G.fromYaml(text);
      ui.sel = state.protocols.length ? { kind: 'protocol', uid: state.protocols[0].uid } : state.messages.length ? { kind: 'message', uid: state.messages[0].uid } : { kind: 'schema' };
      ui.field = null;
      refresh({ leftToo: true });
      toast(`Imported ${file.name}`);
    } catch (err) {
      alert('Import failed: ' + (err && err.message ? err.message : err));
    }
  });

  // splitter
  const splitter = $('#splitter');
  splitter.addEventListener('pointerdown', (e) => {
    splitter.setPointerCapture(e.pointerId);
    splitter.classList.add('dragging');
    const move = (ev) => { left.style.width = Math.max(360, Math.min(window.innerWidth * 0.7, ev.clientX)) + 'px'; };
    const up = () => { splitter.classList.remove('dragging'); splitter.removeEventListener('pointermove', move); splitter.removeEventListener('pointerup', up); };
    splitter.addEventListener('pointermove', move);
    splitter.addEventListener('pointerup', up);
  });

  // ----------------------------------------------------------------------------------------------
  // Deep links: index.html?select=<Message|Protocol|Enum name>&field=<field name>&tab=layout|yaml|decode
  const params = new URLSearchParams(location.search);
  if (params.get('example') !== null) state = G.exampleState();
  if (params.get('select')) {
    const name = params.get('select');
    for (const kind of ['message', 'protocol', 'enum']) {
      const item = listOf(kind).find((x) => x.name === name);
      if (!item) continue;
      ui.sel = { kind, uid: item.uid };
      const fname = params.get('field');
      const flist = item.fields || item.header || [];
      const f = fname && flist.find((x) => x.name === fname);
      if (f) ui.field = f.uid;
    }
  }

  // ?target=protocol:Nmea selects a decode target; &decode=<input> overrides its input.
  if (params.get('example') !== null) dui.saved = {};
  if (params.get('target')) switchDecodeTarget(params.get('target'), { remember: false, reset: params.get('example') !== null });
  if (params.get('decode') !== null) dui.input = params.get('decode');
  if (params.get('format')) dui.format = params.get('format');
  if (!dui.target) ensureDecodeTarget();
  if (!dui.input && G.decoder.exampleInput(state, dui.target)) switchDecodeTarget(dui.target, { remember: false, reset: true });

  $('#legend').innerHTML = G.renderLegend();
  issues = G.validate(state);
  renderLeft();
  if (params.get('tab') && tabs[params.get('tab')]) setTab(params.get('tab'));
  else renderRight();
})(window.GMP);
