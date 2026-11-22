// editor.js - renders the left pane (outline + editor for the selected item).
// Inputs carry data-path="<path into state>"; app.js applies edits generically.
(function (G) {
  'use strict';
  const esc = G.esc;

  // ----------------------------------------------------------------------------------------------
  // small HTML helpers
  // ----------------------------------------------------------------------------------------------
  const ICONS = {
    up: '<svg viewBox="0 0 24 24"><path d="M18 15l-6-6-6 6"/></svg>',
    down: '<svg viewBox="0 0 24 24"><path d="M6 9l6 6 6-6"/></svg>',
    trash: '<svg viewBox="0 0 24 24"><path d="M3 6h18M8 6V4h8v2M19 6l-1 14H6L5 6"/></svg>',
    copy: '<svg viewBox="0 0 24 24"><rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/></svg>',
    plus: '<svg viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></svg>',
  };
  G.ICONS = ICONS;
  const iconBtn = (icon, action, attrs = '', title = '', danger = false) =>
    `<button class="icon-btn${danger ? ' danger' : ''}" data-action="${action}" ${attrs} title="${esc(title)}">${ICONS[icon]}</button>`;

  const input = (path, value, opts = {}) =>
    `<input type="text" data-path="${path}" value="${esc(value)}"${opts.placeholder ? ` placeholder="${esc(opts.placeholder)}"` : ''}` +
    `${opts.mono ? ' class="mono"' : ''}${opts.rename ? ` data-rename="${opts.rename}"` : ''}${opts.mirror ? ` data-mirror-src="1"` : ''}${opts.rerender ? ' data-rerender="1"' : ''} spellcheck="false" autocomplete="off">`;

  const select = (path, value, options, opts = {}) => {
    const body = options.map((o) => {
      if (o.group) return `<optgroup label="${esc(o.group)}">${o.items.map((it) => opt(it, value)).join('')}</optgroup>`;
      return opt(o, value);
    }).join('');
    return `<select data-path="${path}" data-rerender="1"${opts.cls ? ` class="${opts.cls}"` : ''}>${body}</select>`;
  };
  const opt = (o, value) => {
    const [v, label] = Array.isArray(o) ? o : [o, o];
    return `<option value="${esc(v)}"${String(v) === String(value) ? ' selected' : ''}>${esc(label)}</option>`;
  };

  const check = (path, value, label) =>
    `<label class="check"><input type="checkbox" data-path="${path}" data-rerender="1"${value ? ' checked' : ''}> ${esc(label)}</label>`;

  const field = (label, control, hint = '', cls = '') =>
    `<label class="field-label ${cls}"><span class="lbl">${esc(label)}${hint ? ` <span class="hint">· ${esc(hint)}</span>` : ''}</span>${control}</label>`;

  const section = (title, body, tools = '', flat = false) =>
    `<div class="section${flat ? ' flat' : ''}"><div class="section-head"><h3>${esc(title)}</h3><div class="inline">${tools}</div></div><div class="section-body">${body}</div></div>`;

  const inherit = (label, v) => [['', `inherit (${v})`], ...label];

  // ----------------------------------------------------------------------------------------------
  // type option lists
  // ----------------------------------------------------------------------------------------------
  function typeOptions(s, owner, text) {
    const groups = [
      { group: 'Integers', items: G.INT_PRIMS },
      { group: 'Float / other', items: ['f32', 'f64', 'bool', 'char'] },
    ];
    if (text) groups.push({ group: 'Text', items: [['string', 'string'], ['pad', 'pad (skip tokens)']] });
    else groups.push({ group: 'Composite', items: [['bits', 'bits (bit fields)'], 'bytes', 'string', 'array', ['pad', 'pad (skip bytes)']] });
    if (s.enums.length) groups.push({ group: 'Enums', items: s.enums.map((e) => e.name) });
    const msgs = s.messages.filter((m) => m.uid !== owner.uid && !owner.isHeader && G.msgFormat(s, m) === G.msgFormat(s, owner));
    if (msgs.length && !text) groups.push({ group: 'Messages', items: msgs.map((m) => m.name) });
    if (s.customTypes.length) groups.push({ group: 'Custom', items: s.customTypes.map((c) => c.name) });
    return groups;
  }
  function elemOptions(s, owner) {
    const groups = [{ group: 'Scalars', items: [...G.INT_PRIMS, 'f32', 'f64', 'bool', 'char'] }];
    if (s.enums.length) groups.push({ group: 'Enums', items: s.enums.map((e) => e.name) });
    const msgs = s.messages.filter((m) => m.uid !== owner.uid && G.msgFormat(s, m) === 'binary');
    if (msgs.length) groups.push({ group: 'Messages', items: msgs.map((m) => m.name) });
    if (s.customTypes.length) groups.push({ group: 'Custom', items: s.customTypes.map((c) => c.name) });
    return groups;
  }
  function bitTypeOptions(s) {
    const groups = [{ group: 'Storage', items: [['', 'auto'], 'bool', ...G.INT_PRIMS] }];
    if (s.enums.length) groups.push({ group: 'Enums', items: s.enums.map((e) => e.name) });
    return groups;
  }

  // ----------------------------------------------------------------------------------------------
  // outline
  // ----------------------------------------------------------------------------------------------
  function issueCount(issues, kind, uid) {
    return issues.filter((i) => i.level === 'error' && i.target.kind === kind && i.target.uid === uid).length;
  }

  function outline(s, ui, issues) {
    const chip = (kind, uid, name, color) => {
      const active = ui.sel.kind === kind && ui.sel.uid === uid;
      const n = issueCount(issues, kind, uid);
      return `<button class="chip${active ? ' active' : ''}" data-action="select" data-kind="${kind}" data-uid="${uid}">` +
        `${color ? `<span class="dot" style="background:var(--k-${color}-b)"></span>` : ''}${esc(name || '(unnamed)')}${n ? ` <span class="err">${n}</span>` : ''}</button>`;
    };
    const group = (title, items, addAction, addLabel) =>
      `<div class="outline-group"><div class="outline-head"><span class="outline-title">${title}</span>` +
      `${addAction ? `<button class="btn ghost small" data-action="${addAction}">+ ${addLabel}</button>` : ''}</div>` +
      `<div class="outline-items">${items.length ? items.join('') : `<span class="chip empty">none yet</span>`}</div></div>`;

    const schemaErr = issues.filter((i) => i.level === 'error' && i.target.kind === 'schema').length;
    return `<div class="outline">
      ${group('Schema', [`<button class="chip${ui.sel.kind === 'schema' ? ' active' : ''}" data-action="select" data-kind="schema">Settings &amp; defaults${schemaErr ? ` <span class="err">${schemaErr}</span>` : ''}</button>`])}
      ${group('Protocols', s.protocols.map((p) => chip('protocol', p.uid, p.name, 'frame')), 'addProtocol', 'Protocol')}
      ${group('Messages', s.messages.map((m) => chip('message', m.uid, m.name, G.msgFormat(s, m) === 'text' ? 'string' : 'message')), 'addMessage', 'Message')}
      ${group('Enums', s.enums.map((e) => chip('enum', e.uid, e.name, 'enum')), 'addEnum', 'Enum')}
      <div class="outline-group"><div class="outline-head"><span class="outline-title">Custom extensions</span>
        <span class="inline"><button class="btn ghost small" data-action="addCustomType">+ Type</button><button class="btn ghost small" data-action="addCustomChecksum">+ Checksum</button><button class="btn ghost small" data-action="addCustomFramer">+ Framer</button></span></div>
        <div class="outline-items">${[
          ...s.customTypes.map((c) => chip('custom', c.uid, c.name, 'custom')),
          ...s.customChecksums.map((c) => chip('custom', c.uid, c.name + ' (checksum)', 'check')),
          ...s.customFramers.map((c) => chip('custom', c.uid, c.name + ' (framer)', 'frame')),
        ].join('') || '<span class="chip empty">none yet</span>'}</div></div>
    </div>`;
  }

  // ----------------------------------------------------------------------------------------------
  // editors
  // ----------------------------------------------------------------------------------------------
  function head(kind, title, tools = '') {
    return `<div class="editor-head"><div><div class="kind">${esc(kind)}</div><h2>${esc(title || '(unnamed)')}</h2></div><div class="inline">${tools}</div></div>`;
  }

  function schemaEditor(s) {
    const d = s.schema.defaults;
    return head('Schema', 'Settings & defaults') +
      section('General', `<div class="grid">
        ${field('Namespace', input('schema.namespace', s.schema.namespace, { placeholder: 'acme::telemetry', mono: true }), 'C++ namespace', 'span-2')}
      </div>`) +
      section('Defaults', `<div class="grid">
        ${field('Endian', select('schema.defaults.endian', d.endian, ['big', 'little']), 'multi-byte fields')}
        ${field('Bit order', select('schema.defaults.bit_order', d.bit_order, ['msb_first', 'lsb_first']), 'bits groups')}
        ${field('Format', select('schema.defaults.format', d.format, ['binary', 'text']))}
        ${field('Access', select('schema.defaults.access', d.access, [['both', 'view + struct'], ['view', 'view only'], ['struct', 'struct only']]))}
        ${field('Text separator', input('schema.defaults.separator', d.separator, { mono: true }), 'one character')}
        <div class="span-2">${check('schema.defaults.strict_enums', d.strict_enums, 'Strict enums: reject undeclared enum values on decode')}</div>
      </div>
      <p class="callout" style="margin:12px 0 0">Every message and protocol inherits these unless it overrides them. Views are zero-copy accessors over the input buffer; structs are fixed-capacity value types used for encoding.</p>`);
  }

  function enumEditor(s, e, i) {
    const base = `enums.${i}`;
    const rows = e.values.map((v, j) => `<tr>
      <td>${input(`${base}.values.${j}.name`, v.name, { placeholder: 'name', mono: true })}</td>
      <td class="narrow">${input(`${base}.values.${j}.value`, v.value, { placeholder: '0', mono: true })}</td>
      <td style="width:30px">${iconBtn('trash', 'delete', `data-path="${base}.values.${j}"`, 'Remove value', true)}</td></tr>`).join('');
    return head('Enum', e.name, iconBtn('trash', 'deleteItem', `data-kind="enum" data-uid="${e.uid}"`, 'Delete enum', true)) +
      section('Enum', `<div class="grid">
        ${field('Name', input(`${base}.name`, e.name, { rename: 'type', mono: true, rerender: false }))}
        ${field('Underlying type', select(`${base}.type`, e.type, G.INT_PRIMS))}
        ${field('Unknown values', select(`${base}.strict`, e.strict || '', [['', `inherit (${s.schema.defaults.strict_enums ? 'reject' : 'accept'})`], ['true', 'reject on decode (strict)'], ['false', 'accept on decode']]), 'values not listed below')}
        ${field('Doc', input(`${base}.doc`, e.doc, { placeholder: 'Description' }), '', 'span-2')}
      </div>`) +
      section('Values', `<table class="subtable"><thead><tr><th>Name</th><th>Value</th><th></th></tr></thead><tbody>${rows}</tbody></table>
        ${e.values.length ? '' : '<div class="list-empty">No values yet</div>'}`,
        `<button class="btn small" data-action="addEnumValue" data-path="${base}.values">+ Value</button>`);
  }

  /** The C binding (`c:` block) of a custom item; empty until edited. */
  const cl = (c) => (c.lang && c.lang.c) || {};
  /** The JavaScript binding (`js:` block) of a custom item; empty until edited. */
  const jl = (c) => (c.lang && c.lang.js) || {};

  function customEditor(s, uid) {
    let i = s.customTypes.findIndex((c) => c.uid === uid);
    if (i >= 0) {
      const c = s.customTypes[i], b = `customTypes.${i}`;
      return head('Custom field type', c.name, iconBtn('trash', 'deleteItem', `data-kind="custom" data-uid="${uid}"`, 'Delete', true)) +
        section('Type', `<div class="grid">
          ${field('Name', input(`${b}.name`, c.name, { rename: 'type', mono: true }), 'used as a field type')}
          ${field('Wire size', input(`${b}.wire_size`, c.wire_size, { placeholder: 'variable', mono: true }), 'bytes, if fixed')}
          ${field('Max wire size', input(`${b}.max_wire_size`, c.max_wire_size, { placeholder: 'unbounded', mono: true }), 'if variable')}
        </div>`) +
        section('C++ binding', `<div class="grid">
          ${field('Header', input(`${b}.header`, c.header, { placeholder: 'my_codecs.hpp', mono: true }), '', 'span-2')}
          ${field('C++ type', input(`${b}.type`, c.type, { placeholder: 'my::Time', mono: true }), '', 'span-2')}
          ${field('Decode function', input(`${b}.decode`, c.decode, { placeholder: 'my::decode_time', mono: true }))}
          ${field('Encode function', input(`${b}.encode`, c.encode, { placeholder: 'my::encode_time', mono: true }))}
        </div>
        <p class="callout" style="margin:12px 0 0">Binary: <code>gmp::Result&lt;T&gt; decode(gmp::ByteReader&amp;)</code>, <code>gmp::VoidResult encode(gmp::ByteWriter&amp;, const T&amp;)</code>.<br>Text: <code>gmp::Result&lt;T&gt; decode(std::string_view)</code>, <code>gmp::VoidResult encode(gmp::text::TextWriter&amp;, const T&amp;)</code>.</p>`) +
        section('C binding', `<div class="grid">
          ${field('Header', input(`${b}.lang.c.header`, cl(c).header, { placeholder: 'my_codecs.h', mono: true }), '', 'span-2')}
          ${field('C type', input(`${b}.lang.c.type`, cl(c).type, { placeholder: 'my_time', mono: true }), '', 'span-2')}
          ${field('Decode function', input(`${b}.lang.c.decode`, cl(c).decode, { placeholder: 'my_decode_time', mono: true }))}
          ${field('Encode function', input(`${b}.lang.c.encode`, cl(c).encode, { placeholder: 'my_encode_time', mono: true }))}
          ${field('Print function', input(`${b}.lang.c.print`, cl(c).print, { placeholder: 'optional', mono: true }), 'else prints <?>', 'span-2')}
        </div>
        <p class="callout" style="margin:12px 0 0">Only needed for <code>gmpc --lang c</code>; written as a <code>c:</code> block.<br>Binary: <code>gmsg_error decode(gmsg_reader*, T*)</code>, <code>gmsg_error encode(gmsg_writer*, const T*)</code>.<br>Text: <code>gmsg_error decode(const char*, size_t, T*)</code>, <code>gmsg_error encode(gmsg_text_writer*, const T*)</code>.<br>Print: <code>void print(gmsg_printer*, const T*)</code>.</p>`) +
        section('JavaScript binding', `<div class="grid">
          ${field('Module', input(`${b}.lang.js.module`, jl(c).module, { placeholder: './my_codecs.js', mono: true }), 'required as written', 'span-2')}
          ${field('Decode function', input(`${b}.lang.js.decode`, jl(c).decode, { placeholder: 'decodeTime', mono: true }))}
          ${field('Encode function', input(`${b}.lang.js.encode`, jl(c).encode, { placeholder: 'encodeTime', mono: true }))}
          ${field('Print function', input(`${b}.lang.js.print`, jl(c).print, { placeholder: 'optional', mono: true }), 'else numbers / strings print as such', 'span-2')}
        </div>
        <p class="callout" style="margin:12px 0 0">Only needed for <code>gmpc --lang js</code>; written as a <code>js:</code> block. The functions are exports of the module and throw <code>gmsg.GmpError</code> on bad input.<br>Binary: <code>decode(reader) → value</code>, <code>encode(writer, value)</code>. Text: <code>decode(string) → value</code>, <code>encode(textWriter, value)</code>. Print: <code>print(printer, value)</code>.</p>`);
    }
    i = s.customChecksums.findIndex((c) => c.uid === uid);
    if (i >= 0) {
      const c = s.customChecksums[i], b = `customChecksums.${i}`;
      return head('Custom checksum', c.name, iconBtn('trash', 'deleteItem', `data-kind="custom" data-uid="${uid}"`, 'Delete', true)) +
        section('Checksum', `<div class="grid">
          ${field('Name', input(`${b}.name`, c.name, { mono: true }), 'select it as a protocol checksum algo')}
          ${field('Size', input(`${b}.size`, c.size, { mono: true }), 'bytes on the wire')}
          ${field('Header', input(`${b}.header`, c.header, { placeholder: 'my_codecs.hpp', mono: true }))}
          ${field('Function', input(`${b}.function`, c.function, { placeholder: 'my::crc', mono: true }), '<unsigned> fn(gmp::bytes_view)')}
        </div>`) +
        section('C binding', `<div class="grid">
          ${field('Header', input(`${b}.lang.c.header`, cl(c).header, { placeholder: 'my_codecs.h', mono: true }))}
          ${field('Function', input(`${b}.lang.c.function`, cl(c).function, { placeholder: 'my_crc', mono: true }), '<unsigned> fn(const uint8_t*, size_t)')}
        </div>`) +
        section('JavaScript binding', `<div class="grid">
          ${field('Module', input(`${b}.lang.js.module`, jl(c).module, { placeholder: './my_codecs.js', mono: true }))}
          ${field('Function', input(`${b}.lang.js.function`, jl(c).function, { placeholder: 'myCrc', mono: true }), 'fn(Uint8Array) → number | BigInt')}
        </div>`);
    }
    i = s.customFramers.findIndex((c) => c.uid === uid);
    if (i >= 0) {
      const c = s.customFramers[i], b = `customFramers.${i}`;
      return head('Custom framer', c.name, iconBtn('trash', 'deleteItem', `data-kind="custom" data-uid="${uid}"`, 'Delete', true)) +
        section('Framer', `<div class="grid">
          ${field('Name', input(`${b}.name`, c.name, { mono: true }))}
          ${field('Header', input(`${b}.header`, c.header, { placeholder: 'my_codecs.hpp', mono: true }))}
          ${field('C++ type', input(`${b}.type`, c.type, { placeholder: 'my::Framer', mono: true }), 'static gmp::FrameScan scan(gmp::bytes_view)', 'span-2')}
        </div>`) +
        section('C binding', `<div class="grid">
          ${field('Header', input(`${b}.lang.c.header`, cl(c).header, { placeholder: 'my_codecs.h', mono: true }))}
          ${field('Scan function', input(`${b}.lang.c.scan`, cl(c).scan, { placeholder: 'my_scan', mono: true }), 'gmsg_frame_scan fn(const uint8_t*, size_t)')}
        </div>`) +
        section('JavaScript binding', `<div class="grid">
          ${field('Module', input(`${b}.lang.js.module`, jl(c).module, { placeholder: './my_codecs.js', mono: true }))}
          ${field('Scan function', input(`${b}.lang.js.scan`, jl(c).scan, { placeholder: 'myScan', mono: true }), 'fn(Uint8Array) → gmsg.FrameScan.*')}
        </div>`);
    }
    return '';
  }

  // ---- fields -------------------------------------------------------------------------------------
  function fieldList(s, ui, owner, basePath, title) {
    const text = !owner.isHeader && G.msgFormat(s, owner) === 'text';
    const L = G.messageLayout(s, owner);
    const rows = owner.fields.map((f, j) => {
      const path = `${basePath}.${j}`;
      const kind = G.fieldKind(s, f);
      const open = ui.field === f.uid;
      const sz = L.rows[j];
      const sizeText = text ? `#${sz.token}` : G.sizeLabel(sz.size);
      const name = kind === 'pad' ? '<span class="anon">padding</span>' : f.name ? esc(f.name) : '<span class="anon">unnamed</span>';
      return `<div class="frow${open ? ' open' : ''}" data-field-uid="${f.uid}">
        <div class="frow-head" data-action="toggleField" data-uid="${f.uid}">
          <span class="swatch" style="background:var(--k-${G.KIND_COLOR[kind]}-b)"></span>
          <span class="frow-name" data-mirror="${path}.name">${name}</span>
          <span class="frow-type">${esc(G.typeLabel(s, f))}${f.when ? ' ?' : ''}${f.optional && text ? ' ?' : ''}</span>
          <span class="frow-size">${sizeText}</span>
          <span class="frow-tools">
            ${iconBtn('up', 'move', `data-path="${path}" data-dir="-1"`, 'Move up')}
            ${iconBtn('down', 'move', `data-path="${path}" data-dir="1"`, 'Move down')}
            ${iconBtn('copy', 'duplicate', `data-path="${path}"`, 'Duplicate')}
            ${iconBtn('trash', 'delete', `data-path="${path}"`, 'Delete field', true)}
          </span>
        </div>
        ${open ? `<div class="frow-body">${fieldBody(s, owner, f, path, text)}</div>` : ''}
      </div>`;
    }).join('');
    return section(title, `<div class="field-list">${rows || '<div class="list-empty">No fields yet. Add one below.</div>'}</div>
      <div class="list-foot">
        <button class="btn small" data-action="addField" data-path="${basePath}">+ Field</button>
        ${text ? '' : `<button class="btn small" data-action="addField" data-path="${basePath}" data-preset="bits">+ Bits group</button>`}
        <button class="btn small" data-action="addField" data-path="${basePath}" data-preset="pad">+ Padding</button>
      </div>`, `<span class="badge">${owner.fields.length} field${owner.fields.length === 1 ? '' : 's'}</span>`, true);
  }

  function lengthControls(s, f, path, kind) {
    const isArr = kind === 'array';
    const modes = [['fixed', 'Fixed'], ['expr', 'Field / expression'], ['remaining', 'Remaining']];
    if (!isArr) modes.push(['terminator', 'Terminator byte']);
    let valueCtl = '';
    if (f.lenMode === 'fixed') valueCtl = field(isArr ? 'Count' : 'Length (bytes)', input(`${path}.lenValue`, f.lenValue, { placeholder: '8', mono: true }));
    else if (f.lenMode === 'expr') valueCtl = field(isArr ? 'Count from' : 'Length from', input(`${path}.lenValue`, f.lenValue, { placeholder: 'len_field or n * 2', mono: true }), 'earlier field');
    else if (f.lenMode === 'terminator') valueCtl = field('Terminator', input(`${path}.terminator`, f.terminator, { placeholder: '0', mono: true }), 'byte value');
    else valueCtl = `<div class="callout" style="align-self:end">Consumes the rest of the message (last field only).</div>`;
    const variable = f.lenMode !== 'fixed';
    return `${field(isArr ? 'Count mode' : 'Length mode', select(`${path}.lenMode`, f.lenMode, modes))}
      ${valueCtl}
      ${variable ? field('Max', input(`${path}.max`, f.max, { placeholder: 'capacity', mono: true }), 'struct capacity') : ''}`;
  }

  function fieldBody(s, owner, f, path, text) {
    const kind = G.fieldKind(s, f);
    const parts = [];
    if (kind !== 'pad') parts.push(field('Name', input(`${path}.name`, f.name, { mono: true, mirror: true })));
    parts.push(field('Type', select(`${path}.type`, f.type, typeOptions(s, owner, text))));
    if (kind === 'unknown') parts.push(`<div class="callout span-all">Unknown type <b>${esc(f.type)}</b>. Pick a type, or create a message / enum / custom type with this name.</div>`);

    if (kind === 'pad') {
      parts.push(field(text ? 'Tokens to skip' : 'Bytes', input(`${path}.padSize`, f.padSize, { mono: true })));
      parts.push(field('Doc', input(`${path}.doc`, f.doc, { placeholder: 'Reserved' }), '', 'span-2'));
      return `<div class="grid">${parts.join('')}</div>`;
    }

    const multi = ['int', 'float', 'enum'].includes(kind) && !text && (G.fieldSize(s, f).fixed || G.fieldSize(s, f).present || 0) > 1;
    if (multi || (kind === 'array' && !text)) {
      const ownerEndian = owner.isHeader ? owner.endian : G.msgEndian(s, owner);
      parts.push(field('Endian', select(`${path}.endian`, f.endian, inherit(['big', 'little'], ownerEndian))));
    }
    if (kind === 'bits') parts.push(field('Bit order', select(`${path}.bit_order`, f.bit_order, inherit(['msb_first', 'lsb_first'], owner.isHeader ? s.schema.defaults.bit_order : G.msgBitOrder(s, owner)))));
    if (['bytes', 'string', 'array'].includes(kind) && !text) parts.push(lengthControls(s, f, path, kind));
    if (kind === 'array') {
      parts.push(field('Element type', select(`${path}.of`, f.of, elemOptions(s, owner))));
      const ek = G.fieldKind(s, { type: f.of });
      if (['int', 'float', 'enum'].includes(ek)) parts.push(field('Element endian', select(`${path}.ofEndian`, f.ofEndian, inherit(['big', 'little'], 'field'))));
    }
    if (kind === 'string' && text) parts.push(field('Max length', input(`${path}.max`, f.max, { placeholder: '32', mono: true }), 'struct capacity'));
    if (kind === 'string' && !text && f.lenMode === 'fixed') parts.push(field('Padding char', input(`${path}.padding`, f.padding, { placeholder: '\\0', mono: true }), 'stripped on decode'));
    if (['int', 'enum', 'bool', 'char'].includes(kind)) parts.push(field('Const', input(`${path}.const`, f.const, { placeholder: 'none', mono: true }), 'magic value'));
    if (!text && ['int', 'float', 'enum', 'bool', 'char'].includes(kind)) {
      const algos = [['', 'none'],
        { group: 'CRC', items: Object.keys(G.CHECKSUMS).filter((k) => k.startsWith('crc')) },
        { group: 'Sums', items: Object.keys(G.CHECKSUMS).filter((k) => !k.startsWith('crc')) },
        { group: 'Other', items: [['__crc', 'custom CRC parameters…'], ...s.customChecksums.map((c) => [c.name, c.name + ' (custom)'])] }];
      parts.push(field('CRC', select(`${path}.crc`, f.crc, algos), 'checksum stored right after the value'));
      if (f.crc === '__crc') {
        const c = `${path}.crcParams`, cp = f.crcParams;
        parts.push(`<div class="span-2"><div class="grid three">
          ${field('Width', input(`${c}.width`, cp.width, { mono: true }), 'bits')}
          ${field('Poly', input(`${c}.poly`, cp.poly, { mono: true }))}
          ${field('Init', input(`${c}.init`, cp.init, { mono: true }))}
          ${field('XorOut', input(`${c}.xorout`, cp.xorout, { mono: true }))}
          <div style="align-self:end">${check(`${c}.refin`, cp.refin, 'RefIn')}</div>
          <div style="align-self:end">${check(`${c}.refout`, cp.refout, 'RefOut')}</div>
        </div></div>`);
      }
    }
    if (['int', 'float'].includes(kind)) {
      parts.push(field('Scale', input(`${path}.scale`, f.scale, { placeholder: '1', mono: true }), 'value = raw × scale + offset'));
      parts.push(field('Offset', input(`${path}.offset`, f.offset, { placeholder: '0', mono: true })));
    }
    if (text && ['int', 'enum'].includes(kind)) parts.push(field('Base', select(`${path}.base`, f.base, [['', '10 (default)'], ['16', '16 (hex)']])));
    if (text && ['int', 'float', 'enum'].includes(kind)) parts.push(field('Width', input(`${path}.width`, f.width, { placeholder: '0', mono: true }), 'zero padded on encode'));
    if (text && kind === 'float') parts.push(field('Precision', input(`${path}.precision`, f.precision, { placeholder: 'shortest', mono: true }), 'decimals on encode'));
    parts.push(field('Unit', input(`${path}.unit`, f.unit, { placeholder: 'V, ms, degC…' })));
    if (!text && !owner.isHeader) parts.push(field('Present when', input(`${path}.when`, f.when, { placeholder: 'e.g. flags.mode == Mode.fault', mono: true }), 'optional condition', 'span-2'));
    if (text) parts.push(`<div class="span-2">${check(`${path}.optional`, f.optional, 'Optional (empty token allowed)')}</div>`);
    parts.push(field('Doc', input(`${path}.doc`, f.doc, { placeholder: 'Description' }), '', 'span-2'));
    let html = `<div class="grid">${parts.join('')}</div>`;
    if (kind === 'bits') html += bitsEditor(s, f, path);
    if (['int', 'float'].includes(kind)) html += conversionsEditor(f, path);
    return html;
  }

  /** Extra named conversions: each becomes a <field>_<name>() accessor. */
  function conversionsEditor(f, path) {
    const list = f.conversions || [];
    const rows = list.map((c, k) => {
      const cp = `${path}.conversions.${k}`;
      return `<tr>
        <td>${input(`${cp}.name`, c.name, { placeholder: 'name', mono: true })}</td>
        <td>${input(`${cp}.scale`, c.scale, { placeholder: '1', mono: true })}</td>
        <td>${input(`${cp}.offset`, c.offset, { placeholder: '0', mono: true })}</td>
        <td>${input(`${cp}.unit`, c.unit, { placeholder: 'unit' })}</td>
        <td style="width:32px">${iconBtn('trash', 'delete', `data-path="${cp}"`, 'Remove', true)}</td>
      </tr>`;
    }).join('');
    return `<div style="margin-top:12px">
      <div class="inline" style="justify-content:space-between;margin-bottom:6px">
        <span class="outline-title">Extra conversions${list.length ? '' : ' <span style="color:var(--faint);font-weight:400">· e.g. °F next to °C</span>'}</span>
        <button class="btn small" data-action="addConversion" data-path="${path}.conversions">+ Conversion</button>
      </div>
      ${list.length ? `<table class="subtable"><thead><tr><th>Name</th><th>Scale</th><th>Offset</th><th>Unit</th><th></th></tr></thead><tbody>${rows}</tbody></table>
      <div style="margin-top:4px;color:var(--faint);font-size:11.5px">Each adds <code>${esc(f.name || 'field')}_&lt;name&gt;()</code> = raw × scale + offset.</div>` : ''}
    </div>`;
  }

  function bitsEditor(s, f, path) {
    const total = G.bitsTotal(f);
    const rows = f.bits.map((b, k) => {
      const bp = `${path}.bits.${k}`;
      if (b.pad) {
        return `<tr><td><span class="badge">padding</span></td><td class="narrow">${input(`${bp}.bits`, b.bits, { mono: true })}</td><td colspan="3"></td>
          <td style="width:84px">${iconBtn('up', 'move', `data-path="${bp}" data-dir="-1"`, 'Up')}${iconBtn('down', 'move', `data-path="${bp}" data-dir="1"`, 'Down')}${iconBtn('trash', 'delete', `data-path="${bp}"`, 'Remove', true)}</td></tr>`;
      }
      return `<tr>
        <td>${input(`${bp}.name`, b.name, { placeholder: 'name', mono: true })}</td>
        <td class="narrow">${input(`${bp}.bits`, b.bits, { mono: true })}</td>
        <td>${select(`${bp}.type`, b.type, bitTypeOptions(s))}</td>
        <td class="narrow">${input(`${bp}.const`, b.const, { placeholder: 'const', mono: true })}</td>
        <td class="narrow">${input(`${bp}.scale`, b.scale, { placeholder: 'scale', mono: true })}</td>
        <td style="width:84px">${iconBtn('up', 'move', `data-path="${bp}" data-dir="-1"`, 'Up')}${iconBtn('down', 'move', `data-path="${bp}" data-dir="1"`, 'Down')}${iconBtn('trash', 'delete', `data-path="${bp}"`, 'Remove', true)}</td>
      </tr>`;
    }).join('');
    const rem = total % 8 ? 8 - (total % 8) : 0;
    return `<div style="margin-top:12px">
      <div class="inline" style="justify-content:space-between;margin-bottom:6px">
        <span class="outline-title">Bit fields · ${total} bits${rem ? ` <span style="color:var(--danger)">(${rem} short of a byte)</span>` : ` = ${total / 8} byte${total / 8 === 1 ? '' : 's'}`}</span>
        <span class="inline"><button class="btn small" data-action="addBit" data-path="${path}.bits">+ Bit field</button><button class="btn small" data-action="addBit" data-path="${path}.bits" data-pad="1">+ Pad</button>${rem ? `<button class="btn small" data-action="addBit" data-path="${path}.bits" data-pad="${rem}">Fill ${rem}</button>` : ''}</span>
      </div>
      <table class="subtable"><thead><tr><th>Name</th><th>Bits</th><th>Type</th><th>Const</th><th>Scale</th><th></th></tr></thead><tbody>${rows}</tbody></table>
    </div>`;
  }

  // ---- message --------------------------------------------------------------------------------------
  function messageEditor(s, ui, m, i) {
    const b = `messages.${i}`;
    const fmt = G.msgFormat(s, m);
    const d = s.schema.defaults;
    return head(`${fmt} message`, m.name,
      iconBtn('copy', 'duplicateItem', `data-kind="message" data-uid="${m.uid}"`, 'Duplicate message') +
      iconBtn('trash', 'deleteItem', `data-kind="message" data-uid="${m.uid}"`, 'Delete message', true)) +
      section('Message', `<div class="grid">
        ${field('Name', input(`${b}.name`, m.name, { rename: 'type', mono: true }))}
        ${field('Type id', input(`${b}.type_id`, m.type_id, { placeholder: fmt === 'text' ? 'GPGGA' : '0x01 or enum value', mono: true }), 'type_id: matched by message_type')}
        ${field('Format', select(`${b}.format`, m.format, inherit(['binary', 'text'], d.format)))}
        ${field('Access', select(`${b}.access`, m.access, inherit([['both', 'view + struct'], ['view', 'view only'], ['struct', 'struct only']], d.access)))}
        ${fmt === 'binary' ? field('Endian', select(`${b}.endian`, m.endian, inherit(['big', 'little'], d.endian))) : field('Separator', input(`${b}.separator`, m.separator, { placeholder: d.separator, mono: true }))}
        ${fmt === 'binary' ? field('Bit order', select(`${b}.bit_order`, m.bit_order, inherit(['msb_first', 'lsb_first'], d.bit_order))) : ''}
        ${field('Doc', input(`${b}.doc`, m.doc, { placeholder: 'Description' }), '', 'span-2')}
      </div>`) +
      fieldList(s, ui, m, `${b}.fields`, 'Fields') +
      (fmt === 'binary' ? computedEditor(m, b) : '');
  }

  /** Values computed from the fields: name() on struct and view, set_name() on the struct when it can be split. */
  function computedEditor(m, b) {
    const list = m.computed || [];
    const types = ['u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64', 'f32', 'f64'];
    const rows = list.map((c, k) => {
      const cp = `${b}.computed.${k}`;
      return `<div class="computed-item" style="border:1px solid var(--border);border-radius:8px;padding:10px;margin-bottom:8px">
        <div class="grid">
          ${field('Name', input(`${cp}.name`, c.name, { placeholder: 'serial', mono: true }), 'accessor name')}
          ${field('Type', select(`${cp}.type`, c.type || 'u32', types))}
          ${field('Value', input(`${cp}.value`, c.value, { placeholder: '(serial_high << 16) | serial_low', mono: true }), 'expression over fields', 'span-2')}
          ${field('Setter', input(`${cp}.set`, c.set, { placeholder: 'automatic for (a << 8) | b packing', mono: true }), 'optional: field = expr over value; …', 'span-2')}
          ${field('Unit', input(`${cp}.unit`, c.unit, { placeholder: 'optional' }))}
          <div style="align-self:end;justify-self:end">${iconBtn('trash', 'delete', `data-path="${cp}"`, 'Remove computed value', true)}</div>
        </div>
      </div>`;
    }).join('');
    return section('Computed values', `${rows || '<div class="list-empty">None. A computed value is derived from fields, e.g. a serial number split across two words.</div>'}`,
      `<button class="btn small" data-action="addComputed" data-path="${b}.computed">+ Computed</button>`);
  }

  // ---- protocol -------------------------------------------------------------------------------------
  function patternControl(label, modePath, mode, textPath, text, hint) {
    return field(label, `<div class="inline">
        <select data-path="${modePath}" data-rerender="1" style="width:92px"><option value="hex"${mode === 'hex' ? ' selected' : ''}>hex</option><option value="text"${mode === 'text' ? ' selected' : ''}>text</option></select>
        ${input(textPath, text, { placeholder: mode === 'hex' ? 'AA 55' : '$  or  \\r\\n', mono: true })}
      </div>`, hint);
  }

  function protocolEditor(s, ui, p, i) {
    const b = `protocols.${i}`;
    const fmt = G.protoFormat(s, p);
    const d = s.schema.defaults;
    const fr = p.framing, ck = p.checksum, mt = p.message_type;
    const L = G.protocolLayout(s, p);
    const headerNames = p.header.filter((f) => f.type !== 'pad').map((f) => f.name);

    const general = section('Protocol', `<div class="grid">
      ${field('Name', input(`${b}.name`, p.name, { rename: 'proto', mono: true }))}
      ${field('Format', select(`${b}.format`, p.format, inherit(['binary', 'text'], d.format)))}
      ${field('Endian', select(`${b}.endian`, p.endian, inherit(['big', 'little'], d.endian)), 'header & checksum')}
      ${fmt === 'text' ? field('Separator', input(`${b}.separator`, p.separator, { placeholder: d.separator, mono: true })) : '<div></div>'}
      ${field('Doc', input(`${b}.doc`, p.doc, { placeholder: 'Description' }), '', 'span-2')}
    </div>`);

    const header = fmt === 'binary' ? fieldList(s, ui, G.headerMessage(s, p), `${b}.header`, 'Header fields') : '';

    // framing
    const ft = fr.type;
    const fparts = [field('Type', select(`${b}.framing.type`, ft, [
      ['none', 'none - whole buffer is a frame'], ['fixed', 'fixed size'], ['length_prefixed', 'length prefixed'],
      ['delimited', 'delimited (start / end)'], ['custom', 'custom framer']]), '', 'span-2')];
    if (ft === 'fixed' || ft === 'length_prefixed') {
      fparts.push(patternControl('Sync bytes', `${b}.framing.syncMode`, fr.syncMode, `${b}.framing.sync`, fr.sync, 'optional'));
      fparts.push(patternControl('End', `${b}.framing.endMode`, fr.endMode, `${b}.framing.end`, fr.end, 'optional'));
    }
    if (ft === 'delimited') {
      fparts.push(patternControl('Start', `${b}.framing.syncMode`, fr.syncMode, `${b}.framing.sync`, fr.sync, 'optional'));
      fparts.push(patternControl('End', `${b}.framing.endMode`, fr.endMode, `${b}.framing.end`, fr.end, 'required'));
    }
    if (ft === 'fixed') fparts.push(field('Frame size', input(`${b}.framing.size`, fr.size, { placeholder: 'derive', mono: true }), 'bytes'));
    if (ft === 'length_prefixed') {
      fparts.push(field('Length field', select(`${b}.framing.lengthField`, fr.lengthField, [['', '— header field —'], ...headerNames])));
      fparts.push(field('Length covers', select(`${b}.framing.covers`, fr.covers, [['body', 'body'], ['body_checksum', 'body + checksum'], ['header_body', 'header + body'], ['all', 'whole frame']])));
      if (fr.escape.enabled && fmt === 'binary') {
        fparts.push(field('Length counts', select(`${b}.framing.counts`, fr.counts || '', [['', 'unescaped bytes'], ['wire', 'wire bytes (after escaping)']]),
          fr.counts === 'wire' ? 'body (+ checksum) only' : ''));
      }
      fparts.push(field('Adjust', input(`${b}.framing.adjust`, fr.adjust, { placeholder: '0', mono: true }), 'covered = value + adjust'));
    }
    if (ft === 'custom') fparts.push(field('Framer', select(`${b}.framing.framer`, fr.framer, [['', '— select —'], ...s.customFramers.map((c) => c.name)])));
    if (ft !== 'none') fparts.push(field('Max frame', input(`${b}.framing.maxFrame`, fr.maxFrame, { placeholder: L.maxFrame != null ? `auto (${L.maxFrame})` : 'required', mono: true }), 'bytes'));
    if (['delimited', 'fixed', 'length_prefixed'].includes(ft) && fmt === 'binary') {
      fparts.push(`<div class="span-2">${check(`${b}.framing.escape.enabled`, fr.escape.enabled, 'Byte stuffing (reserved bytes never appear inside a frame)')}</div>`);
      if (fr.escape.enabled) {
        const et = G.escapeTable(s, p);
        fparts.push(field('Style', select(`${b}.framing.escape.style`, fr.escape.style, [['xor', 'XOR (HDLC / PPP)'], ['index', 'Index (escape + position)'], ['map', 'Map (SLIP)']])));
        fparts.push(field('Escape byte', input(`${b}.framing.escape.char`, fr.escape.char, { mono: true })));
        if (fr.escape.style === 'map') {
          fparts.push(field('Map', input(`${b}.framing.escape.map`, fr.escape.map, { mono: true, placeholder: '0xC0:0xDC, 0xDB:0xDD' }), 'raw:code pairs', 'span-2'));
        } else {
          const def = et ? et.reserved.map(G.hex2).join(', ') : '';
          fparts.push(field('Reserved bytes', input(`${b}.framing.escape.reserved`, fr.escape.reserved, { mono: true, placeholder: `default: ${def}` }),
            fr.escape.style === 'index' ? 'in order; include the escape byte' : 'default: sync / delimiter bytes', 'span-2'));
          if (fr.escape.style === 'xor') fparts.push(field('XOR mask', input(`${b}.framing.escape.xor`, fr.escape.xor, { mono: true })));
          else fparts.push(field('Base', input(`${b}.framing.escape.base`, fr.escape.base, { mono: true, placeholder: '0x00' }), 'code of the first reserved byte'));
        }
        if (et && !et.errors.length && et.table.length) {
          fparts.push(`<div class="span-2 hint mono">${et.table.map(([r, c]) => `${G.hex2(r)} → ${G.hex2(et.esc)} ${G.hex2(c)}`).join(' · ')}</div>`);
        }
      }
    }
    const framing = section('Framing', `<div class="grid">${fparts.join('')}</div>`);

    // checksum
    const algos = [{ group: 'CRC', items: Object.keys(G.CHECKSUMS).filter((k) => k.startsWith('crc')) },
      { group: 'Sums', items: Object.keys(G.CHECKSUMS).filter((k) => !k.startsWith('crc')) },
      { group: 'Other', items: [['__crc', 'custom CRC parameters…'], ...s.customChecksums.map((c) => [c.name, c.name + ' (custom)'])] }];
    let cparts = [`<div class="span-2">${check(`${b}.checksum.enabled`, ck.enabled, 'Validate frames with a checksum')}</div>`];
    if (ck.enabled) {
      cparts.push(field('Algorithm', select(`${b}.checksum.algo`, ck.algo, algos)));
      if (fmt === 'binary') {
        cparts.push(field('Covers', select(`${b}.checksum.covers`, ck.covers, [['header_body', 'header + body'], ['body', 'body'], ['all', 'prefix + header + body']])));
        cparts.push(field('Byte order', select(`${b}.checksum.endian`, ck.endian, inherit(['big', 'little'], G.protoEndian(s, p)))));
        if (fr.escape.enabled) cparts.push(field('Computed over', select(`${b}.checksum.over`, ck.over || '', [['', 'unescaped bytes'], ['wire', 'escaped (wire) bytes']])));
      } else {
        cparts.push(field('Separator', input(`${b}.checksum.separator`, ck.separator, { mono: true }), 'before hex digits'));
        cparts.push(`<div class="span-2">${check(`${b}.checksum.required`, ck.required, 'Reject sentences without a checksum')}</div>`);
      }
      if (ck.algo === '__crc') {
        const c = `${b}.checksum.crc`;
        cparts.push(`<div class="span-2"><div class="grid three">
          ${field('Width', input(`${c}.width`, ck.crc.width, { mono: true }), 'bits')}
          ${field('Poly', input(`${c}.poly`, ck.crc.poly, { mono: true }))}
          ${field('Init', input(`${c}.init`, ck.crc.init, { mono: true }))}
          ${field('XorOut', input(`${c}.xorout`, ck.crc.xorout, { mono: true }))}
          <div style="align-self:end">${check(`${c}.refin`, ck.crc.refin, 'RefIn')}</div>
          <div style="align-self:end">${check(`${c}.refout`, ck.crc.refout, 'RefOut')}</div>
        </div></div>`);
      }
    }
    const checksum = section('Checksum', `<div class="grid">${cparts.join('')}</div>`);

    // message_type
    const kinds = fmt === 'text' ? [['none', 'none (single message)'], ['token', 'by first token']] : [['none', 'none (single message)'], ['field', 'by header field']];
    const mtParts = [field('Message type', select(`${b}.message_type.kind`, mt.kind, kinds))];
    if (mt.kind === 'field') mtParts.push(field('Header field', select(`${b}.message_type.field`, mt.field, [['', '— header field —'], ...headerNames])));
    if (mt.kind === 'token') mtParts.push(`<div class="callout" style="align-self:end">The first token (e.g. <code>GPGGA</code>) selects the message by its <code>type_id</code>.</div>`);
    const messageType = section('Message type', `<div class="grid">${mtParts.join('')}</div>`);

    // messages
    const cands = s.messages.filter((m) => G.msgFormat(s, m) === fmt);
    const others = p.messages.filter((n) => !cands.some((m) => m.name === n));
    const picks = cands.map((m) => {
      const on = p.messages.includes(m.name);
      return `<button class="chip${on ? ' active' : ''}" data-action="toggleProtoMsg" data-path="${b}" data-name="${esc(m.name)}">${on ? '✓ ' : ''}${esc(m.name)}${m.type_id ? ` <span class="mono" style="color:var(--faint)">${esc(m.type_id)}</span>` : ''}</button>`;
    }).join('');
    const messages = section('Messages carried', `<div class="msg-pick">${picks || `<span class="list-empty">No ${fmt} messages defined yet.</span>`}</div>
      ${others.length ? `<p class="callout" style="margin:10px 0 0">Not found / wrong format: ${others.map(esc).join(', ')}</p>` : ''}`);

    return head(`${fmt} protocol`, p.name,
      iconBtn('copy', 'duplicateItem', `data-kind="protocol" data-uid="${p.uid}"`, 'Duplicate protocol') +
      iconBtn('trash', 'deleteItem', `data-kind="protocol" data-uid="${p.uid}"`, 'Delete protocol', true)) +
      general + messages + header + framing + checksum + messageType;
  }

  // ----------------------------------------------------------------------------------------------
  G.renderEditor = (s, ui, issues) => {
    let body = '';
    const k = ui.sel.kind;
    if (k === 'schema') body = schemaEditor(s);
    else if (k === 'enum') { const i = s.enums.findIndex((e) => e.uid === ui.sel.uid); if (i >= 0) body = enumEditor(s, s.enums[i], i); }
    else if (k === 'message') { const i = s.messages.findIndex((m) => m.uid === ui.sel.uid); if (i >= 0) body = messageEditor(s, ui, s.messages[i], i); }
    else if (k === 'protocol') { const i = s.protocols.findIndex((p) => p.uid === ui.sel.uid); if (i >= 0) body = protocolEditor(s, ui, s.protocols[i], i); }
    else if (k === 'custom') body = customEditor(s, ui.sel.uid);
    if (!body) body = schemaEditor(s);
    return outline(s, ui, issues) + `<div class="editor">${body}</div>`;
  };
})(window.GMP);
