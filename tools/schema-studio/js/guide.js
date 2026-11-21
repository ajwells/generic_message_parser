// guide.js - the visual guide (guide.html): every GMP feature with live, decoded examples.
// Examples are real schemas: they are parsed, laid out and decoded by the same code as the Studio.
(function (G) {
  'use strict';
  const esc = G.esc;
  const D = G.decoder;

  // ----------------------------------------------------------------------------------------------
  // example schemas
  // ----------------------------------------------------------------------------------------------
  const Y = {
    scalars: `schema_version: 1
namespace: guide
messages:
  Reading:
    fields:
      - { name: id, type: u8 }
      - { name: counter, type: u16 }                    # big endian (the default)
      - { name: counter_le, type: u16, endian: little } # same value, bytes swapped
      - { name: delta, type: i16 }                      # two's complement
      - { name: ratio, type: f32 }                      # IEEE-754
`,
    enums: `schema_version: 1
namespace: guide
enums:
  Mode: { type: u8, strict: true, values: { idle: 0, run: 1, fault: 2 } }
  Gear: { type: u8, values: { park: 0, reverse: 1, neutral: 2, drive: 3 } }
messages:
  Status:
    fields:
      - { name: mode, type: Mode }
      - name: flags
        type: bits                        # 8 bits packed msb_first
        fields:
          - { name: armed, bits: 1 }
          - { name: gear, bits: 2, enum: Gear }
          - { name: level, bits: 4 }
          - { pad: 1 }
`,
    containers: `schema_version: 1
namespace: guide
messages:
  Packet:
    fields:
      - { name: tag, type: string, length: 4 }                    # fixed length
      - { name: name_len, type: u8 }
      - { name: name, type: string, length: name_len, max: 16 }   # length from a field
      - { name: count, type: u8 }
      - { name: samples, type: array, of: i16, count: count, max: 8 }
      - { name: note, type: string, terminator: 0, max: 16 }      # C string
      - { name: rest, type: bytes, length: remaining, max: 32 }   # everything left
`,
    structure: `schema_version: 1
namespace: guide
messages:
  Fault:
    fields:
      - { name: code, type: u16 }
      - { name: temperature, type: i16, scale: 0.1, unit: degC }
  Report:
    fields:
      - { name: magic, type: u8, const: 0x5A }          # checked on decode
      - { name: state, type: u8 }
      - { pad: 2 }                                       # reserved bytes
      - { name: fault, type: Fault, when: "state == 2" } # nested + conditional
`,
    values: `schema_version: 1
namespace: guide
defaults: { endian: big }
messages:
  Measurement:
    fields:
      - name: temperature
        type: u16
        crc: crc8_nrsc5                 # CRC-8 stored after the word
        scale: 0.0026703288319218739    # 175 / 65535
        offset: -45
        unit: degC
        conversions:
          degf: { scale: 0.0048065918974593731, offset: -49, unit: degF }
      - { name: humidity, type: u16, crc: crc8_nrsc5, scale: 0.0019073777370870527, offset: -6, unit: "%RH" }
  SerialNumber:
    fields:
      - { name: serial_high, type: u16, crc: crc8_nrsc5 }
      - { name: serial_low, type: u16, crc: crc8_nrsc5 }
    computed:
      serial: { type: u32, value: "(serial_high << 16) | serial_low" }
protocols:
  MeasurementResponse:
    framing: { type: fixed, size: 6 }
    messages: [Measurement]
`,
    framing: `schema_version: 1
namespace: guide
messages:
  Ping:
    type_id: 1
    fields:
      - { name: seq, type: u16 }
  Note:
    type_id: 2
    fields:
      - { name: len, type: u8 }
      - { name: text, type: string, length: len, max: 32 }
protocols:
  Datagram:                             # the buffer is the frame
    framing: { type: none }
    messages: [Ping]
  FixedFrames:                          # A5 | seq | crc8
    framing: { type: fixed, sync: [0xA5], size: 4 }
    checksum: { algo: crc8 }
    messages: [Ping]
  LengthFrames:                         # AA 55 | kind len | body | crc16
    header:
      - { name: kind, type: u8 }
      - { name: len, type: u8 }
    framing:
      type: length_prefixed
      sync: [0xAA, 0x55]
      length: { field: len, covers: body }
    checksum: { algo: crc16_ccitt }
    message_type: { field: kind }
    messages: [Ping, Note]
  StxEtx:                               # 02 | kind | body | 03, DLE escaping
    header:
      - { name: kind, type: u8 }
    framing:
      type: delimited
      start: [0x02]
      end: [0x03]
      escape: { char: 0x10, xor: 0x20 }
    message_type: { field: kind }
    messages: [Ping, Note]
`,
    escaping: `schema_version: 1
namespace: guide
messages:
  Ping:
    type_id: 2
    fields:
      - { name: seq, type: u32 }
protocols:
  SyncLink:                             # 0x80 only ever starts a frame
    header:
      - { name: kind, type: u8 }
      - { name: len, type: u8 }
    framing:
      type: length_prefixed
      sync: [0x80]
      length: { field: len, covers: body }
      escape: { char: 0x81, code: index, reserved: [0x80, 0x81] }
    checksum: { algo: crc16_ccitt }
    message_type: { field: kind }
    messages: [Ping]
`,
    nmea: `schema_version: 1
namespace: nav::nmea
defaults: { format: text, separator: "," }

enums:
  FixQuality:
    type: u8
    values: { invalid: 0, gps: 1, dgps: 2 }

messages:
  GGA:
    type_id: GPGGA
    doc: Global positioning system fix data.
    fields:
      - { name: utc_time, type: f64, precision: 2, width: 9 }
      - { name: latitude, type: f64, precision: 4, width: 9, optional: true }
      - { name: lat_hemi, type: char, optional: true }
      - { name: longitude, type: f64, precision: 4, width: 10, optional: true }
      - { name: lon_hemi, type: char, optional: true }
      - { name: quality, type: FixQuality }
      - { name: satellites, type: u8, width: 2 }
      - { name: hdop, type: f32, precision: 1, optional: true }
      - { name: altitude, type: f64, precision: 1, optional: true }
      - { name: altitude_unit, type: char, optional: true }
      - { pad: 4 }
  TXT:
    type_id: GPTXT
    fields:
      - { name: total, type: u8, width: 2 }
      - { name: number, type: u8, width: 2 }
      - { name: severity, type: u8, width: 2 }
      - { name: text, type: string, max: 61 }

protocols:
  Nmea:
    framing: { type: delimited, start: "$", end: "\\r\\n", max_frame: 82 }
    checksum: { algo: xor8, separator: "*" }
    message_type: { token: 0 }
    messages: [GGA, TXT]
`,
  };
  G.GUIDE_SCHEMAS = Y;

  // ----------------------------------------------------------------------------------------------
  // helpers
  // ----------------------------------------------------------------------------------------------
  const hexLine = (a) => a.map((b) => b.toString(16).toUpperCase().padStart(2, '0')).join(' ');
  const putUint = (out, v, n, endian) => {
    const b = [];
    for (let i = 0; i < n; i++) b.push(Number((BigInt(v) >> BigInt(8 * i)) & 0xFFn));
    out.push(...(endian === 'big' ? b.reverse() : b));
  };
  const str = (t) => Array.from(t, (c) => c.charCodeAt(0));

  /** Build a frame of protocol `pname` around message body bytes (header, length, checksum, escaping). */
  function buildFrame(s, pname, mname, body) {
    const p = s.protocols.find((x) => x.name === pname);
    const m = G.findMessage(s, mname);
    const L = G.protocolLayout(s, p);
    const fr = p.framing, ck = p.checksum;
    body = body.slice();
    if (fr.type === 'fixed') {
      const size = G.parseInt(fr.size) || L.overhead + body.length;
      while (body.length < size - L.overhead) body.push(0);
    }
    const hdr = [];
    for (const f of p.header) {
      const n = G.PRIM_SIZE[f.type] || 1;
      let v = 0;
      if (G.isInt(f.const)) v = G.parseInt(f.const);
      else if (p.message_type.kind === 'field' && f.name === p.message_type.field) v = G.parseInt(m.type_id) || 0;
      else if (fr.type === 'length_prefixed' && f.name === fr.lengthField) {
        const c = { header_body: L.headerSize + body.length, all: L.prefix.length + L.headerSize + body.length + L.trailer, body_checksum: body.length + L.trailer }[fr.covers] ?? body.length;
        v = c - (G.parseInt(fr.adjust) || 0);
      }
      putUint(hdr, v, n, f.endian || G.protoEndian(s, p));
    }
    let interior = [...hdr, ...body];
    if (ck.enabled && L.trailer) {
      const covered = ck.covers === 'body' ? body : ck.covers === 'all' ? [...L.prefix, ...interior] : interior;
      const sum = D.checksum(s, ck, Uint8Array.from(covered));
      putUint(interior, sum, L.trailer, ck.endian || G.protoEndian(s, p));
    }
    const et = fr.escape && fr.escape.enabled ? G.escapeTable(s, p) : null;
    if (et) interior = interior.flatMap((b) => (et.isReserved(b) ? [et.esc, et.encode(b)] : [b]));
    return [...L.prefix, ...interior, ...L.suffix];
  }

  /** Very small C++ highlighter for code samples. */
  function highlightCpp(code) {
    return esc(code)
      .replace(/(\/\/[^\n]*)/g, '<span class="y-com">$1</span>')
      .replace(/(&quot;[^&\n]*&quot;)/g, '<span class="y-str">$1</span>')
      .replace(/\b(auto|const|if|return|struct|class|template|using|for|std|gmp|static|constexpr|void|bool|double)\b(?![^<]*<\/span>)/g, '<span class="y-key">$1</span>')
      .replace(/\b(0x[0-9A-Fa-f]+|\d+(\.\d+)?)\b(?![^<]*<\/span>)/g, '<span class="y-num">$1</span>');
  }
  const code = (text, lang = 'cpp') => `<pre class="yaml g-code">${lang === 'yaml' ? G.highlightYaml(text) : lang === 'cpp' ? highlightCpp(text) : esc(text)}</pre>`;

  /** Horizontal frame bar: [{ label, kind, sub, on }] — `on` highlights covered parts. */
  function segBar(parts, opts = {}) {
    return `<div class="g-seg${opts.dim ? ' dim' : ''}">${parts.map((p) =>
      `<div class="g-seg-part${p.on === false ? ' off' : ''}" style="--f:var(--k-${p.kind});--b:var(--k-${p.kind}-b);flex:${p.w || 1}">
        <b>${esc(p.label)}</b>${p.sub ? `<span>${esc(p.sub)}</span>` : ''}</div>`).join('')}</div>`;
  }

  function table(head, rows, cls = '') {
    return `<table class="g-table ${cls}"><thead><tr>${head.map((h) => `<th>${h}</th>`).join('')}</tr></thead><tbody>${rows.map((r) =>
      `<tr>${r.map((c) => `<td>${c}</td>`).join('')}</tr>`).join('')}</tbody></table>`;
  }

  const kbd = (t) => `<code class="g-k">${esc(t)}</code>`;
  const demo = (id) => `<div class="g-demo" data-demo="${id}"><div class="g-loading">Loading example…</div></div>`;
  const note = (html, kind = '') => `<div class="g-note ${kind}">${html}</div>`;

  // ----------------------------------------------------------------------------------------------
  // demos: schema + what to show + what to decode
  // ----------------------------------------------------------------------------------------------
  const DEMOS = {
    scalars: {
      title: 'Integers, floats and byte order', yaml: Y.scalars, cards: ['message:Reading'],
      decode: [{ target: 'message:Reading', label: 'id 7, counter 4660 (twice, both byte orders), delta −100, ratio 1.5', input: '07 12 34 34 12 FF 9C 3F C0 00 00' }],
    },
    enums: {
      title: 'Enums and bit fields', yaml: Y.enums, cards: ['message:Status'],
      decode: [
        { target: 'message:Status', label: 'mode run, flags 0xEA = armed 1 · gear 11 (drive) · level 0101 (5) · pad 0', input: '01 EA' },
        { target: 'message:Status', label: 'mode 5 is not declared and Mode is strict → UnknownEnum', input: '05 EA' },
      ],
    },
    containers: {
      title: 'Strings, bytes and arrays', yaml: Y.containers, cards: ['message:Packet'],
      decode: [{ target: 'message:Packet', label: '"GMP1", name "probe", 3 samples, note "ok", 4 bytes left over', input: '47 4D 50 31  05 70 72 6F 62 65  03 00 64 FF 9C 01 F4  6F 6B 00  DE AD BE EF' }],
    },
    structure: {
      title: 'Nested messages, conditions, constants and padding', yaml: Y.structure, cards: ['message:Report', 'message:Fault'],
      decode: [
        { target: 'message:Report', label: 'state 2 → the Fault block is present (code 0x42, 36.3 °C)', input: '5A 02 00 00 00 42 01 6B' },
        { target: 'message:Report', label: 'state 1 → no Fault block', input: '5A 01 00 00' },
        { target: 'message:Report', label: 'wrong magic → BadConst', input: '5B 01 00 00' },
      ],
    },
    printing: {
      title: 'Printing decoded messages', yaml: Y.structure, cards: ['message:Report'],
      decode: [
        { target: 'message:Report', label: 'gmp::println(report, stdout): enum names with their value, units, nested blocks', input: '5A 02 00 00 00 42 01 6B', view: 'print' },
        { target: 'message:Report', label: 'single_line style; the conditional Fault block is <absent>', input: '5A 01 00 00', view: 'line' },
      ],
    },
    values: {
      title: 'Scaling, conversions, field CRCs and computed values', yaml: Y.values, cards: ['message:Measurement', 'message:SerialNumber'],
      decode: (s) => [
        { target: 'message:Measurement', label: '25.00 °C, 54.00 %RH; each word is followed by its CRC-8', input: D.exampleInput(s, 'message:Measurement') },
        { target: 'message:SerialNumber', label: 'two words with CRCs; serial() joins them', input: D.exampleInput(s, 'message:SerialNumber') },
      ],
    },
    framing: {
      title: 'The four framing types', yaml: Y.framing, cards: ['protocol:Datagram', 'protocol:FixedFrames', 'protocol:LengthFrames', 'protocol:StxEtx'],
      decode: (s) => {
        const note = [5, ...str('hello')];
        const ping = [0x01, 0x02];
        const lf = buildFrame(s, 'LengthFrames', 'Ping', ping);
        const bad = buildFrame(s, 'LengthFrames', 'Ping', [0x01, 0x03]);
        bad[bad.length - 1] ^= 0xFF;
        return [
          { target: 'protocol:Datagram', label: 'none: the buffer is the message', input: hexLine(ping) },
          { target: 'protocol:FixedFrames', label: 'fixed: sync A5, 2 bytes, CRC-8, twice', input: hexLine([...buildFrame(s, 'FixedFrames', 'Ping', ping), ...buildFrame(s, 'FixedFrames', 'Ping', [0x01, 0x03])]) },
          { target: 'protocol:LengthFrames', label: 'length prefixed: Ping, Note, line noise, a corrupted Ping, then a good one', input: [hexLine(lf), hexLine(buildFrame(s, 'LengthFrames', 'Note', note)), '13 37', hexLine(bad), hexLine(lf)].join('\n') },
          { target: 'protocol:StxEtx', label: 'delimited: STX … ETX; the 03 inside the Ping is escaped as 10 23', input: [hexLine(buildFrame(s, 'StxEtx', 'Ping', [0x03, 0x41])), hexLine(buildFrame(s, 'StxEtx', 'Note', note))].join('\n') },
        ];
      },
    },
    escaping: {
      title: 'Reserved sync byte with index escaping', yaml: Y.escaping, cards: ['protocol:SyncLink'],
      decode: (s) => [{ target: 'protocol:SyncLink', label: 'escaped frame, a frame cut short (resync at the next 80), an unknown escape code', input: D.exampleInput(s, 'protocol:SyncLink') }],
    },
    nmea: {
      title: 'Text protocol: NMEA 0183', yaml: Y.nmea, cards: ['protocol:Nmea', 'message:GGA'],
      decode: (s) => [{ target: 'protocol:Nmea', label: 'a fix, a corrupted sentence, an unknown sentence, a fix with empty optional fields', input: D.exampleInput(s, 'protocol:Nmea'), format: 'text', newline: 'crlf' }],
    },
    stream: {
      title: 'A stream with noise and a corrupted frame', yaml: null, state: () => G.exampleState(), cards: ['protocol:Link'],
      decode: (s) => [{ target: 'protocol:Link', label: 'the parser skips garbage, rejects the bad frame and keeps every good one', input: D.exampleInput(s, 'protocol:Link') }],
    },
  };

  // ----------------------------------------------------------------------------------------------
  // sections
  // ----------------------------------------------------------------------------------------------
  const TYPE_TILES = [
    ['int', 'u8 … u64, i8 … i64', '1–8 B', 'Unsigned / two\'s complement integers in big or little endian.', '{ name: rpm, type: u16 }'],
    ['float', 'f32, f64', '4 / 8 B', 'IEEE-754 floating point.', '{ name: ratio, type: f32 }'],
    ['bool', 'bool, char', '1 B', 'A flag (0 / 1) or a character.', '{ name: armed, type: bool }'],
    ['enum', 'enums', 'storage', 'Named values with to_string() and an optional strict check.', '{ name: mode, type: Mode }'],
    ['bits', 'bits', 'whole bytes', 'Bit fields packed msb_first or lsb_first, with enums, consts and scale.', '{ name: flags, type: bits, fields: [...] }'],
    ['bytes', 'bytes', 'fixed / var', 'Raw bytes: fixed, from a field, remaining, or terminated.', '{ name: data, type: bytes, length: n, max: 64 }'],
    ['string', 'string', 'fixed / var', 'Characters: fixed (padded), from a field, terminated; one token in text.', '{ name: name, type: string, length: 8 }'],
    ['array', 'array', 'n × element', 'Repeated scalars, enums, nested messages or custom types.', '{ name: samples, type: array, of: i16, count: n, max: 32 }'],
    ['message', 'nested message', 'its size', 'Another message used as a field; views nest without copying.', '{ name: fault, type: Fault }'],
    ['custom', 'custom type', 'declared', 'Your C++ decode / encode functions for anything GMP does not describe.', '{ name: when, type: bcd_time }'],
    ['pad', 'pad', 'N B', 'Reserved bytes: skipped on decode, zero on encode.', '{ pad: 2 }'],
    ['check', 'crc (field)', '1–8 B', 'A checksum right after one value, verified per field.', '{ name: t, type: u16, crc: crc8_nrsc5 }'],
  ];

  function overview() {
    const step = (kind, title, sub, body) => `<div class="g-step" style="--f:var(--k-${kind});--b:var(--k-${kind}-b)"><div class="g-step-t">${title}</div><div class="g-step-s">${sub}</div>${body ? `<div class="g-step-b">${body}</div>` : ''}</div>`;
    return `
      <p class="g-lead">GMP turns a <b>YAML description of a wire protocol</b> into dependency-free C++23 that decodes, validates and
      encodes it. Unlike protobuf the wire format is not fixed: the schema describes <i>existing</i> binary and text protocols,
      from CAN payloads and sensor registers to NMEA sentences and framed serial links.</p>
      <div class="g-pipeline">
        ${step('message', 'schema.yaml', 'messages, enums, protocols', '<code>fields: [ { name: rpm, type: u16 } ]</code>')}
        <div class="g-arrow">→</div>
        ${step('custom', 'gmpc', 'checks &amp; generates', 'file:line:col diagnostics')}
        <div class="g-arrow">→</div>
        ${step('int', 'schema.gmp.hpp', 'one header per schema', '<code>struct</code> · <code>View</code> · <code>Protocol</code>')}
        <div class="g-arrow">→</div>
        ${step('string', 'your code', 'decode · encode · stream', '<code>Link::Stream&lt;&gt;</code>')}
      </div>
      <div class="g-props">
        ${['Header-only runtime', 'No heap', 'No exceptions / RTTI', 'C++23 std::expected', 'Zero-copy views', 'Eager structs', 'Streaming with resync', 'Binary and text'].map((t) => `<span class="badge accent">${t}</span>`).join('')}
      </div>
      <div class="g-cols">
        <div><h4>1 · Describe</h4>${code(`schema_version: 1
namespace: demo
messages:
  Status:
    type_id: 1
    fields:
      - { name: voltage, type: u16, scale: 0.001, unit: V }
      - { name: count, type: u8 }
      - { name: samples, type: array, of: i16, count: count, max: 32 }
protocols:
  Link:
    header: [ { name: id, type: u8 }, { name: len, type: u16 } ]
    framing: { type: length_prefixed, sync: [0xAA, 0x55], length: { field: len } }
    checksum: { algo: crc16_ccitt }
    message_type: { field: id }
    messages: [Status]`, 'yaml')}</div>
        <div><h4>2 · Generate</h4>${code(`add_subdirectory(generic_message_parser)
gmp_generate(TARGET link_msgs SCHEMAS link.yaml)
target_link_libraries(app PRIVATE link_msgs)`, 'text')}
          <h4>3 · Use</h4>${code(`demo::Status s;
s.set_voltage_scaled(12.6);
s.samples = {1, 2, 3};          // count is filled in automatically
std::array<std::uint8_t, demo::Link::max_frame> buf;
auto n = demo::Link::encode(s, buf);

demo::Link::Stream<> stream;    // bytes in any chunk size
stream.feed(chunk, gmp::overloaded{
    [](const demo::StatusView& v) { v.voltage_scaled(); },
    [](const gmp::FrameError& e) { /* skipped bytes */ },
});`)}</div>
      </div>`;
  }

  function anatomy() {
    return `
      <p>A schema has two layers. <b>Messages</b> describe a payload: an ordered list of fields, usable on their own
      (<code>Status::decode(bytes)</code>). <b>Protocols</b> wrap messages in frames on a byte stream:</p>
      ${segBar([
        { label: 'prefix', sub: 'sync / start', kind: 'frame' },
        { label: 'header', sub: 'type id, length…', kind: 'int', w: 1.4 },
        { label: 'body', sub: 'one message', kind: 'message', w: 3 },
        { label: 'checksum', sub: 'CRC / sum', kind: 'check' },
        { label: 'suffix', sub: 'end delimiter', kind: 'frame' },
      ])}
      <div class="g-keys">
        ${[
          ['schema_version', '1', 'Required. Lets the format evolve.'],
          ['namespace', 'acme::telemetry', 'C++ namespace of the generated code.'],
          ['defaults', '{ endian: big, … }', 'endian, bit_order, format, separator, access, strict_enums for everything below.'],
          ['enums', 'Mode: { … }', 'Named integer values, used as field types and type ids.'],
          ['custom', 'types / checksums / framers', 'Hooks into your own C++ code, checked with concepts.'],
          ['messages', 'Status: { fields: […] }', 'Payload layouts. Field order is wire order.'],
          ['protocols', 'Link: { framing, … }', 'Framing, header, checksum, message selection.'],
        ].map(([k, v, d]) => `<div class="g-key"><code class="g-k">${k}</code><code class="g-kv">${esc(v)}</code><p>${d}</p></div>`).join('')}
      </div>
      ${note('Schemas are strict: an unknown key such as <code>feilds</code> is an error with <code>file:line:col</code>, and renamed keys say what they became.')}`;
  }

  function types() {
    return `
      <p>Every field has a <code>type</code>. The colours below are the ones used in the Studio's layout and decode views.</p>
      <div class="g-tiles">${TYPE_TILES.map(([k, name, size, d, y]) => `<div class="g-tile" style="--f:var(--k-${k});--b:var(--k-${k}-b)">
          <div class="g-tile-h"><b>${name}</b><span>${size}</span></div><p>${d}</p><code>${esc(y)}</code></div>`).join('')}</div>
      <h3 id="g-scalars">Integers, floats and byte order</h3>
      <p>Byte order comes from <code>defaults.endian</code> and can be overridden per message, protocol or field. Hover a row
      in a decoded example to see exactly which bytes it came from.</p>
      ${demo('scalars')}
      <h3 id="g-enums">Enums and bit fields</h3>
      <p>Enums map numbers to names (<code>to_string()</code>, <code>is_known()</code>). With <code>strict: true</code> (or
      <code>defaults.strict_enums</code>) undeclared values fail with <code>UnknownEnum</code>, on decode and on encode.
      A <code>bits</code> group packs bit fields into whole bytes; each bit field can be a bool, an integer, an enum, a const or scaled.</p>
      ${demo('enums')}
      <h3 id="g-containers">Strings, bytes and arrays</h3>
      <p>Variable-size fields get their length from a number, an earlier field or expression (<code>length: n * 2</code>),
      <code>remaining</code>, or a <code>terminator</code>. <code>max</code> is the capacity of the fixed-size container in the struct, so nothing allocates.
      On encode, a plain length field is filled in from the container size.</p>
      ${segBar([
        { label: 'length: 4', sub: 'fixed', kind: 'string' },
        { label: 'length: len', sub: 'from a field', kind: 'string' },
        { label: 'count: n', sub: 'array elements', kind: 'array' },
        { label: 'terminator: 0', sub: 'C string', kind: 'string' },
        { label: 'remaining', sub: 'last field', kind: 'bytes' },
      ])}
      ${demo('containers')}
      <h3 id="g-structure">Nested messages, conditions, constants and padding</h3>
      <p>Messages can contain messages. <code>when:</code> makes a field exist only if an expression over earlier fields
      is true (it becomes <code>std::optional</code>). <code>const</code> fields are checked on decode and always written on encode.</p>
      ${demo('structure')}`;
  }

  function values() {
    return `
      <p>Fields keep their raw value; GMP adds engineering accessors around it.</p>
      <div class="g-formula">
        <div><span class="badge">raw</span> <code>0x6666</code></div><div class="g-arrow">→</div>
        <div><code>× scale + offset</code></div><div class="g-arrow">→</div>
        <div><span class="badge accent">temperature_scaled()</span> <code>25.00 degC</code></div>
        <div class="g-arrow">+</div><div><span class="badge accent">temperature_degf()</span> <code>77.00 degF</code></div>
      </div>
      ${table(['key', 'generates', 'notes'], [
        [kbd('scale / offset / unit'), '<code>x_scaled()</code>, <code>set_x_scaled(v)</code>', 'value = raw × scale + offset'],
        [kbd('conversions: { degf: {…} }'), '<code>x_degf()</code>, <code>set_x_degf(v)</code>', 'extra units for the same raw value'],
        [kbd('crc: crc8_nrsc5'), 'verified in read / from(), written by encode', 'checksum stored right after the value'],
        [kbd('computed: { serial: {…} }'), '<code>serial()</code>, <code>set_serial(v)</code>', 'no bytes of its own; setter derived for shift / | packing'],
      ])}
      ${demo('values')}`;
  }

  function messages() {
    return `
      ${table(['key', 'meaning'], [
        [kbd('type_id'), 'How a protocol recognises this message: a header field value (number or enum value) or a text token such as <code>GPGGA</code>.'],
        [kbd('format'), '<code>binary</code> byte layout or <code>text</code> tokens.'],
        [kbd('access'), '<code>view</code>, <code>struct</code> or both (default).'],
        [kbd('endian / bit_order'), 'Override the defaults for this message.'],
        [kbd('separator'), 'Text token separator.'],
        [kbd('doc'), 'Copied into the generated code.'],
      ])}
      <div class="g-cols">
        <div><h4>View: zero copy</h4><p>Wraps the input bytes; accessors read on demand. <code>from()</code> validates once.</p>
          ${code(`auto v = demo::StatusView::from(bytes);
if (!v) return v.error();          // gmp::Error
double volts = v->voltage_scaled();
for (auto s : v->samples()) use(s);`)}</div>
        <div><h4>Struct: eager value</h4><p>Fixed-capacity members, <code>operator==</code>, and the only way to encode.</p>
          ${code(`demo::Status s;
s.set_voltage_scaled(12.6);
s.samples = {1, 2, 3};
std::array<std::uint8_t, demo::Status::max_wire_size> out;
auto n = s.encode(out);            // gmp::Result<size_t>`)}</div>
      </div>`;
  }

  function framing() {
    return `
      <p>A protocol finds frames in a byte stream, validates them and picks the message. Four framings cover most links,
      and a <code>custom</code> framer covers the rest.</p>
      ${table(['type', 'frame', 'how the end is found'], [
        [kbd('none'), 'the buffer', 'Datagrams, CAN payloads: whole-buffer mode only.'],
        [kbd('fixed'), '<code>sync</code> + <code>size</code> bytes', 'Every frame is the same size; shorter bodies are zero padded.'],
        [kbd('length_prefixed'), '<code>sync</code> + header with a length', 'The length field (filled in on encode) says how much follows.'],
        [kbd('delimited'), '<code>start</code> … <code>end</code>', 'Search for the end pattern; HDLC-style shared flags allowed.'],
        [kbd('custom'), 'your framer', '<code>static gmp::FrameScan scan(gmp::bytes_view)</code>'],
      ])}
      <h3 id="g-length">What the length counts</h3>
      ${[['body', [0, 0, 1, 0]], ['body_checksum', [0, 0, 1, 1]], ['header_body', [0, 1, 1, 0]], ['all', [1, 1, 1, 1]]].map(([c, on]) =>
        `<div class="g-cov"><code class="g-k">covers: ${c}</code>${segBar([
          { label: 'sync', kind: 'frame', on: !!on[0] }, { label: 'header', kind: 'int', on: !!on[1], w: 1.3 },
          { label: 'body', kind: 'message', on: !!on[2], w: 3 }, { label: 'checksum', kind: 'check', on: !!on[3] }])}</div>`).join('')}
      <p>Covered size = value + <code>adjust</code>. A frame bigger than <code>max_frame</code> (derived automatically when messages are bounded) is rejected at once.</p>
      ${demo('framing')}`;
  }

  function checksums() {
    const names = Object.entries(G.CHECKSUMS);
    return `
      <p>A protocol checksum is stored after the body and covers part of the frame. A mismatch drops the frame with
      <code>BadChecksum</code>; the stream rescans from the next byte so the following frames are not lost.</p>
      ${[['header_body (default)', [0, 1, 1, 0]], ['body', [0, 0, 1, 0]], ['all', [1, 1, 1, 0]]].map(([c, on]) =>
        `<div class="g-cov"><code class="g-k">covers: ${c}</code>${segBar([
          { label: 'sync', kind: 'frame', on: !!on[0] }, { label: 'header', kind: 'int', on: !!on[1], w: 1.3 },
          { label: 'body', kind: 'message', on: !!on[2], w: 3 }, { label: 'checksum', kind: 'check' }])}</div>`).join('')}
      <h3>Built-in algorithms</h3>
      <div class="g-algos">${names.map(([n, size]) => `<span class="g-algo${n.startsWith('crc') ? '' : ' sum'}"><code>${n}</code><small>${size} B</small></span>`).join('')}</div>
      ${code(`checksum: { algo: crc16_ccitt, covers: header_body, endian: big }
checksum: { algo: { crc: { width: 8, poly: 0x1D, init: 0xFF, xorout: 0xFF } } }   # any CRC
checksum: { algo: my_sum }                                                         # custom.checksums
checksum: { algo: xor8, separator: "*" }                                           # text (NMEA)`, 'yaml')}
      ${note('Text protocols compute the checksum between the start delimiter and the separator and write it as uppercase hex digits.')}`;
  }

  function messageType() {
    return `
      <p>When a protocol carries several messages, <code>message_type</code> says how to tell them apart. Every message
      declares its <code>type_id</code>; an unknown id fails with <code>UnknownType</code>.</p>
      <div class="g-cols">
        <div><h4>Binary: a header field</h4>${code(`header: [ { name: kind, type: u8 }, { name: len, type: u8 } ]
message_type: { field: kind }
# Ping has type_id: 1, Note has type_id: 2`, 'yaml')}
          ${segBar([{ label: 'AA 55', kind: 'frame' }, { label: 'kind = 1', kind: 'int', sub: '→ Ping' }, { label: 'len', kind: 'int' }, { label: 'Ping body', kind: 'message', w: 2 }, { label: 'crc', kind: 'check' }])}</div>
        <div><h4>Text: the first token</h4>${code(`message_type: { token: 0 }
# GGA has type_id: GPGGA`, 'yaml')}
          ${segBar([{ label: '$', kind: 'frame' }, { label: 'GPGGA', kind: 'string', sub: '→ GGA' }, { label: ',123519,4807.038,…', kind: 'string', w: 3 }, { label: '*47', kind: 'check' }])}</div>
      </div>
      ${note('In C++, <code>parse()</code> returns <code>AnyView</code>, a <code>std::variant</code> of the message views, and a stream handler is called with whichever view matched.')}`;
  }

  function escaping() {
    return `
      <p>Some links reserve byte values: <code>0x80</code> may only ever mean "start of frame". With <code>escape</code>,
      every reserved byte inside a frame is sent as the escape byte plus a code, so the parser can always resynchronise
      on the next sync byte.</p>
      <div class="g-escape">
        ${[['0x80', '81 00', 'sync byte'], ['0x81', '81 01', 'escape byte itself'], ['0x42', '42', 'ordinary byte']].map(([a, b, d]) =>
          `<div class="g-esc-row"><span class="cell c-${a === '0x42' ? 'int' : 'frame'}">${a.slice(2)}</span><span class="g-arrow">→</span>${b.split(' ').map((x) => `<span class="cell c-${b.length > 2 ? 'check' : 'int'}">${x}</span>`).join('')}<small>${d}</small></div>`).join('')}
      </div>
      ${table(['code', 'reserved byte b is sent as', 'example'], [
        [kbd('xor'), 'escape, b ⊕ mask', 'HDLC / PPP: <code>{ char: 0x7D, xor: 0x20 }</code>'],
        [kbd('index'), 'escape, base + position of b in reserved', '<code>{ char: 0x81, code: index, reserved: [0x80, 0x81] }</code>'],
        [kbd('map'), 'escape, map[b]', 'SLIP: <code>{ char: 0xDB, map: { 0xC0: 0xDC, 0xDB: 0xDD } }</code>'],
      ])}
      <ul class="g-list">
        <li>Works with <code>delimited</code>, <code>fixed</code> and <code>length_prefixed</code> framing; up to 16 reserved bytes.</li>
        <li>Sizes count unescaped bytes by default. <code>length: { counts: wire, covers: body | body_checksum }</code> counts escaped bytes instead.</li>
        <li>The checksum is computed before escaping; <code>checksum.over: wire</code> computes it over the escaped bytes.</li>
        <li>A raw reserved byte inside a frame means it was cut short: it is dropped (<code>BadDelimiter</code>) and a new frame starts there.</li>
      </ul>
      ${demo('escaping')}`;
  }

  function text() {
    return `
      <p>Text messages are tokens separated by a character. Numbers are parsed with <code>std::from_chars</code>;
      <code>width</code> and <code>precision</code> control encoding, <code>optional</code> allows empty tokens, and
      <code>pad</code> skips tokens.</p>
      ${demo('nmea')}`;
  }

  function streaming() {
    return `
      <p><code>Protocol::Stream&lt;&gt;</code> owns a fixed buffer of <code>max_frame</code> bytes. Feed it chunks of any
      size; it finds frames, validates them and calls your handler with the matching view.</p>
      <div class="g-flow">
        ${['bytes in (any chunking)', 'scan: find sync / delimiter / length', 'open: checksum, unescape', 'select message by type id', 'handler(View)'].map((t, i) =>
          `<div class="g-flow-s"><span>${i + 1}</span>${t}</div>`).join('<div class="g-arrow">→</div>')}
      </div>
      ${code(`demo::Link::Stream<> parser;
parser.feed(chunk, gmp::overloaded{
    [](const demo::Link::HeaderView& h, const demo::StatusView& v) { /* header + message */ },
    [](const demo::SamplesView& v) { /* message only */ },
    [](const gmp::FrameError& e) { /* optional: e.error, e.data */ },
});
auto& st = parser.stats();   // frames, frame_errors, bytes_skipped, overflows`)}
      <h3>Storing the handler</h3>
      <p>The parser can keep the handler, so the code that receives bytes (a UART driver, a socket loop) only calls
      <code>parser.feed(bytes)</code>. <code>make_stream</code> stores an overload set with no runtime cost.
      <code>CallbackStream</code> holds runtime listeners. Its type doesn't depend on the handlers, so it can be a
      global or a class member, and listeners can be changed while running.</p>
      ${code(`auto parser = demo::Link::make_stream(gmp::overloaded{   // compile-time handler
    [&](const demo::StatusView& v) { app.on_status(v); },
});
parser.feed(chunk);

demo::Link::CallbackStream<> link;                        // runtime callbacks
link.on<&App::on_status>(app)      // (StatusView): one listener per message
    .on<&App::on_command>(app)     // (Link::HeaderView, CommandView): header only when asked for
    .on<&App::on_frame>(app)       // (Link::AnyView): every valid frame, after the message listener
    .on<&App::on_drop>(app);       // (gmp::FrameError): garbage, bad checksums, unknown ids
link.feed(chunk);`)}
      ${note('The listener is chosen from the function\'s last parameter. Free functions taking <code>(App&amp;, View)</code>, captureless lambdas, and C-style <code>void cb(void* ctx, const View&amp;)</code> pointers work too. Registering again replaces the listener; <code>off&lt;View&gt;()</code> removes it.')}
      <h3>Pulling messages</h3>
      <p>Or ask for the next message yourself, which suits a main loop or a request/response exchange. A polled
      view is valid until the parser is used again (Debug builds check this).</p>
      ${code(`parser.push(chunk);
while (auto m = parser.poll()) {                          // empty: more input needed
    std::visit(handlers, *m);
}
auto r = parser.poll_frame();      // { header, message, bytes }
auto s = parser.poll_message();    // decoded struct copy
auto e = parser.poll_event();      // message or gmp::FrameError, in stream order
auto reply = link.poll_for<demo::StatusView>();   // others go to the listeners meanwhile`)}
      ${demo('stream')}`;
  }

  function printing() {
    return `
      <p>Every generated struct, view and bit group has <code>print_to()</code>, so <code>gmp::print</code> can dump it
      for a console or a debug log. Enums print as <code>name (0xNN)</code> and undeclared values as
      <code>&lt;unknown&gt; (0x07)</code>, so the raw value is always visible. Scaled fields show the raw value followed
      by the engineering value and unit. Bytes print as hex, strings are quoted and escaped, and missing conditional fields
      print as <code>&lt;absent&gt;</code>. A view prints exactly like its struct.</p>
      ${code(`gmp::println(status, stdout);                       // FILE*: console or fopen'ed log
gmp::print(status, std::cout);                         // anything with write(): std::ostream
char line[256];  gmp::print(status, line);            // char buffer, NUL terminated
gmp::static_string<256> s;  gmp::print(status, s);    // appends; std::string works too
gmp::print(status, [](std::string_view t) { uart_log(t); });   // callback

gmp::print(status, stdout, {.style = gmp::PrintStyle::single_line});
// options: style, indent, max_items, max_bytes (0 = all), precision (significant digits of scaled values)

// Stream handlers can print the view directly, and Protocol::parse() results print whichever message matched
stream.feed(chunk, [](const demo::StatusView& v) { gmp::println(v, stdout); });`)}
      ${table(['field', 'printed as'], [
        ['integer, float, bool, char', '<code>42</code>, <code>1.5</code>, <code>true</code>, <code>\'N\'</code>'],
        ['enum', '<code>run (0x01)</code>, <code>&lt;unknown&gt; (0x07)</code>'],
        ['scale / unit / conversions', '<code>1234 (12.34 V)</code>, <code>26214 (25 degC, 77 degF)</code>, <code>12 mA</code> (unit, no scale)'],
        ['bits group, nested message', 'an indented <code>{ … }</code> block'],
        ['bytes, string', '<code>[0A 1B 2C]</code>, <code>"text\\r\\n"</code>'],
        ['array', '<code>[1, 2, 3]</code>; messages one per line; <code>... (+N more)</code> past <code>max_items</code>'],
        ['conditional field', 'its value, or <code>&lt;absent&gt;</code>'],
        ['computed value', '<code>serial: 305419896 (computed)</code>'],
        ['custom type', 'your <code>gmp_print(gmp::Printer&amp;, const T&amp;)</code> next to the type, else <code>&lt;?&gt;</code>'],
      ])}
      ${note('Bounded sinks (char buffers, static_string, or a callback that returns false) keep what fits and return <code>BufferFull</code>. Printing never allocates. In the Studio decode tester, the <b>View</b> switch shows the same text for every decoded frame.')}
      ${demo('printing')}`;
  }

  function cpp() {
    return `
      ${table(['generated', 'what it is'], [
        ['<code>struct M</code>', 'Members per field (std::optional when conditional), <code>decode()</code>, <code>encode()</code>, scaled / conversion / computed accessors, <code>operator==</code>.'],
        ['<code>class MView</code>', '<code>from(bytes)</code> validates once; accessors read from the buffer. <code>to_struct()</code> copies out.'],
        ['<code>enum class E</code>', '<code>to_string()</code>, <code>is_known()</code>.'],
        ['<code>print_to()</code>', 'On structs, views and bit groups: drives <code>gmp::print(msg, sink)</code> (see Printing for logs).'],
        ['<code>struct P</code>', '<code>parse()</code> → <code>AnyView</code>, <code>decode()</code> → <code>AnyMessage</code>, <code>encode(msg, out)</code>, <code>Stream&lt;&gt;</code>, <code>make_stream()</code> / <code>StreamWith</code>, <code>Callbacks</code> / <code>CallbackStream</code>, layout constants.'],
      ])}
      <h3>Errors</h3>
      <p>Every fallible call returns <code>gmp::Result&lt;T&gt;</code> (<code>std::expected&lt;T, gmp::Error&gt;</code>).</p>
      ${table(['error', 'meaning'], [
        ['Truncated', 'Input ended before the message or frame was complete.'],
        ['BadSync', 'Bytes before a sync word / start delimiter were skipped.'],
        ['BadChecksum', 'Frame checksum or field crc mismatch (or a missing text checksum).'],
        ['BadLength', 'Length out of range, frame larger than max_frame, or an escaped length split.'],
        ['BadDelimiter', 'A start delimiter or reserved byte appeared inside a frame.'],
        ['BadConst', 'A const field or bit field did not match.'],
        ['OutOfRange', 'More data than a container\'s max, or a value that does not fit.'],
        ['UnknownType', 'No message has the frame\'s type id.'],
        ['UnknownEnum', 'A strict enum field holds an undeclared value.'],
        ['BufferFull', 'Output buffer too small, or stream buffer full without a frame.'],
        ['ParseError / MissingField', 'A text token is invalid / a required token is empty.'],
        ['InvalidValue / Custom', 'For custom codecs (also: an unknown escape code).'],
      ].map(([a, b]) => [`<code>${a}</code>`, b]))}`;
  }

  function studio() {
    return `
      <div class="g-studio">
        <div class="g-studio-pane"><h4>Left: editor</h4><p>Pick a protocol, message or enum in the outline and edit it
          with forms. <b>Hover any option</b> for an explanation of what it does and the YAML key it writes.</p></div>
        <div class="g-studio-pane"><h4>Layout</h4><p>Size-proportional cards of every frame and message, with offsets,
          checksum and length coverage. Click a block to edit the field.</p></div>
        <div class="g-studio-pane"><h4>YAML</h4><p>The schema as gmpc reads it. Download it, or import an existing schema.</p></div>
        <div class="g-studio-pane"><h4>Decode tester</h4><p>Paste hex or text; frames are found and decoded live. Hover
          a field to highlight its bytes, or a byte to find its field.</p></div>
      </div>
      <div class="g-legend">${G.renderLegend()}</div>`;
  }

  const SECTIONS = [
    ['overview', 'What is GMP', overview],
    ['anatomy', 'Anatomy of a schema', anatomy],
    ['types', 'Field types', types],
    ['values', 'Values: units, CRCs, computed', values],
    ['messages', 'Messages: views and structs', messages],
    ['framing', 'Protocols and framing', framing],
    ['checksums', 'Checksums', checksums],
    ['message-type', 'Choosing the message', messageType],
    ['escaping', 'Reserved bytes and escaping', escaping],
    ['text', 'Text protocols', text],
    ['streaming', 'Streaming and resync', streaming],
    ['printing', 'Printing for logs', printing],
    ['cpp', 'Generated C++ and errors', cpp],
    ['studio', 'Using the Studio', studio],
  ];

  // ----------------------------------------------------------------------------------------------
  // rendering
  // ----------------------------------------------------------------------------------------------
  const states = {};

  function renderDemo(el) {
    const id = el.dataset.demo;
    const d = DEMOS[id];
    let s;
    try {
      s = d.state ? d.state() : G.fromYaml(d.yaml);
    } catch (e) {
      el.innerHTML = `<div class="callout bad">Example failed to load: ${esc(e.message)}</div>`;
      return;
    }
    states[id] = s;
    const yaml = d.yaml || G.toYaml(s);
    const cards = d.cards.map((c) => { const [k, n] = c.split(':'); return G.renderCard(s, k, n); }).join('');
    const runs = (typeof d.decode === 'function' ? d.decode(s) : d.decode).map((r) => {
      const dui = { target: r.target, format: r.format || 'hex', input: r.input || '', newline: r.newline || 'lf', view: r.view || 'fields' };
      return `<div class="g-run">
        <div class="g-run-head"><span class="g-label">Decode ${esc(r.target.split(':')[1])}</span><span>${esc(r.label)}</span></div>
        <pre class="g-input">${esc(dui.input)}</pre>
        <div class="g-result">${G.renderDecodeResults(s, dui)}</div></div>`;
    }).join('');
    el.innerHTML = `
      <div class="g-demo-head"><span class="g-demo-title">${esc(d.title)}</span>
        <button class="btn small" data-open-studio="${id}" title="Load this schema into the Studio editor">Open in Studio ↗</button></div>
      <div class="g-demo-grid">
        <div class="g-demo-yaml"><div class="g-label">YAML</div><pre class="yaml">${G.highlightYaml(yaml)}</pre></div>
        <div class="g-demo-cards"><div class="g-label">Layout</div><div class="g-cards">${cards}</div></div>
      </div>
      <div class="g-runs">${runs}</div>`;
  }

  function highlight(root, start, end, nodeId) {
    root.querySelectorAll('.cell.hl, .drow.hl').forEach((e) => e.classList.remove('hl'));
    if (start == null) return;
    root.querySelectorAll('.cell[data-i]').forEach((c) => {
      const i = Number(c.dataset.i);
      if (i >= start && i < end) c.classList.add('hl');
    });
    if (nodeId) root.querySelectorAll(`.drow[data-n="${nodeId}"]`).forEach((r) => r.classList.add('hl'));
  }

  G.renderGuide = () => {
    const nav = document.getElementById('guide-nav');
    const main = document.getElementById('guide-main');
    nav.innerHTML = `<div class="g-nav-title">Guide</div>${SECTIONS.map(([id, title], i) =>
      `<a href="#${id}" data-sec="${id}"><span>${String(i + 1).padStart(2, '0')}</span>${esc(title)}</a>`).join('')}
      <a class="g-nav-studio" href="index.html">← Back to the Studio</a>`;
    main.innerHTML = SECTIONS.map(([id, title, fn], i) =>
      `<section class="g-sec" id="${id}"><div class="g-sec-kicker">${String(i + 1).padStart(2, '0')}</div><h2>${esc(title)}</h2>${fn()}</section>`).join('') +
      '<footer class="g-foot">Every example on this page is a real schema, laid out and decoded by the same code as the Studio.</footer>';

    const toHash = () => {
      const t = location.hash && document.getElementById(location.hash.slice(1));
      if (t) t.scrollIntoView({ behavior: 'instant', block: 'start' });
    };
    G.loadJsYaml().then(() => {
      main.querySelectorAll('.g-demo').forEach(renderDemo);
      toHash();  // examples change the page height: land on the section again
      spy();
    }, (e) => {
      main.querySelectorAll('.g-demo').forEach((el) => {
        el.innerHTML = `<div class="callout bad">The live examples need js-yaml from cdnjs.cloudflare.com, which could not be loaded (${esc(e && e.message ? e.message : e)}). The rest of the guide works offline.</div>`;
      });
    });

    // decode hover: bytes <-> fields, per example run
    main.addEventListener('mouseover', (e) => {
      const run = e.target.closest('.g-run');
      if (!run) return;
      const row = e.target.closest('.drow');
      if (row) { highlight(run, Number(row.dataset.s), Number(row.dataset.e), row.dataset.n); return; }
      const cell = e.target.closest('.cell[data-i]');
      if (cell && cell.dataset.n) {
        const r = run.querySelector(`.drow[data-n="${cell.dataset.n}"]`);
        if (r) highlight(run, Number(r.dataset.s), Number(r.dataset.e), r.dataset.n);
        else highlight(run, Number(cell.dataset.i), Number(cell.dataset.i) + 1, null);
        return;
      }
      highlight(run, null);
    });

    main.addEventListener('click', (e) => {
      const b = e.target.closest('[data-open-studio]');
      if (!b) return;
      const s = states[b.dataset.openStudio];
      if (!s) return;
      if (!window.confirm('Open this example in the Studio? It replaces the schema currently open there (download it first if you want to keep it).')) return;
      G.save(s);
      window.location.href = 'index.html';
    });

    // scroll spy (also run by the example loader above, which finishes later)
    const links = [...nav.querySelectorAll('a[data-sec]')];
    const secs = [...main.querySelectorAll('.g-sec')];
    const spy = () => {
      let cur = secs[0];
      for (const sec of secs) if (sec.getBoundingClientRect().top - main.getBoundingClientRect().top < 120) cur = sec;
      links.forEach((a) => a.classList.toggle('on', a.dataset.sec === cur.id));
    };
    main.addEventListener('scroll', spy, { passive: true });
    toHash();
    spy();
  };

  G.guideBuildFrame = buildFrame;
  G.GUIDE_DEMOS = DEMOS;
})(window.GMP);
