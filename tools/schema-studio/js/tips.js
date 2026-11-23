// tips.js - hover help for the editor (left pane): what each option means and the YAML it produces.
// Entries are keyed by a normalised data-path (see tipKey); `v` describes individual select values.
(function (G) {
  'use strict';

  const INT = 'Integer stored in';
  const TYPE_VALUES = {
    u8: `${INT} 1 byte, 0..255.`, u16: `${INT} 2 bytes, 0..65535.`, u32: `${INT} 4 bytes, unsigned.`, u64: `${INT} 8 bytes, unsigned.`,
    i8: `${INT} 1 byte, two's complement (-128..127).`, i16: `${INT} 2 bytes, signed.`, i32: `${INT} 4 bytes, signed.`, i64: `${INT} 8 bytes, signed.`,
    f32: 'IEEE-754 single precision float, 4 bytes.', f64: 'IEEE-754 double precision float, 8 bytes.',
    bool: 'One byte: 0 = false, anything else = true (encodes as 1).', char: 'One byte holding a character.',
    bits: 'A group of bit fields packed into whole bytes (flags, small enums, 12-bit values…). Edit the bits below.',
    bytes: 'Raw bytes: fixed length, a length from an earlier field, the rest of the message, or up to a terminator.',
    string: 'Characters. Binary: fixed / length field / terminated. Text: one token.',
    array: 'Repeated elements of one type (scalar, enum, nested message or custom). Count is fixed, from a field, or "the rest".',
    pad: 'Reserved bytes (binary) or skipped tokens (text). Ignored on decode, written as zeros / empty on encode.',
  };

  const ENDIAN = { '': 'Inherit the byte order from the enclosing message / protocol / schema defaults.', big: 'Most significant byte first (network order): 0x1234 → 12 34.', little: 'Least significant byte first (x86, ARM, CAN Intel): 0x1234 → 34 12.' };
  const BIT_ORDER = { '': 'Inherit the bit order.', msb_first: 'The first bit field takes the most significant bits of the first byte (Motorola / network style).', lsb_first: 'The first bit field takes the least significant bits (Intel / CAN DBC style).' };
  const FORMAT = { '': 'Inherit the format from the schema defaults.', binary: 'Fixed byte layout: fields are bytes on the wire.', text: 'ASCII tokens separated by a character, like NMEA: $GPGGA,123519,4807.038,N,…' };
  const ACCESS = {
    '': 'Inherit from the schema defaults.',
    both: 'Generate both a zero-copy View (read straight from the buffer) and an eager struct (decode / encode).',
    view: 'Only the View: decode-only accessors over the input bytes, no copy.',
    struct: 'Only the struct: a value type with fixed-capacity containers, needed for encoding.',
  };

  const T = {
    // ---------------------------------------------------------------- schema
    'schema.namespace': { t: 'Namespace', k: 'namespace', d: 'C++ namespace for everything generated from this schema, e.g. acme::telemetry.' },
    'schema.defaults.endian': { t: 'Default endian', k: 'defaults.endian', d: 'Byte order of multi-byte fields unless a message, protocol or field overrides it.', v: ENDIAN },
    'schema.defaults.bit_order': { t: 'Default bit order', k: 'defaults.bit_order', d: 'How bit fields inside a bits group are packed.', v: BIT_ORDER },
    'schema.defaults.format': { t: 'Default format', k: 'defaults.format', d: 'Whether messages are binary layouts or text sentences by default.', v: FORMAT },
    'schema.defaults.access': { t: 'Default access', k: 'defaults.access', d: 'Which C++ types are generated per message.', v: ACCESS },
    'schema.defaults.separator': { t: 'Text separator', k: 'defaults.separator', d: 'Character between tokens of text messages (NMEA uses a comma).' },
    'schema.defaults.strict_enums': { t: 'Strict enums', k: 'defaults.strict_enums', d: 'Reject enum values that are not declared (UnknownEnum) on decode and encode. Individual enums can opt out with strict: false.' },

    // ---------------------------------------------------------------- enums
    'enum.name': { t: 'Enum name', k: 'enums.<Name>', d: 'Becomes a C++ enum class with to_string() and is_known(). Use it as a field type.' },
    'enum.type': { t: 'Storage type', k: 'type', d: 'Integer type the enum is stored as on the wire.' },
    'enum.strict': { t: 'Unknown values', k: 'strict', d: 'What happens when a value that is not listed is decoded or encoded.', v: { '': 'Follow defaults.strict_enums.', true: 'Reject undeclared values with UnknownEnum.', false: 'Accept any value (it is kept as a raw number).' } },
    'enum.doc': { t: 'Doc', k: 'doc', d: 'Description copied into the generated code as a comment.' },
    'enum.values.name': { t: 'Value name', k: 'values.<name>', d: 'Enumerator name. Can be used as a message type_id and in when: conditions (Mode.fault).' },
    'enum.values.value': { t: 'Value', k: 'values.<name>: <n>', d: 'Number stored on the wire for this enumerator (decimal or 0x hex).' },

    // ---------------------------------------------------------------- custom extensions
    'customType.name': { t: 'Custom type name', k: 'custom.types.<name>', d: 'Use this name as a field type; your own functions (C++ and / or C) decode and encode it.' },
    'customType.wire_size': { t: 'Wire size', k: 'wire_size', d: 'Bytes on the wire if the size is fixed. Leave empty for variable size.' },
    'customType.max_wire_size': { t: 'Max wire size', k: 'max_wire_size', d: 'Upper bound for variable-size custom types, so frame sizes stay bounded.' },
    'customType.header': { t: 'Header', k: 'cpp.header', d: 'C++ header to #include that declares the type and functions.' },
    'customType.type': { t: 'C++ type', k: 'cpp.type', d: 'The value type stored in structs, e.g. my::Time.' },
    'customType.decode': { t: 'Decode function', k: 'cpp.decode', d: 'gmp::Result<T> fn(gmp::ByteReader&) for binary, or fn(std::string_view) for text. Checked by a concept with a clear error.' },
    'customType.encode': { t: 'Encode function', k: 'cpp.encode', d: 'gmp::VoidResult fn(gmp::ByteWriter&, const T&) (binary) or fn(gmp::text::TextWriter&, const T&) (text).' },
    'customChecksum.name': { t: 'Checksum name', k: 'custom.checksums.<name>', d: 'Select this name as a protocol checksum algo or a field crc.' },
    'customChecksum.size': { t: 'Size', k: 'size', d: 'Bytes the checksum occupies on the wire.' },
    'customChecksum.header': { t: 'Header', k: 'cpp.header', d: 'C++ header declaring the function.' },
    'customChecksum.function': { t: 'Function', k: 'cpp.function', d: 'unsigned fn(gmp::bytes_view) returning the checksum of the covered bytes.' },
    'customFramer.name': { t: 'Framer name', k: 'custom.framers.<name>', d: 'Select it with framing type "custom".' },
    'customFramer.header': { t: 'Header', k: 'cpp.header', d: 'C++ header declaring the framer.' },
    'customFramer.type': { t: 'C++ type', k: 'cpp.type', d: 'A type with static gmp::FrameScan scan(gmp::bytes_view): says where the next frame starts and ends.' },
    'customType.lang.c.header': { t: 'C header', k: 'c.header', d: 'C header to #include that declares the type and functions (C output only: gmpc --lang c).' },
    'customType.lang.c.type': { t: 'C type', k: 'c.type', d: 'The C value type stored in structs, e.g. my_time (a typedef). A standard scalar type such as double prints like a number.' },
    'customType.lang.c.decode': { t: 'C decode function', k: 'c.decode', d: 'gmsg_error fn(gmsg_reader*, T*) for binary, or gmsg_error fn(const char*, size_t, T*) for text. The signature is checked at compile time.' },
    'customType.lang.c.encode': { t: 'C encode function', k: 'c.encode', d: 'gmsg_error fn(gmsg_writer*, const T*) (binary) or gmsg_error fn(gmsg_text_writer*, const T*) (text). Needed when structs are generated.' },
    'customType.lang.c.print': { t: 'C print function', k: 'c.print', d: 'Optional: void fn(gmsg_printer*, const T*), used by the generated _print functions (write with gmsg_print_text). Without it the value prints as <?>.' },
    'customChecksum.lang.c.header': { t: 'C header', k: 'c.header', d: 'C header declaring the function (C output only).' },
    'customChecksum.lang.c.function': { t: 'C function', k: 'c.function', d: 'An unsigned integer fn(const uint8_t* data, size_t size) returning the checksum of the covered bytes (uint8_t ... uint64_t).' },
    'customFramer.lang.c.header': { t: 'C header', k: 'c.header', d: 'C header declaring the scan function (C output only).' },
    'customType.lang.js.module': { t: 'JavaScript module', k: 'js.module', d: 'Module id that the generated JavaScript requires for the functions below, e.g. ./my_codecs.js (JavaScript output only: gmpc --lang js). In a browser page the module is looked up in the global gmsgExtensions object.' },
    'customType.lang.js.decode': { t: 'JavaScript decode function', k: 'js.decode', d: 'Export of the module: decode(reader) for binary (a gmsg.Reader positioned at the value) or decode(text) for text; returns the value and throws gmsg.GmpError on bad input.' },
    'customType.lang.js.encode': { t: 'JavaScript encode function', k: 'js.encode', d: 'Export of the module: encode(writer, value) writes the value with a gmsg.Writer (binary) or gmsg.text.TextWriter (text). Needed when structs are generated.' },
    'customType.lang.js.print': { t: 'JavaScript print function', k: 'js.print', d: 'Optional: print(printer, value) for gmsg.print / toString(). Without it numbers, BigInts and strings print as such, other values as <?>.' },
    'customChecksum.lang.js.module': { t: 'JavaScript module', k: 'js.module', d: 'Module id the generated JavaScript requires for the checksum function (JavaScript output only).' },
    'customChecksum.lang.js.function': { t: 'JavaScript function', k: 'js.function', d: 'Export of the module: fn(bytes: Uint8Array) returning the checksum of the covered bytes as a Number or BigInt.' },
    'customFramer.lang.js.module': { t: 'JavaScript module', k: 'js.module', d: 'Module id the generated JavaScript requires for the scan function (JavaScript output only).' },
    'customFramer.lang.js.scan': { t: 'JavaScript scan function', k: 'js.scan', d: 'Export of the module: scan(bytes: Uint8Array) returning gmsg.FrameScan.needMore(hint), .frame(size, consume, consumeOnError) or .skip(n, error).' },
    'customFramer.lang.c.scan': { t: 'C scan function', k: 'c.scan', d: 'gmsg_frame_scan fn(const uint8_t* data, size_t size): says where the next frame starts and ends (gmsg_scan_frame / _need_more / _skip).' },

    // ---------------------------------------------------------------- messages
    'message.name': { t: 'Message name', k: 'messages.<Name>', d: 'Generates struct <Name>, view <Name>View and decode/encode functions. Also usable as a nested field type.' },
    'message.type_id': { t: 'Type id', k: 'type_id', d: 'Value that identifies this message inside a protocol: the header field value (binary, number or enum value) or the first token (text, e.g. GPGGA).' },
    'message.format': { t: 'Format', k: 'format', d: 'Binary layout or text tokens.', v: FORMAT },
    'message.access': { t: 'Access', k: 'access', d: 'Which C++ types are generated for this message.', v: ACCESS },
    'message.endian': { t: 'Endian', k: 'endian', d: 'Byte order for the fields of this message.', v: ENDIAN },
    'message.bit_order': { t: 'Bit order', k: 'bit_order', d: 'Bit packing for bits groups in this message.', v: BIT_ORDER },
    'message.separator': { t: 'Separator', k: 'separator', d: 'Token separator for this text message.' },
    'message.doc': { t: 'Doc', k: 'doc', d: 'Description copied into the generated code.' },

    // ---------------------------------------------------------------- fields
    'field.name': { t: 'Field name', k: 'name', d: 'Member name in the struct and accessor name in the view. Later fields can refer to it in length, count and when expressions.' },
    'field.type': { t: 'Type', k: 'type', d: 'What the bytes (or token) mean. Enums, messages and custom types defined in this schema are listed too.', v: TYPE_VALUES },
    'field.endian': { t: 'Endian', k: 'endian', d: 'Byte order of this field only.', v: ENDIAN },
    'field.bit_order': { t: 'Bit order', k: 'bit_order', d: 'Bit packing of this bits group.', v: BIT_ORDER },
    'field.lenMode': {
      t: 'Length / count', k: 'length / count', d: 'How many bytes (bytes, string) or elements (array) the field has.',
      v: {
        fixed: 'Always the same number: length: 8 / count: 4.',
        expr: 'From an earlier field or an expression: length: len or count: "n * 2". On encode the source field is filled in automatically when it is a plain field.',
        remaining: 'Everything up to the end of the message (last field only).',
        terminator: 'Up to a terminator byte, e.g. a NUL-terminated string. The terminator is consumed but not stored.',
      },
    },
    'field.lenValue': { t: 'Length / count value', k: 'length / count', d: 'A number, an earlier field name, or an expression with + - * / & | << >> over earlier fields.' },
    'field.terminator': { t: 'Terminator', k: 'terminator', d: 'Byte value that ends the field (0 for C strings).' },
    'field.max': { t: 'Max (capacity)', k: 'max', d: 'Capacity of a variable-length bytes, string or array field in the struct (no heap). Decoding more fails with OutOfRange. It is not a value limit (use Min / Max value); on other fields, or a fixed size it does not match, gmpc reports an error.' },
    'field.min_value': { t: 'Min value', k: 'min_value', d: 'Lowest allowed value, inclusive: the scaled value (raw × scale + offset) when the field has a scale or offset, else the value itself. Decoding (struct and view) and encoding fail with ValueOutOfLimits below it; NaN always fails.' },
    'field.max_value': { t: 'Max value', k: 'max_value', d: 'Highest allowed value, inclusive: the scaled value (raw × scale + offset) when the field has a scale or offset, else the value itself. Decoding (struct and view) and encoding fail with ValueOutOfLimits above it; NaN always fails.' },
    'field.min_raw': { t: 'Min raw', k: 'min_raw', d: 'Lowest allowed raw wire value, inclusive, before any scale / offset; integers compare exactly. Decoding and encoding fail with ValueOutOfLimits below it.' },
    'field.max_raw': { t: 'Max raw', k: 'max_raw', d: 'Highest allowed raw wire value, inclusive, before any scale / offset; integers compare exactly. Decoding and encoding fail with ValueOutOfLimits above it.' },
    'field.ofLimits.min_value': { t: 'Element min value', k: 'min_value', d: 'Lowest allowed value, inclusive for every element: the element value. Decoding (struct and view) and encoding fail with ValueOutOfLimits below it; NaN always fails.' },
    'field.ofLimits.max_value': { t: 'Element max value', k: 'max_value', d: 'Highest allowed value, inclusive for every element: the element value. Decoding (struct and view) and encoding fail with ValueOutOfLimits above it; NaN always fails.' },
    'field.ofLimits.min_raw': { t: 'Element min raw', k: 'min_raw', d: 'Lowest allowed raw wire value, inclusive, before any scale / offset; integers compare exactly. Decoding and encoding fail with ValueOutOfLimits below it.' },
    'field.ofLimits.max_raw': { t: 'Element max raw', k: 'max_raw', d: 'Highest allowed raw wire value, inclusive, before any scale / offset; integers compare exactly. Decoding and encoding fail with ValueOutOfLimits above it.' },

    'field.of': { t: 'Element type', k: 'of', d: 'Type of each array element: scalar, enum, nested message or custom type.' },
    'field.ofEndian': { t: 'Element endian', k: 'endian (elements)', d: 'Byte order of each element.', v: ENDIAN },
    'field.padding': { t: 'Padding char', k: 'padding', d: 'Fixed-length strings: fill character used on encode and stripped on decode (default NUL).' },
    'field.const': { t: 'Const', k: 'const', d: 'Magic / fixed value. Decoding fails with BadConst if it differs; encoding always writes it. Not stored in the struct.' },
    'field.crc': { t: 'Field CRC', k: 'crc', d: 'A checksum stored right after this value (e.g. Sensirion word CRCs). Verified on decode (BadChecksum) and written on encode.' },
    'field.scale': { t: 'Scale', k: 'scale', d: 'Engineering value = raw × scale + offset. Generates <name>_scaled() and set_<name>_scaled(); the raw value is still available.' },
    'field.offset': { t: 'Offset', k: 'offset', d: 'Added after scaling: value = raw × scale + offset.' },
    'field.unit': { t: 'Unit', k: 'unit', d: 'Label for the value: shown in comments, the decode tester and gmp::print output (after the scaled value, or after the raw value when there is no scale). Does not change decoding.' },
    'field.base': { t: 'Number base', k: 'base', d: 'Text integers: decimal or hexadecimal digits.', v: { '': 'Decimal digits.', 16: 'Hexadecimal digits (A–F, either case on decode).' } },
    'field.width': { t: 'Width', k: 'width', d: 'Text: minimum digits on encode, zero padded (e.g. 2 → "07").' },
    'field.precision': { t: 'Precision', k: 'precision', d: 'Text floats: decimals written on encode. Empty = shortest exact form.' },
    'field.when': { t: 'Present when', k: 'when', d: 'Condition over earlier fields; the field only exists on the wire when it is true (e.g. flags.mode == Mode.fault). In C++ it becomes std::optional.' },
    'field.optional': { t: 'Optional', k: 'optional', d: 'Text: an empty token is allowed and decodes as "no value" (std::optional).' },
    'field.doc': { t: 'Doc', k: 'doc', d: 'Description copied into the generated code.' },
    'field.padSize': { t: 'Padding size', k: 'pad', d: 'Number of reserved bytes (binary) or tokens to skip (text).' },

    'crc.width': { t: 'CRC width', k: 'crc.width', d: 'Number of bits (1..64). The stored size is width rounded up to whole bytes.' },
    'crc.poly': { t: 'Polynomial', k: 'crc.poly', d: 'Generator polynomial without the top bit, e.g. 0x1021 for CRC-16/CCITT.' },
    'crc.init': { t: 'Init', k: 'crc.init', d: 'Starting register value.' },
    'crc.xorout': { t: 'XorOut', k: 'crc.xorout', d: 'Value XOR-ed into the final result.' },
    'crc.refin': { t: 'RefIn', k: 'crc.refin', d: 'Process input bytes least significant bit first.' },
    'crc.refout': { t: 'RefOut', k: 'crc.refout', d: 'Bit-reverse the result before XorOut.' },

    'conv.name': { t: 'Conversion name', k: 'conversions.<name>', d: 'Adds <field>_<name>() and set_<field>_<name>(), e.g. temperature_degf().' },
    'conv.scale': { t: 'Scale', k: 'scale', d: 'value = raw × scale + offset, applied to the raw field value.' },
    'conv.offset': { t: 'Offset', k: 'offset', d: 'Added after scaling.' },
    'conv.unit': { t: 'Unit', k: 'unit', d: 'Label for the converted value: shown in comments, the decode tester and gmp::print output.' },

    'bit.name': { t: 'Bit field name', k: 'bits[].name', d: 'Accessor / member name of this bit field.' },
    'bit.bits': { t: 'Bits', k: 'bits', d: 'Width in bits. The whole group must add up to whole bytes.' },
    'bit.type': { t: 'Storage type', k: 'type / enum', d: 'C++ type used for the value: auto picks bool for 1 bit or the smallest unsigned type; an enum maps the bits to named values.' },
    'bit.const': { t: 'Const', k: 'const', d: 'Fixed value checked on decode (BadConst) and always written on encode.' },
    'bit.scale': { t: 'Scale', k: 'scale', d: 'Engineering value = raw × scale.' },
    'bit.min_value': { t: 'Min value', k: 'min_value', d: 'Lowest allowed value, inclusive: the scaled value when the bit field has a scale, else its value. Decoding (struct and view) and encoding fail with ValueOutOfLimits below it; NaN always fails.' },
    'bit.max_value': { t: 'Max value', k: 'max_value', d: 'Highest allowed value, inclusive: the scaled value when the bit field has a scale, else its value. Decoding (struct and view) and encoding fail with ValueOutOfLimits above it; NaN always fails.' },
    'bit.min_raw': { t: 'Min raw', k: 'min_raw', d: 'Lowest allowed raw wire value, inclusive, before any scale / offset; it must fit the bit width. Decoding and encoding fail with ValueOutOfLimits below it.' },
    'bit.max_raw': { t: 'Max raw', k: 'max_raw', d: 'Highest allowed raw wire value, inclusive, before any scale / offset; it must fit the bit width. Decoding and encoding fail with ValueOutOfLimits above it.' },


    'computed.name': { t: 'Computed value', k: 'computed.<name>', d: 'A value derived from fields with no bytes of its own: generates name() on the struct and view.' },
    'computed.type': { t: 'Type', k: 'type', d: 'C++ type of the computed value.' },
    'computed.value': { t: 'Value', k: 'value', d: 'Expression over fields, e.g. (serial_high << 16) | serial_low.' },
    'computed.set': { t: 'Setter', k: 'set', d: 'Optional "field = expr over value; …". Without it, a setter is derived automatically for shift / | / + packing of unsigned fields.' },
    'computed.unit': { t: 'Unit', k: 'unit', d: 'Label for the computed value: shown in comments, the decode tester and gmp::print output.' },

    // ---------------------------------------------------------------- protocols
    'protocol.name': { t: 'Protocol name', k: 'protocols.<Name>', d: 'Generates struct <Name> with parse(), decode(), encode() and a Stream<> parser for byte streams.' },
    'protocol.format': { t: 'Format', k: 'format', d: 'Binary frames or text sentences.', v: FORMAT },
    'protocol.endian': { t: 'Endian', k: 'endian', d: 'Byte order of the header fields and the checksum.', v: ENDIAN },
    'protocol.separator': { t: 'Separator', k: 'separator', d: 'Text token separator.' },
    'protocol.exact_size': { t: 'Exact size', k: 'exact_size', d: 'A frame with data after its message (more bytes, or more tokens, than the message uses) fails with BadLength. Without it such frames are decoded and the extra data is ignored, but reported (Received::extra, extra_bytes() / extra_tokens(), stats frames_with_extra). Not for fixed framing, which pads shorter messages.' },
    'protocol.doc': { t: 'Doc', k: 'doc', d: 'Description copied into the generated code.' },
    'protocol.framing.type': {
      t: 'Framing', k: 'framing.type', d: 'How frames are found in a byte stream.',
      v: {
        none: 'The whole buffer is one frame (datagrams, CAN payloads). No stream parser.',
        fixed: 'Every frame has the same size, optionally starting with a sync pattern.',
        length_prefixed: 'A header field gives the size; the parser waits for exactly that many bytes.',
        delimited: 'Frames end with an end pattern (and optionally start with one), like "$…\\r\\n" or HDLC 7E…7E.',
        custom: 'Your own C++ framer decides where frames start and end.',
      },
    },
    'protocol.framing.syncMode': { t: 'Pattern format', k: '', d: 'Write the pattern as hex bytes (AA 55) or as text with escapes ($, \\r\\n).', v: { hex: 'Hex bytes, e.g. AA 55.', text: 'Characters, e.g. $ or \\r\\n.' } },
    'protocol.framing.endMode': { t: 'Pattern format', k: '', d: 'Write the pattern as hex bytes or as text.', v: { hex: 'Hex bytes, e.g. 0D 0A.', text: 'Characters, e.g. \\r\\n.' } },
    'protocol.framing.sync': { t: 'Sync / start', k: 'framing.sync / start', d: 'Bytes at the start of every frame (up to 8). After garbage the parser skips ahead to the next match (BadSync).' },
    'protocol.framing.end': { t: 'End delimiter', k: 'framing.end', d: 'Bytes that end every frame: required for delimited framing, optional after fixed and length-prefixed frames (never escaped). When start and end are equal (HDLC 0x7E) one flag can close a frame and open the next.' },
    'protocol.framing.size': { t: 'Frame size', k: 'framing.size', d: 'Total bytes per frame, before escaping. Empty = derived when all messages have the same fixed size. Shorter bodies are zero padded.' },
    'protocol.framing.lengthField': { t: 'Length field', k: 'framing.length.field', d: 'Header field (unsigned integer) holding the size. Filled in automatically on encode.' },
    'protocol.framing.covers': {
      t: 'Length covers', k: 'framing.length.covers', d: 'What the length value measures.',
      v: {
        body: 'Only the message body.',
        body_checksum: 'The body plus the checksum bytes.',
        header_body: 'Header (including the length field) plus body.',
        all: 'The whole frame: sync, header, body and checksum.',
      },
    },
    'protocol.framing.adjust': { t: 'Adjust', k: 'framing.length.adjust', d: 'Added to the length value: covered size = value + adjust (for protocols that count e.g. one extra byte).' },
    'protocol.framing.counts': {
      t: 'Length counts', k: 'framing.length.counts', d: 'With byte stuffing: does the length count bytes before or after escaping?',
      v: { '': 'Unescaped bytes (default). The parser walks the escaped bytes to find the frame end.', wire: 'Bytes on the wire, after escaping. Only for covers: body or body_checksum (with a body-only checksum), so the length never counts its own escaping.' },
    },
    'protocol.framing.maxFrame': { t: 'Max frame', k: 'framing.max_frame', d: 'Largest frame in bytes: bounds length checks and sizes the stream buffer. Derived automatically when every message is bounded (doubled with escaping).' },
    'protocol.framing.framer': { t: 'Custom framer', k: 'framing.framer', d: 'An entry of custom.framers.' },
    'protocol.framing.escape.enabled': { t: 'Byte stuffing', k: 'framing.escape', d: 'Reserved bytes (sync / delimiter values) never appear inside a frame: each one is sent as the escape byte plus a code. Decoding unescapes in place.' },
    'protocol.framing.escape.style': {
      t: 'Escape style', k: 'escape.code', d: 'How the code byte after the escape byte is chosen.',
      v: {
        xor: 'HDLC / PPP: code = byte ⊕ mask (7E → 7D 5E).',
        index: 'Code = position of the byte in the reserved list (+ base): 80 → 81 00, 81 → 81 01.',
        map: 'SLIP: explicit raw:code pairs (C0 → DB DC, DB → DB DD).',
      },
    },
    'protocol.framing.escape.char': { t: 'Escape byte', k: 'escape.char', d: 'Byte that announces an escaped value. It is always reserved itself.' },
    'protocol.framing.escape.reserved': { t: 'Reserved bytes', k: 'escape.reserved', d: 'Bytes that must never appear raw inside a frame, in order (the order gives index codes). Default: sync/start bytes, end bytes, then the escape byte. Up to 16.' },
    'protocol.framing.escape.xor': { t: 'XOR mask', k: 'escape.xor', d: 'Code = byte ⊕ mask (HDLC uses 0x20).' },
    'protocol.framing.escape.base': { t: 'Base', k: 'escape.base', d: 'Index style: code of the first reserved byte. Use it when 0x00 is reserved, since no code may be a reserved byte.' },
    'protocol.framing.escape.map': { t: 'Map', k: 'escape.map', d: 'raw:code pairs. The keys are the reserved bytes and must include the escape byte.' },
    'protocol.checksum.enabled': { t: 'Checksum', k: 'checksum', d: 'Validate each frame with a checksum stored after the body. A mismatch drops the frame (BadChecksum) and the stream resynchronises.' },
    'protocol.checksum.algo': { t: 'Algorithm', k: 'checksum.algo', d: 'A built-in CRC / sum, a custom checksum, or inline CRC parameters.', v: { crc16_ccitt: 'CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF.', crc32: 'CRC-32 as used by zip / Ethernet.', crc8: 'CRC-8/SMBUS: poly 0x07.', crc8_nrsc5: 'Sensirion word CRC: poly 0x31, init 0xFF.', crc16_modbus: 'Modbus RTU CRC (little endian on the wire).', crc16_x25: 'HDLC / X.25 frame check sequence.', xor8: 'XOR of all bytes (NMEA).', sum8: 'Sum of all bytes modulo 256.', __crc: 'Enter width, polynomial, init, reflection and xorout yourself.' } },
    'protocol.checksum.covers': { t: 'Covers', k: 'checksum.covers', d: 'Bytes the checksum is computed over.', v: { header_body: 'Header and body (default).', body: 'Body only.', all: 'Sync / start bytes, header and body.' } },
    'protocol.checksum.endian': { t: 'Byte order', k: 'checksum.endian', d: 'Byte order of the stored checksum.', v: ENDIAN },
    'protocol.checksum.over': { t: 'Computed over', k: 'checksum.over', d: 'With byte stuffing: compute the checksum before or after escaping.', v: { '': 'The unescaped bytes (default).', wire: 'The escaped bytes as sent. The checksum itself is escaped either way.' } },
    'protocol.checksum.separator': { t: 'Separator', k: 'checksum.separator', d: 'Text: the character before the hex checksum digits (NMEA "*").' },
    'protocol.checksum.required': { t: 'Required', k: 'checksum.required', d: 'Text: reject sentences without a checksum. When off, a present checksum is still verified.' },
    'protocol.message_type.kind': {
      t: 'Message type', k: 'message_type', d: 'How the parser decides which message a frame carries.',
      v: { none: 'The protocol carries a single message.', field: 'A header field holds the type id; each message declares its type_id.', token: 'Text: the first token (e.g. GPGGA) is matched against each message\'s type_id.' },
    },
    'protocol.message_type.field': { t: 'Type field', k: 'message_type.field', d: 'Header field compared with each message\'s type_id. Unknown values fail with UnknownType.' },
    'protocol.header': { t: 'Header', k: 'header', d: 'Fields that come before the body in every frame (type id, length, address…).' },
  };

  const ACTIONS = {
    addProtocol: 'Add a protocol: framing, header, checksum and the messages it carries.',
    addMessage: 'Add a message: a named list of fields.',
    addEnum: 'Add an enum: named integer values usable as field types.',
    addCustomType: 'Add a custom field type decoded by your own C++ or C functions.',
    addCustomChecksum: 'Add a checksum implemented by your own C++ or C function.',
    addCustomFramer: 'Add a framer implemented in your own code (C++ or C), for framing GMP does not describe.',
    addField: 'Add a field at the end of the list.',
    addBit: 'Add a bit field (or padding bits) to this group.',
    addConversion: 'Add another unit conversion for this field, e.g. °F next to °C.',
    addComputed: 'Add a value computed from fields, e.g. a serial number split across two words.',
    addEnumValue: 'Add an enumerator.',
    toggleProtoMsg: 'Include or exclude this message from the protocol.',
    move: 'Move up / down: field order is wire order.',
    duplicate: 'Duplicate this item.',
    delete: 'Remove this item.',
    duplicateItem: 'Duplicate.',
    deleteItem: 'Delete.',
  };
  const CHIPS = {
    schema: 'Schema-wide settings and defaults (namespace, endian, format…).',
    protocol: 'A protocol: how frames are found and which message each frame carries.',
    message: 'A message: its fields define the byte layout.',
    enum: 'An enum: named integer values.',
    custom: 'A custom extension implemented in your own C++ or C code.',
  };

  /** messages.3.fields.0.scale -> field.scale, protocols.0.framing.type -> protocol.framing.type, … */
  G.tipKey = (path) => {
    const p = String(path || '').replace(/\.\d+(?=\.|$)/g, '.*');
    const rules = [
      [/^(messages\.\*\.fields|protocols\.\*\.header)\.\*\.bits\.\*\./, 'bit.'],
      [/^(messages\.\*\.fields|protocols\.\*\.header)\.\*\.conversions\.\*\./, 'conv.'],
      [/^(messages\.\*\.fields|protocols\.\*\.header)\.\*\.crcParams\./, 'crc.'],
      [/^protocols\.\*\.checksum\.crc\./, 'crc.'],
      [/^(messages\.\*\.fields|protocols\.\*\.header)\.\*\./, 'field.'],
      [/^messages\.\*\.computed\.\*\./, 'computed.'],
      [/^messages\.\*\./, 'message.'],
      [/^protocols\.\*\./, 'protocol.'],
      [/^enums\.\*\.values\.\*\./, 'enum.values.'],
      [/^enums\.\*\./, 'enum.'],
      [/^customTypes\.\*\./, 'customType.'],
      [/^customChecksums\.\*\./, 'customChecksum.'],
      [/^customFramers\.\*\./, 'customFramer.'],
    ];
    for (const [re, rep] of rules) if (re.test(p)) return p.replace(re, rep);
    return p;
  };

  /** Tip for an element of the editor, or null. Returns { title, key, text, value }. */
  G.tipFor = (el) => {
    const control = el.matches('[data-path]') && !el.matches('button') ? el : el.querySelector && el.querySelector('input[data-path], select[data-path], textarea[data-path]');
    if (control && (el === control || el.matches('label, .field-label'))) {
      const key = G.tipKey(control.dataset.path);
      const tip = T[key];
      if (!tip) return null;
      let value = null, options = null;
      if (control.tagName === 'SELECT' && tip.v) {
        const opts = Array.from(control.options);
        const described = opts.filter((o) => tip.v[o.value] != null);
        if (opts.length <= 6 && described.length === opts.length) {
          // short lists: explain every choice, current one highlighted
          options = opts.map((o) => ({ label: o.textContent, text: tip.v[o.value], on: o.selected }));
        } else {
          const v = control.value;
          const label = control.selectedOptions[0] ? control.selectedOptions[0].textContent : v;
          const desc = tip.v[v] ?? (key === 'field.type' ? 'A type defined in this schema (enum, message or custom type).' : null);
          if (desc) value = { label, text: desc };
        }
      }
      return { title: tip.t, key: tip.k, text: tip.d, value, options };
    }
    const btn = el.closest('[data-action]');
    if (btn) {
      const a = btn.dataset.action;
      if (a === 'select' && btn.classList.contains('chip')) return { title: btn.textContent.trim(), text: CHIPS[btn.dataset.kind] || '' };
      if (a === 'addField' && btn.dataset.preset === 'bits') return { title: '+ Bits group', text: 'Add a group of bit fields packed into whole bytes.' };
      if (a === 'addField' && btn.dataset.preset === 'pad') return { title: '+ Padding', text: 'Add reserved bytes / skipped tokens.' };
      if (a === 'addBit' && btn.dataset.pad) return { title: btn.textContent.trim(), text: 'Add unused padding bits to the group.' };
      const label = btn.dataset.tipText || btn.textContent.trim();
      if (ACTIONS[a]) return { title: label || a, text: ACTIONS[a] };
      if (btn.dataset.tipText) return { title: btn.dataset.tipText, text: '' };
    }
    return null;
  };

  G.TIPS = T;
})(window.GMP);
