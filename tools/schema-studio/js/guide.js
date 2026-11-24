// guide.js - the visual guide (guide.html): every GMP feature with live, decoded examples.
// Examples are real schemas: they are parsed, laid out and decoded by the same code as the Studio.
(function (G) {
  'use strict';
  const esc = G.esc;
  const D = G.decoder;

  // ----------------------------------------------------------------------------------------------
  // code language: C++, C, JavaScript or Python (`?lang=c`, `?lang=js`, `?lang=py`, remembered in localStorage).
  // Schemas and decoding are shared; only code samples, API names and error names differ.
  // ----------------------------------------------------------------------------------------------
  const LANGS = { cpp: 'C++', c: 'C', js: 'JavaScript', py: 'Python' };
  const LANG_KEY = 'gmp-guide-lang';
  let lang = 'cpp';
  /** The C++, C, JavaScript or Python variant of a piece of content. */
  const ln = (cpp, c, js, py) => (lang === 'c' ? c : lang === 'js' ? js : lang === 'py' ? py : cpp);
  G.guideLang = () => lang;

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
    limits: `schema_version: 1
namespace: guide
messages:
  Climate:
    fields:
      # SHT4x humidity: RH = -6 + 125 * raw / 65535, only 0..100 %RH is physical
      - { name: humidity, type: u16, scale: 0.0019073486328125, offset: -6, unit: "%RH", min_value: 0, max_value: 100 }
      - { name: fan, type: u8, max_raw: 3 }               # raw wire value: speed steps 0..3
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
      .replace(/\b(auto|const|if|return|struct|class|template|using|for|std|gmsg|static|constexpr|void|bool|double)\b(?![^<]*<\/span>)/g, '<span class="y-key">$1</span>')
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
    limits: {
      title: 'Value limits', yaml: Y.limits, cards: ['message:Climate'],
      decode: [
        { target: 'message:Climate', label: 'humidity 30000 → 51.22 %RH, fan 2', input: '75 30 02' },
        { target: 'message:Climate', label: 'humidity 0xFFFF → 119 %RH is above max_value 100 → ValueOutOfLimits', input: 'FF FF 02' },
        { target: 'message:Climate', label: 'fan 4 is above max_raw 3 → ValueOutOfLimits', input: '75 30 04' },
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
        { target: 'message:Report', label: () => ln('gmsg::println(report, stdout)', 'guide_report_print(&report, &printer)', 'console.log(String(report))', 'print(report)') + ': enum names with their value, units, nested blocks', input: '5A 02 00 00 00 42 01 6B', view: 'print' },
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
    ['custom', 'custom type', 'declared', 'Your own decode / encode functions for anything GMP does not describe.', '{ name: when, type: bcd_time }'],
    ['pad', 'pad', 'N B', 'Reserved bytes: skipped on decode, zero on encode.', '{ pad: 2 }'],
    ['check', 'crc (field)', '1–8 B', 'A checksum right after one value, verified per field.', '{ name: t, type: u16, crc: crc8_nrsc5 }'],
  ];

  function overview() {
    const step = (kind, title, sub, body) => `<div class="g-step" style="--f:var(--k-${kind});--b:var(--k-${kind}-b)"><div class="g-step-t">${title}</div><div class="g-step-s">${sub}</div>${body ? `<div class="g-step-b">${body}</div>` : ''}</div>`;
    return `
      <p class="g-lead">GMP turns a <b>YAML description of a wire protocol</b> into dependency-free ${ln('C++23', 'C99 / C11 (or C++23)', 'JavaScript (Node.js or the browser)', 'Python 3.12+ (dataclasses, with an optional C accelerator)')} that decodes, validates and
      encodes it. Unlike protobuf the wire format is not fixed: the schema describes <i>existing</i> binary and text protocols,
      from CAN payloads and sensor registers to NMEA sentences and framed serial links.</p>
      <div class="g-pipeline">
        ${step('message', 'schema.yaml', 'messages, enums, protocols', '<code>fields: [ { name: rpm, type: u16 } ]</code>')}
        <div class="g-arrow">→</div>
        ${step('custom', 'gmpc', 'checks &amp; generates', 'file:line:col diagnostics')}
        <div class="g-arrow">→</div>
        ${ln(step('int', 'schema.gmp.hpp', 'one header per schema', '<code>struct</code> · <code>View</code> · <code>Protocol</code>'),
             step('int', 'schema.gmp.h + .c', 'one pair per schema', '<code>struct</code> · <code>_view</code> · <code>_stream</code>'),
             step('int', 'schema.gmp.js', 'one module per schema', '<code>class</code> · <code>View</code> · <code>Protocol</code>'), step('int', 'schema_gmp.py', 'one module per schema', '<code>@dataclass</code> · <code>View</code> · <code>Protocol</code>'))}
        <div class="g-arrow">→</div>
        ${step('string', 'your code', 'decode · encode · stream', ln('<code>Link::Stream&lt;&gt;</code>', '<code>demo_link_stream</code>', '<code>Link.stream()</code>', '<code>Link.stream()</code>'))}
      </div>
      <div class="g-props">
        ${ln(['Header-only runtime', 'No heap', 'No exceptions / RTTI', 'C++23 std::expected', 'Zero-copy views', 'Eager structs', 'Streaming with resync', 'Binary and text'],
             ['Header-only or compiled runtime', 'No heap', 'C99 or later, no C library needed', 'Error codes + out parameters', 'Zero-copy views', 'Eager structs', 'Streaming with resync', 'Binary and text'],
             ['Small runtime (gmsg.js)', 'No dependencies', 'Node.js or browser (UMD)', 'Throws GmpError', 'Zero-copy views', 'Plain classes', 'Streaming with resync', 'Binary and text'], ['Pure-Python runtime (gmsg)', 'No dependencies', 'Optional C accelerator (abi3)', 'Raises GmpError', 'Zero-copy views (memoryview)', 'Dataclasses', 'Streaming with resync', 'Binary and text']).map((t) => `<span class="badge accent">${t}</span>`).join('')}
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
        <div><h4>2 · Generate</h4>${code(ln(`add_subdirectory(generic_message_parser)
gmp_generate(TARGET link_msgs SCHEMAS link.yaml)
target_link_libraries(app PRIVATE link_msgs)`, `add_subdirectory(generic_message_parser)
gmp_generate(TARGET link_msgs LANG c SCHEMAS link.yaml)   # link.gmp.h + link.gmp.c
target_link_libraries(app PRIVATE link_msgs)`, `gmpc --lang js --out web link.yaml     # web/link.gmp.js
cp runtime-js/gmsg.js web/                # the runtime, next to it
# CMake: gmp_generate(TARGET link_js LANG js SCHEMAS link.yaml)`, `pip install ./runtime-py                  # the runtime package gmsg (+ accelerator)
gmpc --lang python --out app link.yaml     # app/link_gmp.py
# CMake: gmp_generate(TARGET link_py LANG python SCHEMAS link.yaml)`), 'text')}
          <h4>3 · Use</h4>${code(ln(`demo::Status s;
s.set_voltage_scaled(12.6);
s.samples = {1, 2, 3};          // count is filled in automatically
std::array<std::uint8_t, demo::Link::max_frame> buf;
auto n = demo::Link::encode(s, buf);

demo::Link::Stream<> stream;    // bytes in any chunk size
stream.feed(chunk, gmsg::overloaded{
    [](const demo::StatusView& v) { v.voltage_scaled(); },
    [](const gmsg::FrameError& e) { /* skipped bytes */ },
});`, `demo_status s = {0};
demo_status_set_voltage_scaled(&s, 12.6);
s.samples[0] = 1; s.samples[1] = 2; s.samples[2] = 3;
s.count = 3;                    // the length field is the array's size
uint8_t buf[DEMO_LINK_MAX_FRAME];
size_t n;
gmsg_error e = demo_link_encode_status(&s, buf, sizeof buf, &n);

static uint8_t mem[DEMO_LINK_MAX_FRAME];
demo_link_stream stream;        // bytes in any chunk size
demo_link_stream_init(&stream, mem, sizeof mem);
stream.cb.on_status = on_status;   // (ctx, header view, const demo_status_view*)
demo_link_stream_feed(&stream, chunk, len);`, `const demo = require('./link.gmp.js');      // browser: <script> tags, global demo
const s = new demo.Status({ samples: [1, 2, 3] });   // count is filled in automatically
s.voltage_scaled = 12.6;
const frame = demo.Link.encode(s);          // Uint8Array

const stream = demo.Link.stream();          // bytes in any chunk size
stream.on('Status', (v) => console.log(v.voltage_scaled));
stream.on('error', (e) => { /* skipped bytes: e.code, e.bytes */ });
stream.feed(chunk);`, `import link_gmp as demo
s = demo.Status(samples=[1, 2, 3])          # count is filled in automatically
s.voltage_scaled = 12.6
frame = demo.Link.encode(s)                 # bytes

stream = demo.Link.stream()                 # bytes in any chunk size
stream.on(demo.Status, lambda v, h: print(v.voltage_scaled))
stream.on('error', lambda e: ...)           # skipped bytes: e.code, e.data
stream.feed(chunk)`))}</div>
      </div>`;
  }

  function anatomy() {
    return `
      <p>A schema has two layers. <b>Messages</b> describe a payload: an ordered list of fields, usable on their own
      (${ln('<code>Status::decode(bytes)</code>', '<code>demo_status_decode(data, size, &amp;s)</code>', '<code>demo.Status.decode(bytes)</code>', '<code>demo.Status.decode(data)</code>')}). <b>Protocols</b> wrap messages in frames on a byte stream:</p>
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
          ['namespace', 'acme::telemetry', ln('C++ namespace of the generated code.', 'Name prefix of the generated C code: <code>acme_telemetry_</code> (or <code>gmpc --opt prefix=…</code>).', 'Browser global of the generated module: <code>acme_telemetry</code> (or <code>gmpc --opt global=…</code>); with <code>require()</code> the module is what it returns.', 'Nothing in the code: the module is named after the schema file (<code>telemetry_gmp</code>); the namespace is in its docstring.')],
          ['defaults', '{ endian: big, … }', 'endian, bit_order, format, separator, access, strict_enums for everything below.'],
          ['enums', 'Mode: { … }', 'Named integer values, used as field types and type ids.'],
          ['custom', 'types / checksums / framers', ln('Hooks into your own C++ code, checked with concepts.', 'Hooks into your own C code (<code>c:</code> blocks), signatures checked at compile time.', 'Hooks into your own JavaScript (<code>js:</code> blocks: a module and the names of its functions).', 'Hooks into your own Python (<code>python:</code> blocks: a module and the names of its functions).')],
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
      <p>Enums map numbers to names (${ln('<code>to_string()</code>, <code>is_known()</code>', '<code>_to_string()</code>, <code>_is_known()</code>', '<code>Mode.nameOf()</code>, <code>Mode.isKnown()</code>', '<code>Mode.name_of()</code>, <code>Mode.is_known()</code>; fields hold <code>Mode</code> members, an <code>IntEnum</code>')}). With <code>strict: true</code> (or
      <code>defaults.strict_enums</code>) undeclared values fail with <code>UnknownEnum</code>, on decode and on encode.
      A <code>bits</code> group packs bit fields into whole bytes; each bit field can be a bool, an integer, an enum, a const or scaled.</p>
      ${demo('enums')}
      <h3 id="g-limits">Value limits</h3>
      <p>A CRC only proves the bytes arrived intact, not that the value makes sense. <code>min_value</code> / <code>max_value</code>
      limit the value as your code sees it: the scaled value (raw × scale + offset) when the field has a scale or offset,
      else the value itself. <code>min_raw</code> / <code>max_raw</code> limit the raw wire value. Limits are inclusive and can be
      combined. Outside them, decoding (struct and view) and encoding fail with <code>${ln('ValueOutOfLimits', 'GMSG_ERR_VALUE_OUT_OF_LIMITS', 'gmsg.Err.ValueOutOfLimits', 'gmsg.Err.VALUE_OUT_OF_LIMITS')}</code>;
      a NaN always fails. They apply to numeric fields, bit fields, array elements and text tokens.</p>
      ${note('<code>max</code> is not a value limit: it is only the capacity of a variable-length bytes, string or array field. gmpc reports <code>max</code> anywhere else (and a <code>max</code> that does not match a fixed size) as an error.')}
      ${demo('limits')}
      <h3 id="g-containers">Strings, bytes and arrays</h3>
      <p>Variable-size fields get their length from a number, an earlier field or expression (<code>length: n * 2</code>),
      <code>remaining</code>, or a <code>terminator</code>. <code>max</code> is the capacity of the fixed-size container in the struct, so nothing allocates
      (it is not a value limit: see <a href="#g-limits">Value limits</a>).
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
      is true (${ln('it becomes <code>std::optional</code>', 'the struct gets <code>bool has_&lt;field&gt;</code>', 'the property is <code>undefined</code> when absent', 'the attribute is <code>None</code> when absent')}). <code>const</code> fields are checked on decode and always written on encode.</p>
      ${demo('structure')}`;
  }

  function values() {
    return `
      <p>Fields keep their raw value; GMP adds engineering accessors around it.</p>
      <div class="g-formula">
        <div><span class="badge">raw</span> <code>0x6666</code></div><div class="g-arrow">→</div>
        <div><code>× scale + offset</code></div><div class="g-arrow">→</div>
        <div><span class="badge accent">${ln('temperature_scaled()', '…_temperature_scaled(&amp;m)', 'temperature_scaled', 'temperature_scaled')}</span> <code>25.00 degC</code></div>
        <div class="g-arrow">+</div><div><span class="badge accent">${ln('temperature_degf()', '…_temperature_degf(&amp;m)', 'temperature_degf', 'temperature_degf')}</span> <code>77.00 degF</code></div>
      </div>
      ${table(['key', 'generates', 'notes'], [
        [kbd('scale / offset / unit'), ln('<code>x_scaled()</code>, <code>set_x_scaled(v)</code>', '<code>…_x_scaled(&amp;m)</code>, <code>…_set_x_scaled(&amp;m, v)</code>', '<code>x_scaled</code> (getter and setter)', '<code>x_scaled</code> (property with a setter)'), 'value = raw × scale + offset'],
        [kbd('conversions: { degf: {…} }'), ln('<code>x_degf()</code>, <code>set_x_degf(v)</code>', '<code>…_x_degf(&amp;m)</code>, <code>…_set_x_degf(&amp;m, v)</code>', '<code>x_degf</code> (getter and setter)', '<code>x_degf</code> (property with a setter)'), 'extra units for the same raw value'],
        [kbd('crc: crc8_nrsc5'), ln('verified in read / from(), written by encode', 'verified in _read / _view_from, written by _encode', 'verified in read / from(), written by encode', 'verified in read / from_bytes(), written by encode'), 'checksum stored right after the value'],
        [kbd('computed: { serial: {…} }'), ln('<code>serial()</code>, <code>set_serial(v)</code>', '<code>…_serial(&amp;m)</code>, <code>…_set_serial(&amp;m, v)</code>', '<code>serial</code> (getter and setter)', '<code>serial</code> (property with a setter)'), 'no bytes of its own; setter derived for shift / | packing'],
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
        <div><h4>View: zero copy</h4><p>Wraps the input bytes; accessors read on demand. ${ln('<code>from()</code>', '<code>_view_from()</code>', '<code>from()</code>', '<code>from_bytes()</code>')} validates once.</p>
          ${code(ln(`auto v = demo::StatusView::from(bytes);
if (!v) return v.error();          // gmsg::Error
double volts = v->voltage_scaled();
for (auto s : v->samples()) use(s);`, `demo_status_view v;
gmsg_error e = demo_status_view_from(data, size, &v);
if (e != GMSG_OK) return e;        // GMSG_ERR_...
double volts = demo_status_view_voltage_scaled(&v);
for (size_t i = 0; i < demo_status_view_samples_count(&v); ++i)
    use(demo_status_view_samples_at(&v, i));`, `const v = demo.StatusView.from(bytes);   // throws gmsg.GmpError
const volts = v.voltage_scaled;
for (const s of v.samples) use(s);`, `v = demo.StatusView.from_bytes(data)     # raises gmsg.GmpError
volts = v.voltage_scaled
for s in v.samples: use(s)`))}</div>
        <div><h4>Struct: eager value</h4><p>${ln('Fixed-capacity members, <code>operator==</code>, and the only way to encode.', 'Fixed-capacity members (arrays with their count, <code>has_&lt;field&gt;</code> for conditions), and the only way to encode.', 'Plain classes (arrays, strings, Uint8Array; <code>undefined</code> for absent fields), and the only way to encode.', 'Dataclasses (lists, str, bytes; <code>None</code> for absent fields; <code>==</code> and <code>repr</code>), and the only way to encode.')}</p>
          ${code(ln(`demo::Status s;
s.set_voltage_scaled(12.6);
s.samples = {1, 2, 3};
std::array<std::uint8_t, demo::Status::max_wire_size> out;
auto n = s.encode(out);            // gmsg::Result<size_t>`, `demo_status s = {0};
demo_status_set_voltage_scaled(&s, 12.6);
s.samples[0] = 1; s.samples[1] = 2; s.samples[2] = 3;
s.count = 3;
uint8_t out[DEMO_STATUS_MAX_WIRE_SIZE];
size_t n;
gmsg_error e = demo_status_encode(&s, out, sizeof out, &n);`, `const s = new demo.Status();
s.voltage_scaled = 12.6;
s.samples = [1, 2, 3];
const out = s.encode();            // Uint8Array; throws gmsg.GmpError`, `s = demo.Status()
s.voltage_scaled = 12.6
s.samples = [1, 2, 3]
out = s.encode()                  # bytes; raises gmsg.GmpError`))}</div>
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
        [kbd('custom'), 'your framer', ln('<code>static gmsg::FrameScan scan(gmsg::bytes_view)</code>', '<code>gmsg_frame_scan scan(const uint8_t*, size_t)</code>', '<code>scan(bytes)</code> returning <code>gmsg.FrameScan.*</code>', '<code>scan(buf)</code> returning <code>gmsg.FrameScan.*</code>')],
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
      ${note(ln('In C++, <code>parse()</code> returns <code>AnyView</code>, a <code>std::variant</code> of the message views, and a stream handler is called with whichever view matched.',
                'In C, <code>_parse()</code> fills <code>_any_view</code>, a tagged union (<code>kind</code> plus <code>as.&lt;message&gt;</code>), and the stream calls the <code>on_&lt;message&gt;</code> callback of whichever view matched.',
                'In JavaScript, <code>parse()</code> returns the view of whichever message matched (check it with <code>instanceof</code>), and stream listeners are registered per message name.', 'In Python, <code>parse()</code> returns the view of whichever message matched (check it with <code>isinstance</code>), and stream listeners are registered per message class or name.'))}`;
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
      <p>Text messages are tokens separated by a character. Numbers are parsed ${ln('with <code>std::from_chars</code>', 'exactly and locale free (the same results as C++ <code>std::from_chars</code>)', 'exactly and locale free (the same results as C++ <code>std::from_chars</code>)', 'exactly and locale free (the same results as C++ <code>std::from_chars</code>)')};
      <code>width</code> and <code>precision</code> control encoding, <code>optional</code> allows empty tokens, and
      <code>pad</code> skips tokens.</p>
      ${demo('nmea')}`;
  }

  function streaming() {
    return ln(streamingCpp(), streamingC(), streamingJs(), streamingPy()) + `
      <h3 id="g-recover">Recovering on an idle link</h3>
      <p>A corrupted length field that claims more bytes than will arrive makes the parser wait, and good frames behind it
      wait too. ${ln('<code>resync()</code>', '<code>demo_link_stream_resync(&amp;s)</code>', '<code>resync()</code>', '<code>resync()</code>')} gives up on an
      incomplete frame at the head (reported as <code>Truncated</code>) so the next scan finds them;
      ${ln('<code>flush()</code>', '<code>demo_link_stream_flush(&amp;s)</code>', '<code>flush()</code>', '<code>flush()</code>')} drops everything buffered.
      With ${ln('<code>set_idle_timeout(t)</code>', '<code>demo_link_stream_set_idle_timeout(&amp;s, t)</code>', '<code>setIdleTimeout(t)</code>', '<code>set_idle_timeout(t)</code>')}
      and a periodic ${ln('<code>tick(now)</code>', '<code>demo_link_stream_tick(&amp;s, now)</code>', '<code>tick(Date.now())</code>', '<code>tick(time.monotonic())</code>')}
      this happens automatically after <code>t</code> without new bytes (any time unit; the parser has no clock). They only drop
      data: frames that become complete come from the next ${ln('<code>next()</code> / <code>feed()</code> / <code>poll()</code>', '<code>_next</code> / <code>_feed</code> / poll call', '<code>next()</code> / <code>feed()</code> / <code>poll()</code>', '<code>next()</code> / <code>feed()</code> / <code>poll()</code>')}.</p>
      <p>For <code>length_prefixed</code> framing the scanner also checks the length against the frame's message type as soon as
      the header has arrived: a body shorter than the message (or, with <code>exact_size</code>, longer) is rejected with
      <code>BadLength</code> at once.</p>
      <h3 id="g-ring">Interrupt input: a byte ring</h3>
      <p>${ln('Bytes that arrive in an interrupt should only be stored there; decoding and callbacks belong in the main loop. The interrupt pushes into a <code>gmsg::ByteRing&lt;N&gt;</code> (a lock-free single-producer / single-consumer queue, N a power of two) and the main loop drains it:',
        'Bytes that arrive in an interrupt should only be stored there; decoding and callbacks belong in the main loop. The interrupt pushes into a <code>gmsg_ring</code> (<code>gmp/c/ring.h</code>, a lock-free single-producer / single-consumer queue over your buffer, a power of two) and the main loop drains it:',
        'A <code>gmsg.ByteRing</code> is a single-producer / single-consumer byte queue. With <code>{ shared: true }</code> its storage is a <code>SharedArrayBuffer</code>, so a Worker can receive (<code>gmsg.ByteRing.attach(ring.buffer)</code>) while another thread parses:',
        'A <code>gmsg.ByteRing</code> is a single-producer / single-consumer byte queue: a reader thread fills it and the parsing thread drains it:')}</p>
      ${ln(code(`gmsg::ByteRing<256> rx;
void uart_isr() { rx.push(UART->DR); }          // producer: store only

for (;;) parser.feed(rx);                       // consumer: decode + callbacks here`),
code(`static uint8_t rx_buf[256];
static gmsg_ring rx;                            /* gmsg_ring_init(&rx, rx_buf, sizeof rx_buf) */
void uart_isr(void) { gmsg_ring_push(&rx, (uint8_t)UART->DR); }

for (;;) demo_link_stream_feed_ring(&s, &rx, SIZE_MAX);`, 'c'),
code(`const rx = new gmsg.ByteRing(4096, { shared: true });
worker.postMessage(rx.buffer);   // in the worker: gmsg.ByteRing.attach(buffer).pushBytes(chunk)

parser.feed(rx);                 // drain and deliver`, 'js'),
code(`rx = gmsg.ByteRing(4096)
reader = threading.Thread(target=lambda: [rx.push_bytes(port.read(64)) for _ in iter(int, 1)])
reader.start()

parser.feed(rx)                  # drain and deliver`, 'py'))}
      <p>A full ring drops the newest bytes (${ln('<code>rx.dropped()</code>', '<code>gmsg_ring_dropped(&amp;rx)</code>', '<code>rx.dropped</code>', '<code>rx.dropped</code>')})
      and keeps dropping until the main loop has read everything before the gap. The parser then delivers the frames complete
      before the gap, drops the frame in progress and reports it as
      <code>${ln('Overrun', 'GMSG_ERR_OVERRUN', 'gmsg.Err.Overrun', 'gmsg.Err.OVERRUN')}</code> (counted in
      ${ln('<code>stats().overruns</code>', '<code>stats.overruns</code>', '<code>stats.overruns</code>', '<code>stats.overruns</code>')}).
      ${ln('<code>rx.high_water()</code>', '<code>gmsg_ring_high_water(&amp;rx)</code>', '<code>rx.highWater</code>', '<code>rx.high_water</code>')} shows how full the
      ring got.${ln(' On 8- and 16-bit MCUs use a narrower index: <code>gmsg::ByteRing&lt;128, std::uint8_t&gt;</code>.', ' On 8- and 16-bit MCUs set <code>GMSG_RING_INDEX_BITS</code> to 8 or 16; without C11 atomics the indices are ordered by <code>GMSG_RING_FENCE()</code>.', '', '')}</p>
      <h3 id="g-extra">Data after the message</h3>
      <p>A frame can carry more than its message uses (a longer length, extra text tokens). By default it is ignored, so
      newer senders that append fields still work, and reported:
      ${ln('<code>v.extra_bytes()</code> / <code>extra_tokens()</code>, <code>Received::extra</code>, <code>stats().frames_with_extra</code>, <code>Status::decode(bytes, extra)</code>',
        '<code>_view_extra_bytes(&amp;v)</code>, <code>received.extra</code>, <code>stats.frames_with_extra</code>, <code>demo_status_decode_extra(...)</code>',
        '<code>v.extraBytes</code> / <code>extraTokens</code>, <code>frame.extra</code>, <code>stats.framesWithExtra</code>, <code>Status.decodeWithExtra(bytes)</code>', '<code>v.extra_bytes</code> / <code>extra_tokens</code>, <code>frame.extra</code>, <code>stats.frames_with_extra</code>, <code>Status.decode_with_extra(data)</code>')}.
      With <code>exact_size: true</code> on the protocol, such frames fail with <code>BadLength</code>.</p>`;
  }


  function streamingJs() {
    return `
      <p><code>Link.stream()</code> is a <code>gmsg.StreamParser</code> with a buffer of <code>maxFrame</code> bytes. Feed it
      chunks of any size; it finds frames, validates them and calls your listeners with the matching view.</p>
      <div class="g-flow">
        ${['bytes in (any chunking)', 'scan: find sync / delimiter / length', 'open: checksum, unescape', 'select message by type id', "listeners: on('Status', fn)"].map((t, i) =>
          `<div class="g-flow-s"><span>${i + 1}</span>${esc(t)}</div>`).join('<div class="g-arrow">→</div>')}
      </div>
      ${code(`const parser = demo.Link.stream();                 // or demo.Link.stream({ capacity: 4096 })
parser.on('Status', (v, header) => app.onStatus(v));  // one or more listeners per message
parser.on('frame', (v) => log(String(v)));            // every valid frame, after its message listeners
parser.on('error', (e) => drop(e.code, e.bytes));      // garbage, bad checksums, unknown ids (gmsg.GmpError)
parser.feed(chunk);                                    // returns the number of frames delivered
const st = parser.stats;                               // frames, frameErrors, bytesSkipped, overflows`)}
      ${note("<code>on()</code> returns a function that removes the listener. The listener's second argument is the header view (for protocols with a header).")}
      <h3>Pulling messages</h3>
      <p>Or ask for the next message yourself, which suits a request/response exchange. Polled views own a copy of
      their frame, so they stay valid.</p>
      ${code(`parser.push(chunk);
let m;
while ((m = parser.poll())) {                          // null: more input needed
    if (m instanceof demo.StatusView) use(m);
}
for (const m of parser.messages(chunk)) use(m);        // the same as an iterator
const r = parser.pollFrame();      // { header, message, bytes, offset }
const s = parser.pollMessage();    // decoded class instance
const e = parser.pollEvent();      // { message, ... } or { error } in stream order
const reply = parser.pollFor('Status');               // others go to the listeners meanwhile`)}
      ${demo('stream')}`;
  }

  function streamingPy() {
    return `
      <p><code>Link.stream()</code> is a <code>gmsg.StreamParser</code> with a buffer of <code>max_frame</code> bytes. Feed it
      chunks of any size; it finds frames, validates them and calls your listeners with the matching view.</p>
      <div class="g-flow">
        ${['bytes in (any chunking)', 'scan: find sync / delimiter / length', 'open: checksum, unescape', 'select message by type id', 'listeners: on(Status, fn)'].map((t, i) =>
          `<div class="g-flow-s"><span>${i + 1}</span>${esc(t)}</div>`).join('<div class="g-arrow">→</div>')}
      </div>
      ${code(`parser = demo.Link.stream()                            # or demo.Link.stream(capacity=4096)
parser.on(demo.Status, lambda v, header: app.on_status(v))  # one or more listeners per message (class or name)
parser.on('frame', lambda v, header: log.debug('%s', v))     # every valid frame, after its message listeners
parser.on('error', lambda e: drop(e.code, e.data))           # garbage, bad checksums, unknown ids (gmsg.GmpError)
parser.feed(chunk)                                          # returns the number of frames delivered
st = parser.stats                                           # frames, frame_errors, bytes_skipped, overflows`)}
      ${note("<code>on()</code> returns a function that removes the listener. The listener's second argument is the header view (<code>None</code> without a header).")}
      <h3>Pulling messages</h3>
      <p>Or ask for the next message yourself, which suits a request/response exchange. Views from the parser own a copy
      of their frame, so they stay valid.</p>
      ${code(`parser.push(chunk)
while (m := parser.poll()) is not None:                # None: more input needed
    if isinstance(m, demo.StatusView): use(m)
for m in parser.messages(chunk): use(m)                 # push + poll as an iterator
r = parser.poll_frame()      # gmsg.Received(message, header, bytes, offset, extra)
s = parser.poll_message()    # decoded dataclass
e = parser.poll_event()      # gmsg.Received or gmsg.GmpError, in stream order
reply = parser.poll_for(demo.Status)                   # others go to the listeners meanwhile`)}
      ${demo('stream')}`;
  }

  function generatedPy() {
    return `
      <p><code>gmpc --lang python</code> writes one module per schema, <code>&lt;schema&gt;_gmp.py</code> (Python 3.12+), built on
      the runtime package <code>gmsg</code> (<code>pip install ./runtime-py</code>: pure Python, plus an optional C accelerator
      built from the C runtime when a compiler is available; the results are the same). Messages are dataclasses, integers
      are <code>int</code>, strings hold one character per byte, bytes are <code>bytes</code> (<code>memoryview</code> in views),
      and absent fields are <code>None</code>. The behaviour and the printed text are those of C++ (cross-checked with and
      without the accelerator). See README "Python output".</p>
      ${table(['generated', 'what it is'], [
        ['<code>@dataclass class M</code>', 'Attributes per field, <code>decode()</code>, <code>decode_with_extra()</code>, <code>encode()</code>, <code>read()</code> / <code>write()</code>, scaled / conversion / computed properties, <code>__str__</code> / <code>to_text()</code>, <code>==</code>.'],
        ['<code>class MView</code>', '<code>from_bytes(data)</code> (text: <code>from_text(s)</code>) validates once; properties read from a <code>memoryview</code>. <code>to_struct()</code> copies out; <code>ranges()</code> gives the bytes of each field.'],
        ['enum <code>E</code>', 'A <code>gmsg.GmpEnum</code> (an <code>IntEnum</code>) with <code>name_of()</code>, <code>is_known()</code> and <code>of()</code>; names that are Python keywords get a <code>_</code>.'],
        ['<code>print_to()</code>, <code>to_text()</code>', 'On dataclasses, views and bit groups: the text of <code>gmsg.to_text</code> (see Printing for logs).'],
        ['<code>class P</code>', '<code>parse()</code> → view, <code>decode()</code> → dataclass, <code>encode(m, header)</code>, <code>stream()</code>, <code>open()</code> / <code>select()</code> / <code>process()</code>, layout constants.'],
      ])}
      <h3>Errors</h3>
      <p>Failures raise <code>gmsg.GmpError</code>; <code>e.code</code> is one of <code>gmsg.Err</code> (an <code>IntEnum</code>) and <code>str(e)</code> its text.</p>
      ${errorTable()}`;
  }

  function generatedJs() {
    return `
      <p><code>gmpc --lang js</code> writes one module per schema, <code>&lt;schema&gt;.gmp.js</code>, built on the runtime
      <code>runtime-js/gmsg.js</code> (no dependencies). It loads with <code>require()</code> in Node.js, with a bundler, or
      with <code>&lt;script&gt;</code> tags in a browser (global <code>demo</code>, from the namespace). Values are Numbers
      (BigInt for 64-bit types), strings hold one character per byte, bytes are <code>Uint8Array</code>, and absent fields are
      <code>undefined</code>. The behaviour and the printed text are those of C++ (cross-checked). See README "JavaScript output".</p>
      ${table(['generated', 'what it is'], [
        ['<code>class M</code>', 'Properties per field, <code>decode()</code>, <code>encode()</code>, <code>read()</code> / <code>write()</code>, scaled / conversion / computed getters and setters, <code>toString()</code>. Encode also accepts plain objects.'],
        ['<code>class MView</code>', '<code>from(bytes)</code> validates once; getters read from the buffer. <code>toStruct()</code> copies out; <code>ranges()</code> gives the bytes of each field.'],
        ['enum <code>E</code>', 'A frozen <code>{ name: value }</code> object with <code>nameOf()</code> and <code>isKnown()</code>.'],
        ['<code>printTo()</code>, <code>toString()</code>', 'On classes, views and bit groups: the text of <code>gmsg.print</code> (see Printing for logs).'],
        ['<code>class P</code>', '<code>parse()</code> → view, <code>decode()</code> → message, <code>encode(m, header)</code>, <code>stream()</code>, <code>open()</code> / <code>select()</code> / <code>process()</code>, layout constants.'],
      ])}
      <h3>Errors</h3>
      <p>Failures throw <code>gmsg.GmpError</code>; <code>e.code</code> is one of <code>gmsg.Err</code> and <code>e.message</code> its text.</p>
      ${errorTable()}`;
  }

  function streamingC() {
    return `
      <p><code>P_stream</code> works in a buffer you provide (at least <code>P_MAX_FRAME</code> bytes, no heap). Feed it
      chunks of any size; it finds frames, validates them and calls your callbacks with the matching view.</p>
      <div class="g-flow">
        ${['bytes in (any chunking)', 'scan: find sync / delimiter / length', 'open: checksum, unescape', 'select message by type id', 'on_<message>(ctx, view)'].map((t, i) =>
          `<div class="g-flow-s"><span>${i + 1}</span>${esc(t)}</div>`).join('<div class="g-arrow">→</div>')}
      </div>
      ${code(`static uint8_t buf[DEMO_LINK_MAX_FRAME];
demo_link_stream parser;
demo_link_stream_init(&parser, buf, sizeof buf);   // GMSG_ERR_BUFFER_FULL if too small
parser.cb.ctx = &app;                       // passed to every callback
parser.cb.on_status = on_status;            // (ctx, const demo_link_header_view*, const demo_status_view*)
parser.cb.on_frame = on_frame;              // (ctx, header, const demo_link_any_view*): every valid frame
parser.cb.on_error = on_drop;               // (ctx, gmsg_error, const uint8_t* data, size_t size)
demo_link_stream_feed(&parser, chunk, n);   // returns the number of frames delivered
const gmsg_stream_stats* st = demo_link_stream_stats(&parser);   // frames, frame_errors, bytes_skipped, overflows`)}
      <h3>Callbacks</h3>
      <p>The callbacks live in the stream (<code>parser.cb</code>), so the code that receives bytes (a UART driver, a socket
      loop) only calls <code>_stream_feed()</code>. One <code>void* ctx</code> is passed to every callback; NULL slots are
      skipped. For each frame the message's callback runs first, then <code>on_frame</code>. Views are valid only during the call.
      <code>demo_link_process(data, size, &amp;cb)</code> does the same for one whole frame.</p>
      <h3>Pulling messages</h3>
      <p>Or ask for the next message yourself, which suits a main loop or a request/response exchange. A polled
      view is valid until the parser is used again (<code>_stream_generation()</code> changes when it is).</p>
      ${code(`demo_link_stream_push(&parser, chunk, n);   // returns the bytes accepted
demo_link_any_view m;
while (demo_link_stream_poll(&parser, &m)) {           // false: more input needed
    if (m.kind == DEMO_LINK_STATUS) use(&m.as.status);
}
demo_link_received r;     demo_link_stream_poll_frame(&parser, &r);     // header, message, bytes
demo_link_any_message d;  demo_link_stream_poll_message(&parser, &d);   // decoded struct copy
demo_link_event ev;       demo_link_stream_poll_event(&parser, &ev);    // GMSG_POLL_MESSAGE / _ERROR / _NEED_MORE
demo_link_stream_poll_for(&parser, DEMO_LINK_STATUS, &m);   // others go to the callbacks meanwhile`)}
      ${demo('stream')}`;
  }

  function streamingCpp() {
    return `
      <p><code>Protocol::Stream&lt;&gt;</code> owns a fixed buffer of <code>max_frame</code> bytes. Feed it chunks of any
      size; it finds frames, validates them and calls your handler with the matching view.</p>
      <div class="g-flow">
        ${['bytes in (any chunking)', 'scan: find sync / delimiter / length', 'open: checksum, unescape', 'select message by type id', 'handler(View)'].map((t, i) =>
          `<div class="g-flow-s"><span>${i + 1}</span>${t}</div>`).join('<div class="g-arrow">→</div>')}
      </div>
      ${code(`demo::Link::Stream<> parser;
parser.feed(chunk, gmsg::overloaded{
    [](const demo::Link::HeaderView& h, const demo::StatusView& v) { /* header + message */ },
    [](const demo::SamplesView& v) { /* message only */ },
    [](const gmsg::FrameError& e) { /* optional: e.error, e.data */ },
});
auto& st = parser.stats();   // frames, frame_errors, bytes_skipped, overflows`)}
      <h3>Storing the handler</h3>
      <p>The parser can keep the handler, so the code that receives bytes (a UART driver, a socket loop) only calls
      <code>parser.feed(bytes)</code>. <code>make_stream</code> stores an overload set with no runtime cost.
      <code>CallbackStream</code> holds runtime listeners. Its type doesn't depend on the handlers, so it can be a
      global or a class member, and listeners can be changed while running.</p>
      ${code(`auto parser = demo::Link::make_stream(gmsg::overloaded{   // compile-time handler
    [&](const demo::StatusView& v) { app.on_status(v); },
});
parser.feed(chunk);

demo::Link::CallbackStream<> link;                        // runtime callbacks
link.on<&App::on_status>(app)      // (StatusView): one listener per message
    .on<&App::on_command>(app)     // (Link::HeaderView, CommandView): header only when asked for
    .on<&App::on_frame>(app)       // (Link::AnyView): every valid frame, after the message listener
    .on<&App::on_drop>(app);       // (gmsg::FrameError): garbage, bad checksums, unknown ids
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
auto e = parser.poll_event();      // message or gmsg::FrameError, in stream order
auto reply = link.poll_for<demo::StatusView>();   // others go to the listeners meanwhile`)}
      ${demo('stream')}`;
  }

  function printing() {
    return `
      <p>${ln('Every generated struct, view and bit group has <code>print_to()</code>, so <code>gmsg::print</code> can dump it',
             'Every generated struct, view and bit group has <code>_print(&amp;m, &amp;printer)</code> (<code>gmp/c/print.h</code>), with the same text as C++, to dump it',
             'Every generated class, view and bit group has <code>printTo()</code> and <code>toString()</code>, so <code>gmsg.print</code> gives the same text as C++', 'Every generated dataclass, view and bit group has <code>print_to()</code>, <code>to_text()</code> and <code>__str__</code>, so <code>print(m)</code> gives the same text as C++')}
      for a console or a debug log. Enums print as <code>name (0xNN)</code> and undeclared values as
      <code>&lt;unknown&gt; (0x07)</code>, so the raw value is always visible. Scaled fields show the raw value followed
      by the engineering value and unit. Bytes print as hex, strings are quoted and escaped, and missing conditional fields
      print as <code>&lt;absent&gt;</code>. A view prints exactly like its struct.</p>
      ${code(ln(`gmsg::println(status, stdout);                       // FILE*: console or fopen'ed log
gmsg::print(status, std::cout);                         // anything with write(): std::ostream
char line[256];  gmsg::print(status, line);            // char buffer, NUL terminated
gmsg::static_string<256> s;  gmsg::print(status, s);    // appends; std::string works too
gmsg::print(status, [](std::string_view t) { uart_log(t); });   // callback

gmsg::print(status, stdout, {.style = gmsg::PrintStyle::single_line});
// options: style, indent, max_items, max_bytes (0 = all), precision (significant digits of scaled values)

// Stream handlers can print the view directly, and Protocol::parse() results print whichever message matched
stream.feed(chunk, [](const demo::StatusView& v) { gmsg::println(v, stdout); });`, `gmsg_printer p;
gmsg_printer_init_file(&p, stdout, NULL);           // FILE*: console or fopen'ed log
demo_status_print(&status, &p);
gmsg_printer_finish(&p, true, NULL);                // true: end with a newline

char line[256];
gmsg_printer_init_buffer(&p, line, sizeof line, NULL);   // char buffer, NUL terminated
gmsg_printer_init(&p, uart_write, &uart, NULL);     // bool uart_write(void* ctx, const char* s, size_t n)

gmsg_print_options o = gmsg_print_defaults();
o.style = GMSG_PRINT_SINGLE_LINE;
// options: style, indent, max_items, max_bytes (0 = all), precision (significant digits of scaled values)

// views print like structs, and a parsed frame prints whichever message it holds
demo_link_any_view_print(&any, &p);`, `console.log(String(status));                       // multi-line text (the same as C++)
console.log(gmsg.print(status, { style: 'single_line' }));
// options: style, indent, maxItems, maxBytes (0 = all), precision (significant digits of scaled values)

// views print like their class, and Link.parse() results print whichever message matched
stream.on('frame', (v) => log(String(v)));`, `print(status)                                      # multi-line text (the same as C++)
print(status.to_text(style='single_line'))
# options: style, indent, max_items, max_bytes (0 = all), precision (significant digits of scaled values)

# views print like their dataclass, and Link.parse() results print whichever message matched
stream.on('frame', lambda v, h: log.info('%s', v))`))}
      ${table(['field', 'printed as'], [
        ['integer, float, bool, char', '<code>42</code>, <code>1.5</code>, <code>true</code>, <code>\'N\'</code>'],
        ['enum', '<code>run (0x01)</code>, <code>&lt;unknown&gt; (0x07)</code>'],
        ['scale / unit / conversions', '<code>1234 (12.34 V)</code>, <code>26214 (25 degC, 77 degF)</code>, <code>12 mA</code> (unit, no scale)'],
        ['bits group, nested message', 'an indented <code>{ … }</code> block'],
        ['bytes, string', '<code>[0A 1B 2C]</code>, <code>"text\\r\\n"</code>'],
        ['array', '<code>[1, 2, 3]</code>; messages one per line; <code>... (+N more)</code> past <code>max_items</code>'],
        ['conditional field', 'its value, or <code>&lt;absent&gt;</code>'],
        ['computed value', '<code>serial: 305419896 (computed)</code>'],
        ['custom type', ln('your <code>gmsg_print(gmsg::Printer&amp;, const T&amp;)</code> next to the type, else <code>&lt;?&gt;</code>',
                           'the <code>print</code> function of its <code>c:</code> block, <code>void fn(gmsg_printer*, const T*)</code>, else <code>&lt;?&gt;</code>',
                           'the <code>print</code> function of its <code>js:</code> block, <code>print(printer, value)</code>; else numbers and strings print as such, other values as <code>&lt;?&gt;</code>', 'the <code>print</code> function of its <code>python:</code> block, <code>print(printer, value)</code>; else numbers and strings print as such, other values as <code>&lt;?&gt;</code>')],
      ])}
      ${note(ln('Bounded sinks (char buffers, static_string, or a callback that returns false) keep what fits and return <code>BufferFull</code>.',
                'Bounded sinks (char buffers, or a write function that returns false) keep what fits; <code>gmsg_printer_finish()</code> returns <code>GMSG_ERR_BUFFER_FULL</code>.',
                'Printing returns a string.', 'Printing returns a string.') +
             ' Printing never allocates. In the Studio decode tester, the <b>View</b> switch shows the same text for every decoded frame.')}
      ${demo('printing')}`;
  }

  const ERRORS = [
    ['Truncated', 'TRUNCATED', 'Input ended before the message or frame was complete.'],
    ['BadSync', 'BAD_SYNC', 'Bytes before a sync word / start delimiter were skipped.'],
    ['BadChecksum', 'BAD_CHECKSUM', 'Frame checksum or field crc mismatch (or a missing text checksum).'],
    ['BadLength', 'BAD_LENGTH', 'Length out of range, frame larger than max_frame, or an escaped length split.'],
    ['BadDelimiter', 'BAD_DELIMITER', 'A start delimiter or reserved byte appeared inside a frame.'],
    ['BadConst', 'BAD_CONST', 'A const field or bit field did not match.'],
    ['OutOfRange', 'OUT_OF_RANGE', 'More data than a container\'s max, or a value that does not fit.'],
    ['UnknownType', 'UNKNOWN_TYPE', 'No message has the frame\'s type id.'],
    ['UnknownEnum', 'UNKNOWN_ENUM', 'A strict enum field holds an undeclared value.'],
    ['ValueOutOfLimits', 'VALUE_OUT_OF_LIMITS', 'A value is outside its min_value / max_value / min_raw / max_raw.'],
    ['Overrun', 'OVERRUN', 'Input bytes were lost before parsing (a full byte ring dropped them); the frame in progress is dropped.'],
    ['BufferFull', 'BUFFER_FULL', 'Output buffer too small, or stream buffer full without a frame.'],
    ['ParseError / MissingField', 'PARSE / MISSING_FIELD', 'A text token is invalid / a required token is empty.'],
    ['InvalidValue / Custom', 'INVALID_VALUE / CUSTOM', 'For custom codecs (also: an unknown escape code).'],
  ];
  const errorTable = () => table(['error', 'meaning'], ERRORS.map(([cpp, c, d]) =>
    [ln(`<code>${cpp}</code>`, c.split(' / ').map((x) => `<code>GMSG_ERR_${x}</code>`).join(' / '),
      cpp.split(' / ').map((x) => `<code>gmsg.Err.${x}</code>`).join(' / '), cpp.split(' / ').map((x) => `<code>gmsg.Err.${x.replace(/([a-z])([A-Z])/g, '$1_$2').toUpperCase()}</code>`).join(' / ')), d]));

  function generatedC() {
    return `
      <p>Names come from the schema's namespace: <code>namespace: demo</code> gives <code>demo_status</code>,
      <code>demo_status_view</code>, <code>DEMO_MODE_RUN</code>; <code>gmpc --lang c --opt prefix=my_</code> picks another prefix.
      The headers also compile as C++. See README "C output".</p>
      <p>Any C99 or later compiler works (C11 adds compile-time checks of the functions in <code>c:</code> blocks).
      The runtime is header-only (<code>gmp::runtime_c</code>), or a compiled library with CRC lookup tables:
      <code>gmp_generate(... LANG c C_RUNTIME library)</code> links <code>gmp::runtime_c_lib</code>.</p>
      ${table(['generated', 'what it is'], [
        ['<code>P_m</code> struct', 'Members per field (<code>has_&lt;field&gt;</code> when conditional, arrays and strings with their length), <code>_decode()</code>, <code>_encode()</code>, <code>_read()</code> / <code>_write()</code>, scaled / conversion / computed accessors.'],
        ['<code>P_m_view</code>', '<code>_view_from(data, size, &amp;v)</code> validates once; accessor functions read from the buffer. <code>_view_to_struct()</code> copies out.'],
        ['enum <code>P_e</code>', 'A typedef of the integer type, <code>P_E_VALUE</code> constants, <code>_to_string()</code> (NULL when undeclared), <code>_is_known()</code>.'],
        ['<code>_print()</code>', 'On structs, views and bit groups (see Printing for logs).'],
        ['protocol <code>P_p</code>', '<code>_parse()</code> → <code>_any_view</code>, <code>_decode()</code> → <code>_any_message</code>, <code>_process()</code>, <code>_encode_&lt;m&gt;()</code>, <code>_stream</code> with callbacks and the poll API, <code>_MAX_FRAME</code>.'],
      ])}
      <h3>Errors</h3>
      <p>Every fallible call returns a <code>gmsg_error</code> (<code>GMSG_OK</code> is 0) and writes its result through an out parameter.
      <code>gmsg_error_str(e)</code> gives the text.</p>
      ${errorTable()}`;
  }

  function cpp() {
    if (lang === 'c') return generatedC();
    if (lang === 'js') return generatedJs();
    if (lang === 'py') return generatedPy();
    return `
      ${table(['generated', 'what it is'], [
        ['<code>struct M</code>', 'Members per field (std::optional when conditional), <code>decode()</code>, <code>encode()</code>, scaled / conversion / computed accessors, <code>operator==</code>.'],
        ['<code>class MView</code>', '<code>from(bytes)</code> validates once; accessors read from the buffer. <code>to_struct()</code> copies out.'],
        ['<code>enum class E</code>', '<code>to_string()</code>, <code>is_known()</code>.'],
        ['<code>print_to()</code>', 'On structs, views and bit groups: drives <code>gmsg::print(msg, sink)</code> (see Printing for logs).'],
        ['<code>struct P</code>', '<code>parse()</code> → <code>AnyView</code>, <code>decode()</code> → <code>AnyMessage</code>, <code>encode(msg, out)</code>, <code>Stream&lt;&gt;</code>, <code>make_stream()</code> / <code>StreamWith</code>, <code>Callbacks</code> / <code>CallbackStream</code>, layout constants.'],
      ])}
      <h3>Errors</h3>
      <p>Every fallible call returns <code>gmsg::Result&lt;T&gt;</code> (<code>std::expected&lt;T, gmsg::Error&gt;</code>).</p>
      ${errorTable()}`;
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

  // ----------------------------------------------------------------------------------------------
  // API reference: every generated / runtime function, grouped by task. Rows are [C++, C, what it does];
  // a null signature means the language has no counterpart; the description may be [C++ text, C text].
  // Names use the guide's example: namespace demo, message Status (voltage scaled, temperature with a degF
  // conversion, samples array, ext conditional, flags bits group, label string), enum Mode, text message Gga,
  // protocol Link.
  // ----------------------------------------------------------------------------------------------
  const API = [
    { id: 'decode', title: 'Decode: bytes → struct',
      intro: 'Validate a whole message and copy every field into a struct you own. Use it when you keep or modify the values.',
      rows: [
        ['demo::Status::decode(bytes)  →  gmsg::Result<Status>', 'gmsg_error demo_status_decode(const uint8_t* data, size_t size, demo_status* out)',
          'Decode one message from the start of the buffer (trailing bytes are allowed).'],
        ['demo::Status::decode(bytes, extra)', 'gmsg_error demo_status_decode_extra(const uint8_t* data, size_t size, demo_status* out, size_t* extra)',
          'The same, also reporting how many bytes followed the message (tokens for text messages).'],
        ['gmsg::VoidResult s.read(gmsg::ByteReader& r)', 'gmsg_error demo_status_read(gmsg_reader* r, demo_status* m)',
          'Decode from a reader at its current position, e.g. a message embedded in your own format. The reader advances past it.'],
        ['demo::Link::decode(frame)  →  gmsg::Result<AnyMessage>', 'gmsg_error demo_link_decode(const uint8_t* data, size_t size, demo_link_any_message* out)',
          'Decode a whole protocol frame into whichever message it carries (see Protocols).'],
        ['v.to_struct()  →  gmsg::Result<Status>', 'gmsg_error demo_status_view_to_struct(const demo_status_view* v, demo_status* out)',
          'Copy a validated view into a struct.'],
      ] },
    { id: 'views', title: 'Views: zero-copy access',
      intro: 'A view checks the message once and then reads each field straight from the buffer when you ask for it. Nothing is copied; the buffer must outlive the view.',
      rows: [
        ['demo::StatusView::from(bytes)  →  gmsg::Result<StatusView>', 'gmsg_error demo_status_view_from(const uint8_t* data, size_t size, demo_status_view* out)',
          'Validate lengths, consts, field CRCs and strict enums, and record field offsets.'],
        ['demo::StatusView::unchecked(bytes)', 'demo_status_view demo_status_view_unchecked(const uint8_t* data, size_t size)',
          'A view without validation, for bytes you have already checked.'],
        ['v.voltage()', 'uint16_t demo_status_view_voltage(const demo_status_view* v)', 'Read one field (the raw wire value).'],
        ['v.data(), v.wire_size()', 'v.data, v.size', 'The bytes the view covers and its size on the wire.'],
        ['v.extra_bytes()', 'demo_status_view_extra_bytes(&v)', 'Bytes after the message in the data given to from() (text views: extra_tokens()).'],
        ['v.to_struct()', 'demo_status_view_to_struct(&v, &out)', 'Copy everything out into a struct.'],
      ] },
    { id: 'fields', title: 'Field access (structs and views)',
      intro: 'How each kind of field looks. Struct fields are members; view fields are accessor functions with the same names.',
      rows: [
        ['s.voltage', 's.voltage', 'Plain field: a struct member with the wire value.'],
        ['s.voltage_scaled(), s.set_voltage_scaled(12.6)', 'demo_status_voltage_scaled(&s), demo_status_set_voltage_scaled(&s, 12.6)',
          'Engineering value of a field with scale / offset; the setter rounds back to the raw value. Views have the getter.'],
        ['s.temperature_degf(), s.set_temperature_degf(77)', 'demo_status_temperature_degf(&s), demo_status_set_temperature_degf(&s, 77)',
          'One accessor pair per entry in the field\'s conversions list.'],
        ['s.rgb(), s.set_rgb(v)', 'demo_status_rgb(&s), demo_status_set_rgb(&s, v)', 'computed: values derived from other fields (getter on views too).'],
        ['s.ext (std::optional), v.ext()  →  std::optional<ExtendedView>', 's.has_ext + s.ext, demo_status_view_has_ext(&v), demo_status_view_ext(&v)',
          'A conditional (when:) field, present or absent.'],
        ['s.samples (static_vector), v.samples()  →  size(), [i], range-for', 's.samples[i] with its count field, demo_status_view_samples_count(&v), demo_status_view_samples_at(&v, i)',
          'Arrays. A length / count taken from another field fills that field automatically on encode.'],
        ['s.label (static_string), v.label()  →  std::string_view', 's.label (NUL terminated) + s.label_len, demo_status_view_label(&v)  →  gmsg_str',
          ['Strings; bytes fields give gmsg::bytes_view in views.', 'Strings; bytes fields give gmsg_bytes { data, size } in views.']],
        ['s.flags.armed', 's.flags.armed', 'Members of a bits group (see Enums and bit groups).'],
        ['v.when()  →  gmsg::Result<MyTime>', 'gmsg_error demo_status_view_when(const demo_status_view* v, my_time* out)',
          'A custom type in a view is decoded on access, so it can fail.'],
      ] },
    { id: 'encode', title: 'Encode: struct → bytes',
      intro: 'Write a struct to a buffer you provide. Lengths and counts that come from other fields are filled in for you; nothing allocates.',
      rows: [
        ['s.encode(out)  →  gmsg::Result<std::size_t>', 'gmsg_error demo_status_encode(const demo_status* m, uint8_t* out, size_t capacity, size_t* written)',
          'Encode one message; returns the bytes written (BufferFull when out is too small).'],
        ['gmsg::VoidResult s.write(gmsg::ByteWriter& w)', 'gmsg_error demo_status_write(const demo_status* m, gmsg_writer* w)',
          'Encode at a writer\'s current position (inside your own format).'],
        ['demo::Link::encode(s, out)', 'demo_link_encode_status(&s, out, capacity, &n)',
          'A complete frame: sync, header (type id and length filled in), message, checksum, escaping.'],
        ['demo::Link::encode(header, s, out)', 'demo_link_encode_status_with_header(&h, &s, out, capacity, &n)',
          'The same, with header fields you choose (sequence numbers, addresses).'],
        ['demo::Link::encode(any_message, out)', 'demo_link_encode(&any, out, capacity, &n)', 'Encode whichever message the AnyMessage holds.'],
      ] },
    { id: 'text', title: 'Text messages',
      intro: 'Text messages (NMEA-style tokens) have the same struct / view shape; these are the differences.',
      rows: [
        ['demo::Gga::decode_text(std::string_view)', null, 'Decode from text without the protocol framing.'],
        ['demo::Gga::decode(bytes), g.encode(out)', 'demo_gga_decode(data, size, &g), demo_gga_encode(&g, out, capacity, &n)', 'Decode / encode the message tokens.'],
        ['g.read_text(std::span<const std::string_view> tokens)', 'gmsg_error demo_gga_read_text(const gmsg_str* tok, size_t ntok, demo_gga* m)',
          'Fill the struct from tokens you split yourself.'],
        ['g.write_text(gmsg::text::TextWriter& w)', 'gmsg_error demo_gga_write_text(const demo_gga* m, gmsg_text_writer* w)', 'Append the tokens to a text writer.'],
        ['demo::GgaView::from(std::string_view or bytes)', 'demo_gga_view_from(data, size, &v)', 'Split into tokens once; fields are parsed when read.'],
        ['v.latitude()  →  gmsg::Result<std::optional<double>>', 'gmsg_error demo_gga_view_latitude(const demo_gga_view* v, double* out), demo_gga_view_has_latitude(&v)',
          ['Parse one token on demand; optional tokens give an empty optional.', 'Parse one token on demand; an empty optional token gives GMSG_ERR_MISSING_FIELD (check _has_ first).']],
        ['g.latitude (std::optional<double>)', 'g.has_latitude + g.latitude', 'Optional tokens in the struct.'],
        [null, 'demo_gga_view_token(&v, i), demo_gga_view_present_tokens(&v)', 'The raw token text and the number of tokens present.'],
        ['Gga::token_count, Gga::separator', null, 'Tokens per message and the separator character.'],
      ] },
    { id: 'enums', title: 'Enums and bit groups',
      intro: 'Enums keep every raw value (undeclared ones too); bits groups pack several fields into bytes.',
      rows: [
        ['enum class demo::Mode : std::uint8_t { idle, run, fault }', 'typedef uint8_t demo_mode;  DEMO_MODE_IDLE, DEMO_MODE_RUN, ...', 'The enum type and its values.'],
        ['to_string(Mode)  →  std::string_view', 'const char* demo_mode_to_string(demo_mode v)', ['The value\'s name; empty when undeclared.', 'The value\'s name; NULL when undeclared.']],
        ['is_known(Mode)', 'bool demo_mode_is_known(demo_mode v)', 'True for a declared value.'],
        ['Status_flags::unpack(p), f.pack(p)', 'demo_status_flags_unpack(&f, p), demo_status_flags_pack(&f, p)', 'Read / write the group from / to its bytes.'],
        ['Status_flags::check(p)  →  gmsg::VoidResult', 'gmsg_error demo_status_flags_check(const uint8_t* p)', 'Validate const bits and strict enums in the group.'],
      ] },
    { id: 'protocols', title: 'Protocols: one frame at a time',
      intro: 'For datagrams, packets from a queue, or any buffer that holds exactly one frame. Framing, checksum and escaping are handled; the message is chosen by its type id.',
      rows: [
        ['demo::Link::parse(frame)  →  gmsg::Result<AnyView>', 'gmsg_error demo_link_parse(const uint8_t* data, size_t size, demo_link_any_view* out)',
          ['A view of whichever message the frame carries: AnyView is a std::variant of the message views.', 'A view of whichever message the frame carries: out->kind (DEMO_LINK_STATUS, ...) and out->as.status, ...']],
        ['demo::Link::decode(frame)  →  gmsg::Result<AnyMessage>', 'demo_link_decode(data, size, &msg)', 'The same, decoded into a struct (a variant / tagged union).'],
        ['demo::Link::process(frame, handler)  →  gmsg::Error', 'gmsg_error demo_link_process(const uint8_t* data, size_t size, const demo_link_callbacks* cb)',
          ['Call the handler overload for the message (with the header view when it takes one).', 'Call the callback slot for the message, then on_frame.']],
        ['demo::Link::open(frame)  →  gmsg::Result<Frame>, select_message(f), visit_frame(f, fn)', null,
          'The steps of parse separately: check and unescape the frame, then pick the message.'],
        ['demo::Link::scan(bytes)  →  gmsg::FrameScan', null, 'Find where the next frame starts and ends (what the stream parser uses).'],
        ['Link::Header, Link::HeaderView, Link::AnyView, Link::AnyMessage', 'demo_link_header, demo_link_header_view, demo_link_any_view, demo_link_any_message, demo_link_kind',
          'The protocol\'s types.'],
      ] },
    { id: 'stream', title: 'Streaming: push bytes, get callbacks',
      intro: 'For serial ports and sockets: bytes arrive in chunks of any size; the parser finds frames, resyncs after garbage and delivers each message.',
      rows: [
        ['demo::Link::Stream<> parser;', 'demo_link_stream s;  uint8_t buf[DEMO_LINK_MAX_FRAME];  demo_link_stream_init(&s, buf, sizeof buf)',
          ['A parser with its own buffer of max_frame bytes (pass a capacity to change it).', 'A parser using your buffer (at least DEMO_LINK_MAX_FRAME bytes).']],
        ['parser.feed(chunk, handler)  →  frames delivered', 'size_t demo_link_stream_feed(demo_link_stream* s, const uint8_t* data, size_t n)',
          ['Push the bytes and call the handler for every complete frame.', 'Push the bytes and call s.cb for every complete frame; returns frames delivered.']],
        ['parser.push(chunk), parser.next(handler)', 'demo_link_stream_push(&s, data, n), demo_link_stream_next(&s)', 'Push without processing, then deliver one frame (or one error) at a time.'],
        ['demo::Link::make_stream(handler), feed(chunk)', null, 'A parser that stores its handler at compile time, so the byte receiver only calls feed(chunk).'],
        ['demo::Link::CallbackStream<> link;  link.on<&App::on_status>(app)', 's.cb.ctx = &app;  s.cb.on_status = on_status;',
          ['Runtime listeners, one per message; the listener is picked from its parameter type.', 'Callback slots, one per message, with one context pointer; NULL slots are skipped.']],
        ['link.off<StatusView>(), link.has<StatusView>(), link.clear()', null, 'Remove, test and clear listeners.'],
        ['on<&App::on_frame>, on<&App::on_drop>', 's.cb.on_frame, s.cb.on_error', 'Every valid frame (after its message callback), and dropped bytes / rejected frames with the reason.'],
        ['parser.stats()', 'const gmsg_stream_stats* demo_link_stream_stats(const demo_link_stream* s)',
          'Counters: frames, frame_errors, bytes_skipped, overflows, resyncs, flushes, frames_with_extra, overruns.'],
        ['parser.resync(handler)', 'bool demo_link_stream_resync(demo_link_stream* s)',
          'Give up on an incomplete frame at the head (e.g. a corrupted length): reported as Truncated, then the next scan resynchronises.'],
        ['parser.flush(handler)', 'bool demo_link_stream_flush(demo_link_stream* s)', 'Drop everything buffered (reported as Truncated).'],
        ['parser.set_idle_timeout(t), parser.tick(now, handler)', 'demo_link_stream_set_idle_timeout(&s, t), demo_link_stream_tick(&s, now)',
          'Resync automatically once the link has been idle for t (any time unit; the parser has no clock).'],
        ['parser.last_error(), parser.buffered(), parser.reset()', 'demo_link_stream_last_error(&s), demo_link_stream_buffered(&s), demo_link_stream_reset(&s)',
          'Reason of the last drop, bytes waiting, and start over (clears stats).'],
        ['gmsg::ByteRing<256> rx;  rx.push(byte)', 'gmsg_ring rx;  gmsg_ring_init(&rx, buf, 256);  gmsg_ring_push(&rx, b)',
          'A lock-free single-producer / single-consumer byte ring: the interrupt pushes, the main loop parses.'],
        ['parser.feed(rx), parser.feed(rx, max_bytes), parser.push(rx)', 'demo_link_stream_feed_ring(&s, &rx, SIZE_MAX), demo_link_stream_push_ring(&s, &rx, max)',
          'Drain the ring: deliver frames (feed), or move bytes only for the pull API (push). Input the full ring dropped is reported as Overrun.'],
        ['rx.dropped(), rx.high_water()', 'gmsg_ring_dropped(&rx), gmsg_ring_high_water(&rx)',
          'Bytes dropped because the ring was full, and the highest fill level (size the ring from it).'],
      ] },
    { id: 'poll', title: 'Pull API: ask for the next message',
      intro: 'Push bytes, then poll in your main loop or while waiting for a reply. A polled view stays valid until the parser is used again.',
      rows: [
        ['parser.poll()  →  Polled<AnyView>', 'bool demo_link_stream_poll(demo_link_stream* s, demo_link_any_view* out)', 'The next message, or nothing when more input is needed.'],
        ['parser.poll_frame()  →  { header, message, bytes, extra }', 'bool demo_link_stream_poll_frame(demo_link_stream* s, demo_link_received* out)',
          'The next frame with its header view, raw bytes and the data after its message (extra).'],
        ['parser.poll_message()  →  std::optional<AnyMessage>', 'bool demo_link_stream_poll_message(demo_link_stream* s, demo_link_any_message* out)', 'The next message as a decoded copy.'],
        ['parser.poll_event()  →  AnyView or gmsg::FrameError', 'gmsg_poll demo_link_stream_poll_event(demo_link_stream* s, demo_link_event* out)',
          ['Messages and errors in stream order.', 'Messages and errors in stream order: GMSG_POLL_MESSAGE, GMSG_POLL_ERROR or GMSG_POLL_NEED_MORE.']],
        ['link.poll_for<StatusView>()', 'bool demo_link_stream_poll_for(demo_link_stream* s, demo_link_kind kind, demo_link_any_view* out)',
          'Wait for one message type; other messages go to the listeners meanwhile.'],
        [null, 'uint32_t demo_link_stream_generation(const demo_link_stream* s)', 'Changes whenever polled views expire, so you can check one is still current.'],
      ] },
    { id: 'print', title: 'Printing',
      intro: 'Human-readable text for logs and consoles; the same output in both languages and in the Studio decode tester.',
      rows: [
        ['gmsg::print(v, sink, opts), gmsg::println(v, sink, opts)  →  gmsg::Result<std::size_t>', 'demo_status_print(&s, &p), demo_status_view_print(&v, &p), demo_status_flags_print(&f, &p)',
          'Print a struct, view or bits group.'],
        ['sinks: FILE*, std::ostream, char[N], static_string, std::string, callback', 'gmsg_printer_init_file(&p, f, opts), gmsg_printer_init_buffer(&p, buf, cap, opts), gmsg_printer_init(&p, write_fn, ctx, opts)',
          'Where the text goes.'],
        [null, 'gmsg_error gmsg_printer_finish(gmsg_printer* p, bool newline, size_t* written)', 'End printing; GMSG_ERR_BUFFER_FULL when the text was cut.'],
        ['gmsg::print(Link::parse(frame).value(), sink)', 'demo_link_any_view_print(&any, &p), demo_link_any_message_print(&msg, &p)', 'Print whichever message a frame holds.'],
        ['gmsg::PrintOptions{ .style, .indent, .max_items, .max_bytes, .precision }', 'gmsg_print_options o = gmsg_print_defaults();', 'Multi or single line, indent, how much of arrays and bytes, significant digits.'],
        ['m.print_to(gmsg::Printer& p)', null, 'What gmsg::print calls; use it to print inside your own printer.'],
        ['void gmsg_print(gmsg::Printer&, const MyTime&)', 'c: { print: my_time_print }  →  void my_time_print(gmsg_printer* p, const my_time* v)', 'How a custom type prints (otherwise <?>).'],
      ] },
    { id: 'sizes', title: 'Sizes and constants',
      intro: 'Compile-time sizes for buffers and checks.',
      rows: [
        ['Status::min_wire_size, Status::max_wire_size', 'DEMO_STATUS_MIN_WIRE_SIZE, DEMO_STATUS_MAX_WIRE_SIZE', 'Smallest and largest encoding of the message.'],
        ['Status::wire_size', 'DEMO_STATUS_WIRE_SIZE', 'Only for fixed-size messages.'],
        ['Status::magic_value', 'DEMO_STATUS_MAGIC_VALUE', 'The value of a const field (magic).'],
        ['Link::max_frame', 'DEMO_LINK_MAX_FRAME', 'Largest frame on the wire: size of stream and encode buffers.'],
        ['Link::prefix_size, header_size, trailer_size, suffix_size, prefix', null, 'Frame layout: sync / start bytes, header, checksum, end delimiter.'],
      ] },
    { id: 'errors', title: 'Errors',
      intro: 'Nothing throws and nothing allocates; every fallible call reports an error code (table in the next section).',
      rows: [
        ['gmsg::Result<T>  (std::expected<T, gmsg::Error>)', 'gmsg_error return value + out parameter (GMSG_OK is 0)', 'How results are returned.'],
        ['gmsg::to_string(gmsg::Error)', 'const char* gmsg_error_str(gmsg_error e)', 'Short description, e.g. "bad checksum".'],
        ['gmsg::FrameError{ error, data }', 'on_error(ctx, gmsg_error e, const uint8_t* data, size_t size)', 'A dropped frame or skipped bytes, with the reason.'],
      ] },
    { id: 'checksums', title: 'Checksums (runtime)',
      intro: 'The algorithms protocols use are available directly, e.g. to check a frame by hand or build a reply.',
      rows: [
        ['gmsg::crc32::compute(bytes)', 'uint32_t gmsg_crc32_compute(const uint8_t* data, size_t n)', 'One call over a whole buffer (every preset: crc8 … crc64_ecma, xor8, sum8, lrc8, sum16, fletcher16, fletcher8_ubx).'],
        ['init(), update(state, bytes), finish(state)', 'gmsg_crc32_init(), gmsg_crc32_update(state, data, n), gmsg_crc32_finish(state)', 'Incremental: feed the data in pieces.'],
        ['gmsg::Crc<T, width, poly, init, refin, refout, xorout>', 'gmsg_crc_params p = { width, refin, refout, poly, init, xorout };  gmsg_crc_compute(&p, data, n)', 'Any other CRC from its parameters.'],
      ] },
  ];


  // JavaScript signatures of the API rows above, keyed by the row's C++ signature (or its C signature when the row has
  // no C++ one): [signature or null (no counterpart), optional JavaScript description].
  const API_JS = {
    'demo::Status::decode(bytes)  →  gmsg::Result<Status>': ['demo.Status.decode(bytes)  →  Status', 'Decode one message from the start of the buffer (trailing bytes are allowed). Throws gmsg.GmpError.'],
    'demo::Status::decode(bytes, extra)': ['demo.Status.decodeWithExtra(bytes)  →  { message, extra }'],
    'v.extra_bytes()': ['v.extraBytes, v.extraTokens'],
    'gmsg::VoidResult s.read(gmsg::ByteReader& r)': ['demo.Status.read(reader)  →  Status', 'Decode from a gmsg.Reader at its current position, e.g. a message embedded in your own format. The reader advances past it.'],
    'demo::Link::decode(frame)  →  gmsg::Result<AnyMessage>': ['demo.Link.decode(frame)  →  Status | …'],
    'v.to_struct()  →  gmsg::Result<Status>': ['v.toStruct()  →  Status'],
    'demo::StatusView::from(bytes)  →  gmsg::Result<StatusView>': ['demo.StatusView.from(bytes)  →  StatusView'],
    'demo::StatusView::unchecked(bytes)': ['demo.StatusView.unchecked(bytes)'],
    'v.voltage()': ['v.voltage', 'Read one field (the raw wire value); fields are getters.'],
    'v.data(), v.wire_size()': ['v.data, v.wireSize', 'The bytes the view covers (a Uint8Array over the input) and its size on the wire.'],
    'v.to_struct()': ['v.toStruct()'],
    's.voltage': ['s.voltage', 'Plain field: a property with the wire value (Number; BigInt for 64-bit types).'],
    's.voltage_scaled(), s.set_voltage_scaled(12.6)': ['s.voltage_scaled, s.voltage_scaled = 12.6', 'Engineering value of a field with scale / offset; assigning rounds back to the raw value. Views have the getter.'],
    's.temperature_degf(), s.set_temperature_degf(77)': ['s.temperature_degf, s.temperature_degf = 77', "One getter / setter per entry in the field's conversions list."],
    's.rgb(), s.set_rgb(v)': ['s.rgb, s.rgb = v', 'computed: values derived from other fields (a getter on views too).'],
    's.ext (std::optional), v.ext()  →  std::optional<ExtendedView>': ['s.ext (undefined when absent), v.ext  →  ExtendedView | undefined'],
    's.samples (static_vector), v.samples()  →  size(), [i], range-for': ['s.samples (Array), v.samples  →  Array', 'Arrays. A length / count taken from another field is filled in on encode; more than max elements is OutOfRange.'],
    's.label (static_string), v.label()  →  std::string_view': ['s.label (string), v.label  →  string', 'Strings hold one character per byte (latin1); bytes fields are Uint8Array.'],
    's.flags.armed': ['s.flags.armed'],
    'v.when()  →  gmsg::Result<MyTime>': ['v.when', 'A custom type in a view is decoded on access, so reading it can throw.'],
    's.encode(out)  →  gmsg::Result<std::size_t>': ['s.encode()  →  Uint8Array', 'Encode one message to a new Uint8Array. Values that do not fit their type, or containers past max, throw OutOfRange.'],
    'gmsg::VoidResult s.write(gmsg::ByteWriter& w)': ['demo.Status.write(writer, s)', 'Encode with a gmsg.Writer at its current position (inside your own format). Plain objects with the same fields work too.'],
    'demo::Link::encode(s, out)': ['demo.Link.encode(s)  →  Uint8Array'],
    'demo::Link::encode(header, s, out)': ['demo.Link.encode(s, { seq: 7 })', 'The same, with header fields you choose (sequence numbers, addresses).'],
    'demo::Link::encode(any_message, out)': [null],
    'demo::Gga::decode_text(std::string_view)': ['demo.Gga.decodeText(string)'],
    'demo::Gga::decode(bytes), g.encode(out)': ['demo.Gga.decode(bytes), g.encode()  →  Uint8Array, g.encodeText()  →  string'],
    'g.read_text(std::span<const std::string_view> tokens)': ['demo.Gga.readText(tokens)  →  Gga', 'Build the message from tokens (strings) you split yourself.'],
    'g.write_text(gmsg::text::TextWriter& w)': ['demo.Gga.writeText(textWriter, g)', 'Append the tokens to a gmsg.text.TextWriter.'],
    'demo::GgaView::from(std::string_view or bytes)': ['demo.GgaView.from(string or bytes)'],
    'v.latitude()  →  gmsg::Result<std::optional<double>>': ['v.latitude  →  number | undefined', 'Parse one token on access (throws GmpError when it does not parse); an empty optional token gives undefined.'],
    'g.latitude (std::optional<double>)': ['g.latitude (undefined when empty)'],
    'demo_gga_view_token(&v, i), demo_gga_view_present_tokens(&v)': ['v.token(i), v.presentTokens'],
    'Gga::token_count, Gga::separator': ['demo.Gga.tokenCount, demo.Gga.separator'],
    'enum class demo::Mode : std::uint8_t { idle, run, fault }': ['demo.Mode  →  { idle: 0, run: 1, fault: 2 } (frozen)'],
    'to_string(Mode)  →  std::string_view': ['demo.Mode.nameOf(v)  →  string'],
    'is_known(Mode)': ['demo.Mode.isKnown(v)'],
    'Status_flags::unpack(p), f.pack(p)': ['demo.Status_flags.unpack(bytes, at), demo.Status_flags.pack(f, bytes, at)'],
    'Status_flags::check(p)  →  gmsg::VoidResult': ['demo.Status_flags.check(bytes, at)', 'Validate const bits and strict enums in the group (throws GmpError).'],
    'demo::Link::parse(frame)  →  gmsg::Result<AnyView>': ['demo.Link.parse(frame)  →  StatusView | …', 'A view of whichever message the frame carries (check it with instanceof).'],
    'demo::Link::decode(frame)  →  gmsg::Result<AnyMessage>_': [null],
    'demo::Link::process(frame, handler)  →  gmsg::Error': ['demo.Link.process(frame)  →  { header, message }', "The frame's header view and message view (what the stream parser calls)."],
    'demo::Link::open(frame)  →  gmsg::Result<Frame>, select_message(f), visit_frame(f, fn)': ['demo.Link.open(frame)  →  { header, body }, demo.Link.select(f)'],
    'demo::Link::scan(bytes)  →  gmsg::FrameScan': ['demo.Link.scan(bytes)  →  { kind, size, consume, consumeOnError, error }'],
    'Link::Header, Link::HeaderView, Link::AnyView, Link::AnyMessage': ['demo.Link.Header, demo.Link.HeaderView, demo.Link.views, demo.Link.messages'],
    'demo::Link::Stream<> parser;': ['const parser = demo.Link.stream()', 'A gmsg.StreamParser with a buffer of maxFrame bytes (pass { capacity } to change it).'],
    'parser.feed(chunk, handler)  →  frames delivered': ['parser.feed(chunk)  →  frames delivered', 'Push the bytes and call the listeners for every complete frame.'],
    'parser.push(chunk), parser.next(handler)': ['parser.push(chunk), parser.next()'],
    'demo::Link::make_stream(handler), feed(chunk)': [null],
    'demo::Link::CallbackStream<> link;  link.on<&App::on_status>(app)': ["parser.on('Status', (view, header) => …)  →  off()", 'Listeners per message name (or class); the header view is the second argument. on() returns a function that removes the listener.'],
    'link.off<StatusView>(), link.has<StatusView>(), link.clear()': [null],
    'on<&App::on_frame>, on<&App::on_drop>': ["parser.on('frame', fn), parser.on('error', (e) => …)", 'Every valid frame (after its message listeners), and dropped bytes / rejected frames as a GmpError with .bytes and .offset.'],
    'parser.stats()': ['parser.stats', 'Counters: frames, frameErrors, bytesSkipped, overflows, resyncs, flushes, framesWithExtra, overruns.'],
    'gmsg::ByteRing<256> rx;  rx.push(byte)': ['const rx = new gmsg.ByteRing(256, { shared: true });  rx.push(b), rx.pushBytes(bytes)', 'A single-producer / single-consumer byte ring; with shared: true a Worker can fill it (gmsg.ByteRing.attach(rx.buffer)).'],
    'parser.feed(rx), parser.feed(rx, max_bytes), parser.push(rx)': ['parser.feed(rx[, max]), parser.push(rx[, max]), parser.messages(rx), parser.events(rx)'],
    'rx.dropped(), rx.high_water()': ['rx.dropped, rx.highWater'],
    'parser.resync(handler)': ['parser.resync()', 'Give up on an incomplete frame at the head: reported to the error listeners as Truncated.'],
    'parser.flush(handler)': ['parser.flush()'],
    'parser.set_idle_timeout(t), parser.tick(now, handler)': ['parser.setIdleTimeout(t), parser.tick(Date.now())'],
    'parser.last_error(), parser.buffered(), parser.reset()': ['parser.lastError, parser.buffered, parser.position, parser.reset()', 'Reason of the last drop, bytes waiting, stream offset, and start over (clears stats).'],
    'parser.poll()  →  Polled<AnyView>': ['parser.poll()  →  view | null'],
    'parser.poll_frame()  →  { header, message, bytes, extra }': ['parser.pollFrame()  →  { header, message, bytes, offset, extra } | null'],
    'parser.poll_message()  →  std::optional<AnyMessage>': ['parser.pollMessage()  →  Status | … | null'],
    'parser.poll_event()  →  AnyView or gmsg::FrameError': ['parser.pollEvent()  →  { message, header, bytes, offset } | { error, offset } | null'],
    'link.poll_for<StatusView>()': ["parser.pollFor('Status')  →  StatusView | null"],
    'uint32_t demo_link_stream_generation(const demo_link_stream* s)': [null],
    'gmsg::print(v, sink, opts), gmsg::println(v, sink, opts)  →  gmsg::Result<std::size_t>': ['gmsg.print(v, opts)  →  string, v.toString(opts)'],
    'sinks: FILE*, std::ostream, char[N], static_string, std::string, callback': [null],
    'gmsg_error gmsg_printer_finish(gmsg_printer* p, bool newline, size_t* written)': [null],
    'gmsg::print(Link::parse(frame).value(), sink)': ['gmsg.print(demo.Link.parse(frame))'],
    'gmsg::PrintOptions{ .style, .indent, .max_items, .max_bytes, .precision }': ["{ style: 'single_line', indent, maxItems, maxBytes, precision }"],
    'm.print_to(gmsg::Printer& p)': ['m.printTo(printer)', 'What gmsg.print calls; use it to print inside your own gmsg.Printer.'],
    'void gmsg_print(gmsg::Printer&, const MyTime&)': ['js: { print: printTime }  →  printTime(printer, value)', 'How a custom type prints (otherwise numbers and strings as such, else <?>).'],
    'Status::min_wire_size, Status::max_wire_size': ['demo.Status.minWireSize, demo.Status.maxWireSize'],
    'Status::wire_size': ['demo.Status.wireSize'],
    'Status::magic_value': ['demo.Status.magic_value'],
    'Link::max_frame': ['demo.Link.maxFrame'],
    'Link::prefix_size, header_size, trailer_size, suffix_size, prefix': ['demo.Link.prefixSize, headerSize, trailerSize, suffixSize, prefix, suffix'],
    'gmsg::Result<T>  (std::expected<T, gmsg::Error>)': ['throw new gmsg.GmpError(code)', 'Failures throw a gmsg.GmpError; e.code is a gmsg.Err value.'],
    'gmsg::to_string(gmsg::Error)': ['gmsg.errorText(code), e.message'],
    'gmsg::FrameError{ error, data }': ["parser.on('error', (e) => …): e.code, e.bytes, e.offset"],
    'gmsg::crc32::compute(bytes)': ['gmsg.checksum.crc32.compute(bytes)'],
    'init(), update(state, bytes), finish(state)': ['init(), update(state, bytes), finish(state)'],
    'gmsg::Crc<T, width, poly, init, refin, refout, xorout>': ['gmsg.checksum.crc({ width, poly, init, refin, refout, xorout })'],
  };
  // rows only JavaScript has: [null, null, description, signature]
  const API_JS_EXTRA = {
    poll: [[null, null, 'Iterate over the messages (or events) of a chunk. Polled views own their bytes, so they never expire.', 'for (const v of parser.messages(chunk)) …, parser.events(chunk)']],
  };

  // Python names of the API rows, keyed like API_JS: [signature, description (optional)]
  const API_PY = {
    'demo::Status::decode(bytes)  →  gmsg::Result<Status>': ['demo.Status.decode(data)  →  Status', 'Decode one message from the start of the buffer (trailing bytes are allowed). Raises gmsg.GmpError.'],
    'demo::Status::decode(bytes, extra)': ['demo.Status.decode_with_extra(data)  →  (Status, extra)'],
    'v.extra_bytes()': ['v.extra_bytes, v.extra_tokens'],
    'gmsg::VoidResult s.read(gmsg::ByteReader& r)': ['demo.Status.read(reader)  →  Status', 'Decode from a gmsg.Reader at its current position, e.g. a message embedded in your own format. The reader advances past it.'],
    'demo::Link::decode(frame)  →  gmsg::Result<AnyMessage>': ['demo.Link.decode(frame)  →  Status | …'],
    'v.to_struct()  →  gmsg::Result<Status>': ['v.to_struct()  →  Status'],
    'demo::StatusView::from(bytes)  →  gmsg::Result<StatusView>': ['demo.StatusView.from_bytes(data)  →  StatusView'],
    'demo::StatusView::unchecked(bytes)': ['demo.StatusView.unchecked(data)'],
    'v.voltage()': ['v.voltage', 'Read one field (the raw wire value); fields are properties.'],
    'v.data(), v.wire_size()': ['v.data, v.wire_size', 'The bytes the view covers (a memoryview over the input) and its size on the wire.'],
    'v.to_struct()': ['v.to_struct()'],
    's.voltage': ['s.voltage', 'Plain field: a dataclass attribute with the wire value (int, float, bool or str).'],
    's.voltage_scaled(), s.set_voltage_scaled(12.6)': ['s.voltage_scaled, s.voltage_scaled = 12.6', 'Engineering value of a field with scale / offset; assigning rounds back to the raw value. Views have the property too.'],
    's.temperature_degf(), s.set_temperature_degf(77)': ['s.temperature_degf, s.temperature_degf = 77', "One property per entry in the field's conversions list."],
    's.rgb(), s.set_rgb(v)': ['s.rgb, s.rgb = v', 'computed: values derived from other fields (a property on views too).'],
    's.ext (std::optional), v.ext()  →  std::optional<ExtendedView>': ['s.ext (None when absent), v.ext  →  ExtendedView | None'],
    's.samples (static_vector), v.samples()  →  size(), [i], range-for': ['s.samples (list), v.samples  →  list', 'Arrays. A length / count taken from another field is filled in on encode; more than max elements is OUT_OF_RANGE.'],
    's.label (static_string), v.label()  →  std::string_view': ['s.label (str), v.label  →  str', 'Strings hold one character per byte (latin-1); bytes fields are bytes (a memoryview in views).'],
    's.flags.armed': ['s.flags.armed'],
    'v.when()  →  gmsg::Result<MyTime>': ['v.when', 'A custom type in a view is decoded on access, so reading it can raise.'],
    's.encode(out)  →  gmsg::Result<std::size_t>': ['s.encode()  →  bytes', 'Encode one message. Values that do not fit their type, or containers past max, raise OUT_OF_RANGE.'],
    'gmsg::VoidResult s.write(gmsg::ByteWriter& w)': ['demo.Status.write(writer, s)', 'Encode with a gmsg.Writer at its current position (inside your own format).'],
    'demo::Link::encode(s, out)': ['demo.Link.encode(s)  →  bytes'],
    'demo::Link::encode(header, s, out)': ["demo.Link.encode(s, {'seq': 7})", 'The same, with header fields you choose (a dict or a Header instance).'],
    'demo::Link::encode(any_message, out)': [null],
    'demo::Gga::decode_text(std::string_view)': ['demo.Gga.decode_text(s)'],
    'demo::Gga::decode(bytes), g.encode(out)': ['demo.Gga.decode(data), g.encode()  →  bytes, g.encode_text()  →  str'],
    'g.read_text(std::span<const std::string_view> tokens)': ['demo.Gga.read_text(tokens)  →  Gga', 'Build the message from tokens (strings) you split yourself.'],
    'g.write_text(gmsg::text::TextWriter& w)': ['demo.Gga.write_text(text_writer, g)', 'Append the tokens to a gmsg.text.TextWriter.'],
    'demo::GgaView::from(std::string_view or bytes)': ['demo.GgaView.from_text(str or bytes)'],
    'v.latitude()  →  gmsg::Result<std::optional<double>>': ['v.latitude  →  float | None', 'Parse one token on access (raises GmpError when it does not parse); an empty optional token gives None.'],
    'g.latitude (std::optional<double>)': ['g.latitude (None when empty)'],
    'demo_gga_view_token(&v, i), demo_gga_view_present_tokens(&v)': ['v.token(i), v.present_tokens'],
    'Gga::token_count, Gga::separator': ['demo.Gga.token_count, demo.Gga.separator'],
    'enum class demo::Mode : std::uint8_t { idle, run, fault }': ['class demo.Mode(gmsg.GmpEnum): idle = 0, run = 1, fault = 2'],
    'to_string(Mode)  →  std::string_view': ['demo.Mode.name_of(v)  →  str'],
    'is_known(Mode)': ['demo.Mode.is_known(v), demo.Mode.of(v)'],
    'Status_flags::unpack(p), f.pack(p)': ['demo.Status_flags.unpack(data, at), demo.Status_flags.pack(f, buf, at)'],
    'Status_flags::check(p)  →  gmsg::VoidResult': ['demo.Status_flags.check(data, at)', 'Validate const bits and strict enums in the group (raises GmpError).'],
    'demo::Link::parse(frame)  →  gmsg::Result<AnyView>': ['demo.Link.parse(frame)  →  StatusView | …', 'A view of whichever message the frame carries (check it with isinstance).'],
    'demo::Link::decode(frame)  →  gmsg::Result<AnyMessage>_': [null],
    'demo::Link::process(frame, handler)  →  gmsg::Error': ['demo.Link.process(frame)  →  gmsg.Received(message, header)', "The frame's message view and header view (what the stream parser calls)."],
    'demo::Link::open(frame)  →  gmsg::Result<Frame>, select_message(f), visit_frame(f, fn)': ['demo.Link.open(frame)  →  gmsg.Opened(body, header), demo.Link.select(f)'],
    'demo::Link::scan(bytes)  →  gmsg::FrameScan': ['demo.Link.scan(buf)  →  gmsg.FrameScan(kind, size, consume, consume_on_error, error)'],
    'Link::Header, Link::HeaderView, Link::AnyView, Link::AnyMessage': ['demo.Link.Header, demo.Link.HeaderView, demo.Link.views, demo.Link.messages'],
    'demo::Link::Stream<> parser;': ['parser = demo.Link.stream()', 'A gmsg.StreamParser with a buffer of max_frame bytes (pass capacity= to change it).'],
    'parser.feed(chunk, handler)  →  frames delivered': ['parser.feed(chunk)  →  frames delivered', 'Push the bytes and call the listeners for every complete frame.'],
    'parser.push(chunk), parser.next(handler)': ['parser.push(chunk), parser.next()'],
    'demo::Link::make_stream(handler), feed(chunk)': [null],
    'demo::Link::CallbackStream<> link;  link.on<&App::on_status>(app)': ['parser.on(demo.Status, lambda view, header: …)  →  off()', 'Listeners per message class (or name); the header view is the second argument. on() returns a function that removes the listener.'],
    'link.off<StatusView>(), link.has<StatusView>(), link.clear()': [null],
    'on<&App::on_frame>, on<&App::on_drop>': ["parser.on('frame', fn), parser.on('error', lambda e: …)", 'Every valid frame (after its message listeners), and dropped bytes / rejected frames as a GmpError with .data and .offset.'],
    'parser.stats()': ['parser.stats', 'Counters: frames, frame_errors, bytes_skipped, overflows, resyncs, flushes, frames_with_extra, overruns.'],
    'gmsg::ByteRing<256> rx;  rx.push(byte)': ['rx = gmsg.ByteRing(256);  rx.push(b), rx.push_bytes(data)', 'A single-producer / single-consumer byte ring: a reader thread fills it, the parsing thread drains it.'],
    'parser.feed(rx), parser.feed(rx, max_bytes), parser.push(rx)': ['parser.feed(rx, max_bytes=None), parser.push(rx), parser.messages(rx), parser.events(rx)'],
    'rx.dropped(), rx.high_water()': ['rx.dropped, rx.high_water'],
    'parser.resync(handler)': ['parser.resync()', 'Give up on an incomplete frame at the head: reported to the error listeners as TRUNCATED.'],
    'parser.flush(handler)': ['parser.flush()'],
    'parser.set_idle_timeout(t), parser.tick(now, handler)': ['parser.set_idle_timeout(t), parser.tick(time.monotonic())'],
    'parser.last_error(), parser.buffered(), parser.reset()': ['parser.last_error, parser.buffered, parser.position, parser.reset()', 'Reason of the last drop, bytes waiting, stream offset, and start over (clears stats).'],
    'parser.poll()  →  Polled<AnyView>': ['parser.poll()  →  view | None'],
    'parser.poll_frame()  →  { header, message, bytes, extra }': ['parser.poll_frame()  →  gmsg.Received(message, header, bytes, offset, extra) | None'],
    'parser.poll_message()  →  std::optional<AnyMessage>': ['parser.poll_message()  →  Status | … | None'],
    'parser.poll_event()  →  AnyView or gmsg::FrameError': ['parser.poll_event()  →  gmsg.Received | gmsg.GmpError | None'],
    'link.poll_for<StatusView>()': ['parser.poll_for(demo.Status)  →  StatusView | None'],
    'uint32_t demo_link_stream_generation(const demo_link_stream* s)': [null],
    'gmsg::print(v, sink, opts), gmsg::println(v, sink, opts)  →  gmsg::Result<std::size_t>': ['str(v), v.to_text(**opts), gmsg.to_text(v, **opts)  →  str'],
    'sinks: FILE*, std::ostream, char[N], static_string, std::string, callback': [null],
    'gmsg_error gmsg_printer_finish(gmsg_printer* p, bool newline, size_t* written)': [null],
    'gmsg::print(Link::parse(frame).value(), sink)': ['print(demo.Link.parse(frame))'],
    'gmsg::PrintOptions{ .style, .indent, .max_items, .max_bytes, .precision }': ["to_text(style='single_line', indent=2, max_items=64, max_bytes=64, precision=9)"],
    'm.print_to(gmsg::Printer& p)': ['m.print_to(printer)', 'What gmsg.to_text calls; use it to print inside your own gmsg.Printer.'],
    'void gmsg_print(gmsg::Printer&, const MyTime&)': ['python: { print: print_time }  →  print_time(printer, value)', 'How a custom type prints (otherwise numbers and strings as such, else <?>).'],
    'Status::min_wire_size, Status::max_wire_size': ['demo.Status.min_wire_size, demo.Status.max_wire_size'],
    'Status::wire_size': ['demo.Status.wire_size'],
    'Status::magic_value': ['demo.Status.magic_value'],
    'Link::max_frame': ['demo.Link.max_frame'],
    'Link::prefix_size, header_size, trailer_size, suffix_size, prefix': ['demo.Link.prefix_size, header_size, trailer_size, suffix_size, prefix, suffix'],
    'gmsg::Result<T>  (std::expected<T, gmsg::Error>)': ['raise gmsg.GmpError(code)', 'Failures raise a gmsg.GmpError; e.code is a gmsg.Err (IntEnum).'],
    'gmsg::to_string(gmsg::Error)': ['gmsg.error_text(code), str(e)'],
    'gmsg::FrameError{ error, data }': ["parser.on('error', lambda e: …): e.code, e.data, e.offset"],
    'gmsg::crc32::compute(bytes)': ['gmsg.checksum.crc32.compute(data)'],
    'init(), update(state, bytes), finish(state)': ['init(), update(state, data), finish(state)'],
    'gmsg::Crc<T, width, poly, init, refin, refout, xorout>': ['gmsg.checksum.crc(width, poly, init, refin, refout, xorout)'],
  };
  // rows only Python has: [null, null, description, signature]
  const API_PY_EXTRA = {
    poll: [[null, null, 'Iterate over the messages (or events) of a chunk. Views from the parser own their bytes, so they never expire.', 'for v in parser.messages(chunk): …, parser.events(chunk)']],
    checksums: [[null, null, 'Whether the C accelerator (gmsg._native) is in use, and switching it off (or GMSG_PURE=1). Results are the same either way.', 'gmsg.NATIVE, gmsg.use_native(False)']],
  };
  const API_MAP = { js: API_JS, py: API_PY };
  const API_EXTRA = { js: API_JS_EXTRA, py: API_PY_EXTRA };

  function apiRef() {
    const langName = LANGS[lang];
    const groups = API.map((g) => {
      const map = API_MAP[lang];
      const all = map ? [...g.rows, ...(API_EXTRA[lang][g.id] || [])] : g.rows;
      const sigOf = (r) => (lang === 'c' ? r[1] : map ? (r.length > 3 ? r[3] : (map[r[0] != null ? r[0] : r[1]] || [null])[0]) : r[0]);
      const rows = all.filter((r) => sigOf(r) != null);
      if (!rows.length) return '';
      const body = rows.map((r) => {
        const sig = sigOf(r);
        const ownDesc = map && r.length <= 3 ? (map[r[0] != null ? r[0] : r[1]] || [])[1] : null;
        const desc = ownDesc || (Array.isArray(r[2]) ? ln(r[2][0], r[2][1], r[2][2] || r[2][0], r[2][3] || r[2][2] || r[2][0]) : r[2]);
        return `<tr data-api-row><td><code class="g-sig">${esc(sig)}</code></td><td>${esc(desc)}</td></tr>`;
      }).join('');
      return `<div class="g-api-group" data-api-group="${g.id}"><h3 id="api-${g.id}">${esc(g.title)}</h3><p>${esc(g.intro)}</p>
        <table class="g-table g-api"><thead><tr><th>${langName}</th><th>what it does</th></tr></thead><tbody>${body}</tbody></table></div>`;
    }).join('');
    return `
      <p>Every function the generated code and the runtime give you, grouped by what you want to do. Names follow the
      examples in this guide: <code>namespace: demo</code>, a message <code>Status</code>, an enum <code>Mode</code>, a
      text message <code>Gga</code> and a protocol <code>Link</code>; your schema's names take their place
      (${ln('<code>demo::Status</code>, <code>demo::Link</code>', '<code>demo_status_…</code>, <code>demo_link_…</code>; the prefix is the namespace, or <code>--opt prefix=</code>', '<code>demo.Status</code>, <code>demo.Link</code>, where <code>demo</code> is the generated module', '<code>demo.Status</code>, <code>demo.Link</code>, where <code>demo</code> is the generated module (<code>import link_gmp as demo</code>)')}).</p>
      <div class="g-api-bar">
        <input id="api-filter" type="search" placeholder="Filter, e.g. encode, poll, print, crc" aria-label="Filter the API reference">
        <div class="g-api-jump">${API.map((g) => `<a class="g-k" href="#api-${g.id}">${esc(g.title.split(':')[0])}</a>`).join('')}</div>
      </div>
      ${groups}
      <p class="g-api-none" hidden>No function matches the filter.</p>`;
  }

  /** Show only API rows containing every word of the filter (groups whose title matches keep all rows). */
  function filterApi(root, text) {
    const words = text.toLowerCase().split(/\s+/).filter(Boolean);
    let any = false;
    root.querySelectorAll('[data-api-group]').forEach((g) => {
      const head = g.querySelector('h3').textContent.toLowerCase();
      let shown = 0;
      g.querySelectorAll('[data-api-row]').forEach((r) => {
        const t = head + ' ' + r.textContent.toLowerCase();
        const ok = words.every((w) => t.includes(w));
        r.hidden = !ok;
        if (ok) shown++;
      });
      g.hidden = shown === 0;
      if (shown) any = true;
    });
    const none = root.querySelector('.g-api-none');
    if (none) none.hidden = any;
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
    ['api', 'API reference', apiRef],
    ['cpp', () => `Generated ${LANGS[lang]} and errors`, cpp],
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
        <div class="g-run-head"><span class="g-label">Decode ${esc(r.target.split(':')[1])}</span><span>${esc(typeof r.label === 'function' ? r.label() : r.label)}</span></div>
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
    const langSel = document.getElementById('guide-lang');
    try {
      const q = new URLSearchParams(location.search).get('lang');
      const saved = localStorage.getItem(LANG_KEY);
      if (LANGS[q]) lang = q;
      else if (LANGS[saved]) lang = saved;
    } catch (_) { /* storage may be unavailable */ }

    const toHash = () => {
      const t = location.hash && document.getElementById(location.hash.slice(1));
      if (t) t.scrollIntoView({ behavior: 'instant', block: 'start' });
    };
    let yaml = null;  // the js-yaml load: a promise, settled once
    const renderDemos = () => {
      yaml = yaml || G.loadJsYaml();
      yaml.then(() => {
        main.querySelectorAll('.g-demo').forEach(renderDemo);
        toHash();  // examples change the page height: land on the section again
        spy();
      }, (e) => {
        main.querySelectorAll('.g-demo').forEach((el) => {
          el.innerHTML = `<div class="callout bad">The live examples need js-yaml from cdnjs.cloudflare.com, which could not be loaded (${esc(e && e.message ? e.message : e)}). The rest of the guide works offline.</div>`;
        });
      });
    };
    const title = (t) => (typeof t === 'function' ? t() : t);
    let apiFilterText = '';  // kept when the language switch renders the page again
    const render = () => {
      document.documentElement.dataset.lang = lang;
      if (langSel) langSel.querySelectorAll('button[data-lang]').forEach((b) => b.classList.toggle('on', b.dataset.lang === lang));
      nav.innerHTML = `<div class="g-nav-title">Guide</div>${SECTIONS.map(([id, t], i) =>
        `<a href="#${id}" data-sec="${id}"><span>${String(i + 1).padStart(2, '0')}</span>${esc(title(t))}</a>`).join('')}
        <a class="g-nav-studio" href="index.html">← Back to the Studio</a>`;
      main.innerHTML = SECTIONS.map(([id, t, fn], i) =>
        `<section class="g-sec" id="${id}"><div class="g-sec-kicker">${String(i + 1).padStart(2, '0')}</div><h2>${esc(title(t))}</h2>${fn()}</section>`).join('') +
        '<footer class="g-foot">Every example on this page is a real schema, laid out and decoded by the same code as the Studio.</footer>';
      const filter = document.getElementById('api-filter');
      if (filter && apiFilterText) {
        filter.value = apiFilterText;
        filterApi(main, apiFilterText);
      }
      renderDemos();
    };
    if (langSel) langSel.addEventListener('click', (e) => {
      const b = e.target.closest('button[data-lang]');
      if (!b || b.dataset.lang === lang) return;
      // keep the reader's place: the section at the top stays at the top
      const secs = [...main.querySelectorAll('.g-sec')];
      let cur = secs[0];
      for (const sec of secs) if (sec.getBoundingClientRect().top - main.getBoundingClientRect().top < 120) cur = sec;
      lang = b.dataset.lang;
      try { localStorage.setItem(LANG_KEY, lang); } catch (_) { /* private mode */ }
      render();
      const again = cur && document.getElementById(cur.id);
      if (again) again.scrollIntoView({ behavior: 'instant', block: 'start' });
      spy();
    });

    main.addEventListener('input', (e) => {
      if (e.target.id !== 'api-filter') return;
      apiFilterText = e.target.value;
      filterApi(main, apiFilterText);
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
    function spy() {
      const links = [...nav.querySelectorAll('a[data-sec]')];
      const secs = [...main.querySelectorAll('.g-sec')];
      if (!secs.length) return;
      let cur = secs[0];
      for (const sec of secs) if (sec.getBoundingClientRect().top - main.getBoundingClientRect().top < 120) cur = sec;
      links.forEach((a) => a.classList.toggle('on', a.dataset.sec === cur.id));
    }
    main.addEventListener('scroll', spy, { passive: true });
    render();
    toHash();
    spy();
  };

  G.guideBuildFrame = buildFrame;
  G.GUIDE_DEMOS = DEMOS;
})(window.GMP);
