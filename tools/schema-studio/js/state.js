// state.js - data model, constructors, persistence and small utilities.
// All modules attach to the global GMP namespace (classic scripts so the app works from file://).
window.GMP = window.GMP || {};

(function (G) {
  'use strict';

  let counter = 0;
  G.uid = () => 'u' + (++counter).toString(36) + Math.random().toString(36).slice(2, 7);

  G.PRIMS = ['u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64', 'f32', 'f64', 'bool', 'char'];
  G.PRIM_SIZE = { u8: 1, u16: 2, u32: 4, u64: 8, i8: 1, i16: 2, i32: 4, i64: 8, f32: 4, f64: 8, bool: 1, char: 1 };
  G.INT_PRIMS = ['u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64'];
  G.COMPOSITES = ['bits', 'bytes', 'string', 'array', 'pad'];

  /** Built-in checksum algorithms -> bytes on the wire (mirrors generator/src/ir/ir.cpp). */
  G.CHECKSUMS = {
    crc8: 1, crc8_maxim: 1, crc8_autosar: 1, crc8_nrsc5: 1, crc15_can: 2, crc16_ccitt: 2, crc16_xmodem: 2, crc16_kermit: 2,
    crc16_x25: 2, crc16_modbus: 2, crc16_arc: 2, crc24_openpgp: 3, crc32: 4, crc32c: 4, crc32_mpeg2: 4,
    crc64_ecma: 8, xor8: 1, sum8: 1, lrc8: 1, sum16: 2, fletcher16: 2, fletcher8_ubx: 2,
  };

  // ----------------------------------------------------------------------------------------------
  // constructors
  // ----------------------------------------------------------------------------------------------
  G.newField = (over = {}) => ({
    uid: G.uid(), name: '', type: 'u8', endian: '', bit_order: '',
    lenMode: 'fixed', lenValue: '', max: '', terminator: '0', padding: '',
    of: 'u8', ofEndian: '',
    scale: '', offset: '', unit: '', const: '', when: '', optional: false,
    crc: '', crcParams: { width: '8', poly: '0x31', init: '0xFF', refin: false, refout: false, xorout: '0x00' },
    conversions: [],
    base: '', width: '', precision: '', doc: '', padSize: '1',
    bits: [],
    ...over,
  });

  G.newBit = (over = {}) => ({ uid: G.uid(), name: '', bits: '1', type: '', const: '', scale: '', offset: '', pad: false, doc: '', ...over });
  /** A computed value: `set` is '' (automatic setter when possible) or "field = expr; field = expr". */
  G.newComputed = (over = {}) => ({ uid: G.uid(), name: '', type: 'u32', value: '', set: '', unit: '', doc: '', ...over });
  /** Parse a computed setter list "a = value >> 8; b = value & 255" into [[field, expr], ...] (invalid parts skipped). */
  G.parseSetList = (text) => String(text || '').split(/[;\n]/).map((part) => {
    const m = /^\s*([A-Za-z_]\w*)\s*=(?!=)\s*(.+?)\s*$/.exec(part);
    return m ? [m[1], m[2]] : null;
  }).filter(Boolean);

  G.newConversion =(over = {}) => ({ uid: G.uid(), name: '', scale: '', offset: '', unit: '', ...over });

  /** Integer list "0x80, 0x81" / "80 81" -> numbers (null when any entry is not an integer). */
  G.parseIntList = (text) => {
    const parts = String(text || '').split(/[\s,]+/).filter(Boolean);
    const out = parts.map((x) => (G.isInt(x) ? G.parseInt(x) : (/^[0-9a-f]{1,2}$/i.test(x) ? parseInt(x, 16) : NaN)));
    return out.some((v) => Number.isNaN(v) || v < 0 || v > 255) ? null : out;
  };

  /**
   * Resolved byte stuffing of a protocol (mirrors the generator): the escape byte, the style and
   * [raw, code] for every reserved byte (escape byte included). `errors` lists problems; null when off.
   */
  G.escapeTable = (s, p) => {
    const fr = p.framing, e = fr.escape;
    if (!e || !e.enabled) return null;
    const errors = [];
    const hx = G.hex2;
    const esc = G.parseInt(e.char);
    if (Number.isNaN(esc) || esc < 0 || esc > 255) errors.push('escape byte must be 0..255');
    const style = e.style === 'map' || e.style === 'index' ? e.style : 'xor';
    const reserved = [];
    const add = (b) => { if (!reserved.includes(b)) reserved.push(b); };
    let codes = null;
    if (style === 'map') {
      codes = [];
      String(e.map || '').split(',').forEach((pair) => {
        if (!pair.trim()) return;
        const [a, b] = pair.split(':').map((x) => G.parseInt((x || '').trim()));
        if (Number.isNaN(a) || Number.isNaN(b)) { errors.push(`map entry "${pair.trim()}" must be raw:code`); return; }
        if (reserved.includes(a)) errors.push(`map lists ${hx(a)} twice`);
        reserved.push(a); codes.push(b);
      });
      if (!reserved.includes(esc)) errors.push(`map must include the escape byte ${hx(esc)} (it has to be escaped too)`);
    } else if (String(e.reserved || '').trim()) {
      const list = G.parseIntList(e.reserved);
      if (!list) errors.push('reserved bytes must be a list like "0x80, 0x81"');
      for (const b of list || []) {
        if (reserved.includes(b)) errors.push(`reserved lists ${hx(b)} twice`);
        reserved.push(b);
      }
      if (style === 'index' && !reserved.includes(esc)) errors.push(`reserved bytes must include the escape byte ${hx(esc)}`);
      if (style === 'xor') add(esc);
    } else {
      const sync = G.patternBytes(fr.syncMode, fr.sync) || [];
      const end = G.patternBytes(fr.endMode, fr.end) || [];  // as gmpc: sync / start and end bytes, then the escape byte
      sync.forEach(add); end.forEach(add); add(esc);
    }
    if (reserved.length > 16) errors.push('at most 16 reserved bytes (escape byte included)');
    const xor = G.parseInt(e.xor || '0x20');
    const base = G.parseInt(e.base || '0') || 0;
    const table = reserved.map((b, i) => [b, style === 'xor' ? (b ^ xor) & 0xff : style === 'index' ? base + i : codes[i]]);
    if (style === 'index' && base + reserved.length - 1 > 255) errors.push('base + reserved index exceeds 0xFF');
    table.forEach(([b, c], i) => {
      if (reserved.includes(c)) errors.push(`code ${hx(c)} for ${hx(b)} is itself a reserved byte${style === 'index' ? ' (set a base to move the codes)' : ''}`);
      const j = table.findIndex((x, k) => k < i && x[1] === c);
      if (j >= 0) errors.push(`code ${hx(c)} is used for both ${hx(table[j][0])} and ${hx(b)}`);
    });
    const byRaw = new Map(table), byCode = new Map(table.map(([b, c]) => [c, b]));
    return {
      esc, style, xor, base, reserved, table, errors,
      isReserved: (b) => byRaw.has(b),
      encode: (b) => (byRaw.has(b) ? byRaw.get(b) : null),
      decode: (c) => (style === 'xor' ? (c ^ xor) & 0xff : (byCode.has(c) ? byCode.get(c) : null)),
    };
  };

  /** Bytes on the wire for a checksum algorithm (protocol checksum or field `crc`); 0 when unknown. */
  G.checksumSize = (s, algo, crcParams) => {
    if (!algo) return 0;
    if (algo === '__crc') return Math.ceil((G.parseInt(crcParams && crcParams.width) || 0) / 8);
    if (G.CHECKSUMS[algo]) return G.CHECKSUMS[algo];
    const c = s.customChecksums.find((x) => x.name === algo);
    return c ? G.parseInt(c.size) || 0 : 0;
  };

  G.newMessage = (over = {}) => ({
    uid: G.uid(), name: 'Message', type_id: '', format: '', endian: '', bit_order: '', separator: '', access: '', doc: '',
    computed: [],
    fields: [], ...over,
  });

  G.newProtocol = (over = {}) => ({
    uid: G.uid(), name: 'Protocol', format: '', endian: '', separator: '', doc: '',
    header: [],
    framing: {
      type: 'none', syncMode: 'hex', sync: '', endMode: 'hex', end: '', size: '',
      lengthField: '', covers: 'body', adjust: '', counts: '', maxFrame: '', framer: '',
      escape: { enabled: false, style: 'xor', char: '0x7D', xor: '0x20', map: '0xC0:0xDC, 0xDB:0xDD', reserved: '', base: '' },
    },
    checksum: {
      enabled: false, algo: 'crc16_ccitt', covers: 'header_body', endian: '', separator: '*', required: true, over: '',
      crc: { width: '16', poly: '0x1021', init: '0xFFFF', refin: false, refout: false, xorout: '0x0' },
    },
    message_type: { kind: 'none', field: '', token: '0' },
    messages: [],
    ...over,
  });

  G.newEnum = (over = {}) => ({ uid: G.uid(), name: 'Enum', type: 'u8', strict: '', doc: '', values: [], ...over });
  /** Whether decode rejects undeclared values: the enum's `strict` ('' inherits defaults.strict_enums). */
  G.enumStrict = (s, e) => (e.strict === '' || e.strict == null ? !!s.schema.defaults.strict_enums : e.strict === 'true');
  G.newEnumValue = (name = '', value = '') => ({ uid: G.uid(), name, value });
  // `lang`: other language blocks ({ c: { header, type, ... } }), kept from imported YAML
  G.newCustomType = (over = {}) => ({ uid: G.uid(), name: 'custom_type', wire_size: '', max_wire_size: '', header: '', type: '', decode: '', encode: '', lang: {}, ...over });
  G.newCustomChecksum = (over = {}) => ({ uid: G.uid(), name: 'my_checksum', size: '1', header: '', function: '', lang: {}, ...over });
  G.newCustomFramer = (over = {}) => ({ uid: G.uid(), name: 'my_framer', header: '', type: '', lang: {}, ...over });

  G.emptyState = () => ({
    version: STATE_VERSION,  // defined in the persistence section below
    schema: {
      namespace: '',
      defaults: { endian: 'big', bit_order: 'msb_first', format: 'binary', separator: ',', access: 'both', strict_enums: false },
    },
    enums: [],
    customTypes: [],
    customChecksums: [],
    customFramers: [],
    messages: [],
    protocols: [],
  });

  // ----------------------------------------------------------------------------------------------
  // example schema (mirrors examples/binary_telemetry + a text protocol)
  // ----------------------------------------------------------------------------------------------
  G.exampleState = () => {
    const s = G.emptyState();
    s.schema.namespace = 'demo';
    s.enums.push(G.newEnum({ name: 'Mode', values: [G.newEnumValue('idle', '0'), G.newEnumValue('run', '1'), G.newEnumValue('fault', '2')] }));
    s.enums.push(G.newEnum({ name: 'MsgId', values: [G.newEnumValue('status', '1'), G.newEnumValue('samples', '2')] }));
    s.messages.push(G.newMessage({
      name: 'Fault', fields: [
        G.newField({ name: 'code', type: 'u16' }),
        G.newField({ name: 'temperature', type: 'i16', scale: '0.1', unit: 'degC' }),
      ],
    }));
    s.messages.push(G.newMessage({
      name: 'Status', type_id: 'status', doc: 'Periodic status report.', fields: [
        G.newField({ name: 'magic', type: 'u8', const: '0x5A' }),
        G.newField({ name: 'uptime_s', type: 'u32', unit: 's' }),
        G.newField({ name: 'voltage', type: 'u16', scale: '0.001', unit: 'V' }),
        G.newField({
          name: 'state', type: 'bits', bits: [
            G.newBit({ name: 'armed', bits: '1' }),
            G.newBit({ name: 'mode', bits: '2', type: 'Mode' }),
            G.newBit({ name: 'level', bits: '4', type: 'i8' }),
            G.newBit({ pad: true, bits: '1' }),
          ],
        }),
        G.newField({ name: 'fault', type: 'Fault', when: 'state.mode == Mode.fault' }),
        G.newField({ name: 'label', type: 'string', lenMode: 'fixed', lenValue: '8', padding: ' ' }),
      ],
    }));
    s.messages.push(G.newMessage({
      name: 'Samples', type_id: 'samples', fields: [
        G.newField({ name: 'channel', type: 'u8' }),
        G.newField({ name: 'count', type: 'u8' }),
        G.newField({ name: 'values', type: 'array', of: 'i16', lenMode: 'expr', lenValue: 'count', max: '64' }),
      ],
    }));
    s.messages.push(G.newMessage({
      name: 'GGA', type_id: 'GPGGA', format: 'text', doc: 'NMEA fix data.', fields: [
        G.newField({ name: 'utc_time', type: 'f64', precision: '2' }),
        G.newField({ name: 'latitude', type: 'f64', precision: '4', optional: true }),
        G.newField({ name: 'lat_hemi', type: 'char', optional: true }),
        G.newField({ name: 'longitude', type: 'f64', precision: '4', optional: true }),
        G.newField({ name: 'lon_hemi', type: 'char', optional: true }),
        G.newField({ name: 'quality', type: 'u8' }),
        G.newField({ name: 'satellites', type: 'u8', width: '2' }),
        G.newField({ name: 'rest', type: 'pad', padSize: '7' }),
      ],
    }));
    const link = G.newProtocol({ name: 'Link', messages: ['Status', 'Samples'] });
    link.header = [G.newField({ name: 'id', type: 'MsgId' }), G.newField({ name: 'length', type: 'u16' })];
    Object.assign(link.framing, { type: 'length_prefixed', sync: 'AA 55', lengthField: 'length', covers: 'body' });
    Object.assign(link.checksum, { enabled: true, algo: 'crc16_ccitt', covers: 'header_body' });
    link.message_type = { kind: 'field', field: 'id', token: '0' };
    s.protocols.push(link);

    const nmea = G.newProtocol({ name: 'Nmea', format: 'text', messages: ['GGA'] });
    Object.assign(nmea.framing, { type: 'delimited', syncMode: 'text', sync: '$', endMode: 'text', end: '\\r\\n', maxFrame: '82' });
    Object.assign(nmea.checksum, { enabled: true, algo: 'xor8', separator: '*', required: true });
    nmea.message_type = { kind: 'token', field: '', token: '0' };
    s.protocols.push(nmea);
    return s;
  };

  // ----------------------------------------------------------------------------------------------
  // lookups and effective values
  // ----------------------------------------------------------------------------------------------
  G.findEnum = (s, name) => s.enums.find((e) => e.name === name);
  G.findMessage = (s, name) => s.messages.find((m) => m.name === name);
  G.findCustomType = (s, name) => s.customTypes.find((c) => c.name === name);

  G.msgFormat = (s, m) => m.format || s.schema.defaults.format;
  G.msgEndian = (s, m) => m.endian || s.schema.defaults.endian;
  G.msgBitOrder = (s, m) => m.bit_order || s.schema.defaults.bit_order;
  G.msgSeparator = (s, m) => m.separator || s.schema.defaults.separator;
  G.msgAccess = (s, m) => m.access || s.schema.defaults.access;
  G.protoFormat = (s, p) => p.format || s.schema.defaults.format;
  G.protoEndian = (s, p) => p.endian || s.schema.defaults.endian;
  G.wantsStruct = (s, m) => G.msgAccess(s, m) !== 'view';
  G.wantsView = (s, m) => G.msgAccess(s, m) !== 'struct';

  /** The protocol header behaves like a binary message named <Proto>Header. */
  G.headerMessage = (s, p) => ({
    uid: p.uid + ':hdr', name: p.name + 'Header', format: 'binary', endian: G.protoEndian(s, p),
    bit_order: '', separator: '', access: 'both', fields: p.header, isHeader: true,
  });

  // ----------------------------------------------------------------------------------------------
  // literals and byte patterns
  // ----------------------------------------------------------------------------------------------
  G.INT_RE = /^-?(0x[0-9a-f]+|0b[01]+|\d+)$/i;
  G.isInt = (v) => typeof v === 'string' && G.INT_RE.test(v.trim());
  G.parseInt = (v) => {
    if (v === undefined || v === null) return NaN;
    const t = String(v).trim();
    if (!G.INT_RE.test(t)) return NaN;
    const neg = t.startsWith('-');
    const body = neg ? t.slice(1) : t;
    let n;
    if (/^0b/i.test(body)) n = parseInt(body.slice(2), 2);
    else n = Number(body);
    return neg ? -n : n;
  };
  G.isIdent = (v) => /^[A-Za-z_][A-Za-z0-9_]*$/.test(v || '');

  /** "AA 55" / "0xAA,0x55" -> [170, 85] (null on syntax error). */
  G.parseHexBytes = (text) => {
    const t = (text || '').trim();
    if (!t) return [];
    const parts = t.split(/[\s,]+/).filter(Boolean);
    const out = [];
    for (const p of parts) {
      const h = p.replace(/^0x/i, '');
      if (!/^[0-9a-f]{1,2}$/i.test(h)) return null;
      out.push(parseInt(h, 16));
    }
    return out;
  };

  /** Text with C escapes (\r \n \t \\ \xHH) -> bytes. */
  G.parseEscapedText = (text) => {
    const out = [];
    const s = text || '';
    for (let i = 0; i < s.length; i++) {
      const c = s[i];
      if (c !== '\\' || i + 1 >= s.length) { out.push(c.charCodeAt(0) & 0xff); continue; }
      const n = s[++i];
      if (n === 'r') out.push(13);
      else if (n === 'n') out.push(10);
      else if (n === 't') out.push(9);
      else if (n === '0') out.push(0);
      else if (n === '\\') out.push(92);
      else if (n === 'x' && /^[0-9a-f]{2}$/i.test(s.slice(i + 1, i + 3))) { out.push(parseInt(s.slice(i + 1, i + 3), 16)); i += 2; }
      else out.push(n.charCodeAt(0) & 0xff);
    }
    return out;
  };

  G.patternBytes = (mode, text) => (mode === 'text' ? G.parseEscapedText(text) : G.parseHexBytes(text));
  G.hex2 = (b) => '0x' + b.toString(16).toUpperCase().padStart(2, '0');
  G.bytesToEscaped = (bytes) =>
    bytes.map((b) => (b === 13 ? '\\r' : b === 10 ? '\\n' : b === 9 ? '\\t' : b === 92 ? '\\\\' : b >= 32 && b < 127 ? String.fromCharCode(b) : '\\x' + b.toString(16).padStart(2, '0'))).join('');

  // ----------------------------------------------------------------------------------------------
  // paths
  // ----------------------------------------------------------------------------------------------
  G.splitPath = (p) => p.split('.').map((k) => (/^\d+$/.test(k) ? Number(k) : k));
  G.getPath = (obj, path) => G.splitPath(path).reduce((o, k) => (o == null ? undefined : o[k]), obj);
  G.setPath = (obj, path, value) => {
    const keys = G.splitPath(path);
    let o = obj;
    for (let i = 0; i < keys.length - 1; i++) {
      if (o[keys[i]] == null) o[keys[i]] = typeof keys[i + 1] === 'number' ? [] : {};  // e.g. lang.c on first edit
      o = o[keys[i]];
    }
    o[keys[keys.length - 1]] = value;
  };

  G.esc = (v) => String(v ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

  // ----------------------------------------------------------------------------------------------
  // persistence
  // ----------------------------------------------------------------------------------------------
  const KEY = 'gmp-schema-studio-v1';
  /** Shape of the saved state; saved state with another version is ignored (no migration). */
  const STATE_VERSION = 2;

  G.save = (s) => { try { localStorage.setItem(KEY, JSON.stringify(s)); } catch (_) { /* private mode etc. */ } };
  G.load = () => {
    try {
      const raw = localStorage.getItem(KEY);
      if (!raw) return null;
      const s = JSON.parse(raw);
      return s && s.version === STATE_VERSION ? s : null;
    } catch (_) { return null; }
  };

  /** Rename references to a type (message / enum / custom type) everywhere. */
  G.renameType = (s, oldName, newName) => {
    if (!oldName || oldName === newName) return;
    const fix = (f) => {
      if (f.type === oldName) f.type = newName;
      if (f.of === oldName) f.of = newName;
      (f.bits || []).forEach((b) => { if (b.type === oldName) b.type = newName; });
      if (f.when) f.when = f.when.replace(new RegExp('\\b' + oldName + '\\.', 'g'), newName + '.');
    };
    s.messages.forEach((m) => m.fields.forEach(fix));
    s.protocols.forEach((p) => {
      p.header.forEach(fix);
      p.messages = p.messages.map((n) => (n === oldName ? newName : n));
    });
  };
})(window.GMP);
