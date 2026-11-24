# GMP schema reference (schema_version 1)

A schema is one YAML file. `gmpc` turns it into one generated header `<schema>.gmp.hpp`.

```yaml
schema_version: 1          # required value: 1
namespace: acme::telemetry # C++ namespace of the generated code (optional)
defaults: { ... }          # defaults applied to messages / protocols
enums: { ... }
custom: { types: {...}, checksums: {...}, framers: {...} }
messages: { ... }
protocols: { ... }
```

Unknown keys are errors, so a typo like `feilds` is caught. Diagnostics report `file:line:col`.

---

## defaults

| key         | values                       | default      |
|-------------|------------------------------|--------------|
| `endian`    | `big`, `little`              | `big`        |
| `bit_order` | `msb_first`, `lsb_first`     | `msb_first`  |
| `format`    | `binary`, `text`             | `binary`     |
| `separator` | one character (text fields)  | `,`          |
| `access`    | `view`, `struct`, `both` or a list | `both` |
| `strict_enums` | `true`, `false` | `false` |

## enums

```yaml
enums:
  Mode:
    type: u8                       # underlying integer type (default u8)
    values: { idle: 0, run: 1, fault: 2 }
    doc: Operating mode
```

Generates `enum class Mode : std::uint8_t` and `constexpr std::string_view to_string(Mode)`.
Values that are not listed still decode; the enum just holds the raw value (check with the generated `is_known(Mode)`).
With `strict: true` on the enum (or `defaults: { strict_enums: true }`), decoding or encoding an undeclared value fails with
`Error::UnknownEnum` instead; `strict: false` opts one enum out of the default.

---

## messages

```yaml
messages:
  Status:
    type_id: 1           # value of the protocol's message_type field/token (integer, enum value name, or a string for text protocols)
    format: binary       # or text
    endian: big          # default for this message's fields
    bit_order: msb_first
    separator: ","       # text messages
    access: [view, struct]
    doc: Periodic status.
    fields: [ ... ]
    computed:            # binary: values derived from fields; accessor serial(), struct setter set_serial()
      serial: { type: u32, value: "(hi << 16) | lo" }   # setter is automatic for shift/or packing;
                                                        # otherwise add set: { field: "expr over value" }
```

### Field types

| type | binary wire format | struct member | view accessor |
|------|--------------------|---------------|---------------|
| `u8 u16 u32 u64 i8 i16 i32 i64` | fixed width integer | `std::uintN_t` / `std::intN_t` | value |
| `f32 f64` | IEEE-754 | `float` / `double` | value |
| `bool` | 1 byte, non-zero = true | `bool` | value |
| `char` | 1 byte | `char` | value |
| *EnumName* | underlying integer | enum | enum |
| `bits` | group of bit fields packed into whole bytes | `<Msg>_<field>` struct | that struct |
| `bytes` | raw bytes, see *lengths* | `std::array` / `gmsg::static_vector` | `gmsg::bytes_view` |
| `string` | characters, see *lengths* | `gmsg::static_string<N>` | `std::string_view` |
| `array` | repeated `of:` element | `std::array` / `gmsg::static_vector` | `ScalarArrayView` / `FixedArrayView` / `SeqArrayView` |
| *MessageName* | nested message | that struct | that view |
| *custom type* | user codec | user type | `gmsg::Result<T>` |
| `{ pad: N }` | N zero bytes (text: N ignored tokens) | none | none |

The same type names are used in text messages, which parse them from tokens instead.

### Field keys

| key | applies to | meaning |
|-----|-----------|---------|
| `name` | all | identifier. Names that clash with C++ keywords or generated members get a `_` suffix (e.g. `data` becomes `data_()`) |
| `type` | all | see above |
| `endian` | numeric | overrides the message default |
| `const` | integer / enum | magic value. It is checked on decode (`Error::BadConst`), written automatically on encode, and has no struct member |
| `scale`, `offset` | numeric | engineering value = raw * scale + offset. Generates `<name>_scaled()` and (struct) `set_<name>_scaled()` |
| `conversions` | numeric | extra named conversions `{ name: { scale, offset, unit } }`. Each generates `<name>_<conversion>()` and (struct) `set_<name>_<conversion>()` |
| `min_value`, `max_value` | numeric (also in `of`) | inclusive limits on the scaled value (`raw * scale + offset`), or the value when there is no scale/offset. Decode (struct and view) and encode fail with `Error::ValueOutOfLimits`; NaN fails |
| `min_raw`, `max_raw` | numeric (also in `of`) | inclusive limits on the raw wire value (integers compare exactly); combinable with `min_value`/`max_value` |
| `crc` | binary scalar / enum | checksum of the value bytes, stored right after them (`crc: crc8_nrsc5`, a custom checksum, or `{ crc: {...} }`). Verified on decode (`BadChecksum`), written on encode |
| `unit`, `doc` | all | documentation, copied into comments |
| `length` / `size` | `bytes`, `string`, `pad` | number, expression, or `remaining` |
| `count` | `array` | number, expression, or `remaining` |
| `terminator` | `bytes`, `string` | delimiter byte, e.g. `0` for C strings |
| `max` | variable length | capacity of the generated struct container (required when the message has a struct). Not a value limit: an error on any other field, and on a fixed size it does not match |
| `padding` | fixed `string` | pad character, stripped on decode and filled on encode (default `\0`) |
| `of` | `array` | element type name, or a mapping like `{ type: u16, endian: little, max_raw: 1000 }` |
| `when` | binary | expression; the field is present only when it evaluates to true (`std::optional`) |
| `optional` | text | an empty token is allowed (`std::optional`) |
| `fields` | `bits` | list of bit fields (see below) |
| `bit_order` | `bits` | overrides the message default |
| `base`, `width`, `precision` | text numbers | number base (10/16), zero-padded width, decimal places on encode |

### Lengths and counts

- `length: 8` gives a fixed size.
- `length: len_field` or `length: "n * 2 + 1"` is an expression over earlier fields.
  If the expression is just a field name, that field is **derived**. On encode it is written from the
  container's size, so you never set it by hand. On decode it is read as normal.
- `length: remaining` consumes the rest of the message. Only allowed on the last field.
- `terminator: 0` reads up to and including the terminator byte.

### Bit fields

```yaml
- name: flags
  type: bits
  bit_order: lsb_first          # optional
  fields:
    - { name: armed, bits: 1 }                  # 1 bit defaults to bool
    - { name: mode,  bits: 3, enum: Mode }
    - { name: level, bits: 4, type: i8 }        # signed values are sign-extended
    - { name: rpm,   bits: 14, type: u16, scale: 0.5 }
    - { name: ver,   bits: 4, const: 1 }        # validated, no member
    - { pad: 4 }
```

Numeric bit fields also take `min_value` / `max_value` / `min_raw` / `max_raw` (a raw limit must fit the bit width).

Value limits are rejected by gmpc on enums (use `strict`), bools, chars and `const` fields, when a raw limit for an
integer type is not an integer or is outside the type's range, and when a minimum is greater than its maximum.

The group must add up to a whole number of bytes. Bit offset 0 is the first bit of the group:
- `msb_first`: the MSB of byte 0, with values read most significant bit first (network / Motorola order).
- `lsb_first`: the LSB of byte 0, with values read least significant bit first (Intel / CAN DBC `@1`).

### Expressions

Used by `when`, `length` and `count`. They support integer literals (`10`, `0x1F`, `0b101`), references to earlier
non-conditional fields (`count`, `flags.mode`), enum constants (`Mode.fault`), and the operators
`! - ~ * / % + - << >> & ^ | == != < <= > >= && ||` with C precedence.

---

## protocols

Messages and enums can't be named `Header`, `HeaderView`, `AnyView`, `AnyMessage`, `Frame`, `Stream`, `StreamWith`,
`Callbacks`, `CallbackStream` or `Received` when the schema has a protocol, because every generated protocol struct declares
these names.

A protocol ties messages to a wire framing:

```
| prefix (sync/start) | header | body (message) | checksum | suffix (end) |
```

```yaml
protocols:
  BinaryLink:
    format: binary                # or text
    endian: big                   # header / checksum default
    header:                       # binary only; fixed-size fields
      - { name: msg_id, type: u8 }
      - { name: len, type: u16 }
    framing: { ... }
    checksum: { ... }
    message_type: { field: msg_id }   # binary: header field. text: { token: 0 }
    messages: [Status, Command]
    exact_size: false             # true: data after the message in a frame fails with BadLength
```

- Data after the message (a body longer than the message, extra text tokens) is ignored by default and reported:
  view `extra_bytes()` / `extra_tokens()`, `decode(data, extra)`, `Received::extra`, `stats().frames_with_extra`.
  `exact_size: true` rejects it (`BadLength`); not for `fixed` framing (it pads).
- `length_prefixed` (length counting unescaped bytes): the stream rejects a length the frame's message type cannot
  have as soon as the header arrives (`BadLength`): below the message's minimum size, or above its maximum with
  `exact_size`.
- Streams recover on an idle link with `resync()` (give up an incomplete head frame), `flush()` (drop everything) and
  `set_idle_timeout(t)` + `tick(now)` (resync after `t` without new bytes; `now` is any monotonic unsigned time).
- Interrupt input: the ISR pushes into a `gmsg::ByteRing<N>` (C `gmsg_ring`), the main loop calls `feed(ring)`. Input a
  full ring dropped is reported as `Overrun` (the frame in progress is dropped), counted in `stats().overruns`.
- Endpoints (`P::make_endpoint(source, sink[, clock])`; C `<p>_endpoint`, JS `P.endpoint({ write })`, Python
  `P.endpoint(source, sink)` / `P.async_endpoint(reader, writer)`): the stream joined to a byte source, a byte sink and
  a clock. `service()` reads and delivers; `send(msg)` encodes and writes (send policy `whole_frame`: all or nothing,
  `BufferFull`; `retry`: until written or `Timeout`); `wait_for<M>(timeout, match)` returns the next matching `M`
  decoded (others go to the listeners) or `Timeout`; `transact<M>(request, timeout, match)` delivers what arrived,
  sends, then waits; `request(pending, req, timeout)` / `expect(pending, timeout)` do the same without blocking
  (a caller-owned `gmsg::Pending<M>` / C `<p>_request` completed by `service()`; JS / Python async: Promises / awaits).
  Sources / sinks: `ByteRing`, memory, `FILE*`, callables, a circular DMA buffer
  (`dma_source`, laps reported as `Overrun`). Endpoints exist for protocols with a stream whose messages all have
  structs. `Endpoint` is a name generated inside every protocol (C++), so messages and enums cannot use it.

### framing

| `type` | keys | description |
|--------|------|-------------|
| `none` | | The whole buffer is one frame (datagrams, CAN payloads). No stream parser. |
| `fixed` | `sync`, `size`, `end`, `escape` | Fixed-size frames, optionally starting with a sync pattern and ending with an `end` pattern (never escaped). Without `size`, all messages must share one fixed size. Shorter messages are zero padded. |
| `length_prefixed` | `sync`, `length: { field, covers, adjust, counts }`, `end`, `escape` | The size comes from a header field. `covers`: `body` (default), `body_checksum` (body and checksum), `header_body` or `all` (prefix through checksum). Covered length = value + `adjust`. |
| `delimited` | `start`, `end`, `escape` | Bounded by delimiters (`start` is optional). Identical start and end bytes (HDLC `0x7E`) may share a flag between frames. |
| `custom` | `framer` | User type from `custom.framers` providing `static gmsg::FrameScan scan(gmsg::bytes_view)`. |

Shared keys:
- `max_frame`: largest frame and the stream buffer size. It is computed when every message is bounded, and required otherwise.
- `sync` / `start` / `end`: a list of bytes `[0xAA, 0x55]`, a string `"$"`, or an integer `0xA5A5` (big endian).
- `escape` (`delimited`, `fixed` and `length_prefixed`): byte stuffing, see below.

#### escape (reserved bytes)

Reserved bytes never appear between the sync/start pattern and the end of the frame. Each one is sent as the escape
`char` followed by a code byte:

```yaml
escape:
  char: 0x81                 # escape byte; always reserved
  code: index                # xor (default) | index | map
  reserved: [0x80, 0x81]     # optional; default = sync/start bytes, end bytes, then char
  base: 0x00                 # index only: code of the first reserved byte
  xor: 0x20                  # xor only (default 0x20)
  map: { 0xC0: 0xDC, 0xDB: 0xDD }   # map only: raw -> code; the keys are the reserved bytes
```

| `code` | reserved byte `b` is sent as | example |
|--------|------------------------------|---------|
| `xor` | `char, b ^ xor` | HDLC/PPP: `{ char: 0x7D, xor: 0x20 }` |
| `index` | `char, base + position of b in reserved` | `{ char: 0x81, code: index, reserved: [0x80, 0x81] }`: `0x80` -> `81 00`, `0x81` -> `81 01` |
| `map` | `char, map[b]` | SLIP: `{ char: 0xDB, map: { 0xC0: 0xDC, 0xDB: 0xDD } }` |

- Up to 16 reserved bytes (the escape char included).
- With `index` or `map`, the escape char must be in the reserved list / map.
- No code may itself be a reserved byte, and codes must be unique. With `index`, use `base` to move the codes if `0x00` is
  reserved.
- `delimited`: the first `end` byte must be reserved.
- `fixed` / `length_prefixed`: a sync byte that is not reserved only gives a warning. When it is reserved, a sync byte inside
  a frame means the frame was cut short, and the parser restarts there at once.
- Sizes count unescaped bytes: the `fixed` `size` and the `length` field value. `max_frame` (wire bytes) is doubled
  automatically.
- `length: { field: len, covers: body, counts: wire }` makes the length the body size on the wire (after escaping).
  `covers: body_checksum` also counts the escaped checksum, which must then have `checksum.covers: body`.
  Other coverages are rejected: a length that counts its own escaped bytes, or a checksum over it, cannot always be
  encoded.
- The checksum is computed over the unescaped bytes. Use `checksum.over: wire` for protocols that compute it over the
  escaped bytes. The checksum itself is escaped either way.
- Decoding unescapes in place, so `parse` / `open` / `decode` take a mutable buffer.
- The checksum is verified before the frame is unescaped, and a frame that fails later is escaped again, so after an
  error the buffer (and the stream's resync) sees the bytes as received.

### checksum

```yaml
checksum:
  algo: crc16_ccitt      # built-in name, a custom.checksums name, or { crc: {...} }
  covers: header_body    # binary: header_body (default), body, all (prefix..body)
  over: unescaped        # with framing.escape: unescaped (default) or wire (the escaped bytes)
  endian: big            # byte order of the stored checksum
  separator: "*"         # text: checksum follows this character as hex digits
  required: true         # text: reject frames without a checksum
```

Built-in algorithms (all `constexpr`, table driven):

- CRCs: `crc8`, `crc8_maxim`, `crc8_autosar`, `crc8_nrsc5` (Sensirion), `crc15_can`, `crc16_ccitt` (CCITT-FALSE), `crc16_xmodem`,
  `crc16_kermit`, `crc16_x25`, `crc16_modbus`, `crc16_arc`, `crc24_openpgp`, `crc32`, `crc32c`, `crc32_mpeg2`, `crc64_ecma`
- Sums: `xor8` (NMEA), `sum8`, `lrc8`, `sum16`, `fletcher16`, `fletcher8_ubx`

Any other CRC can be defined inline:

```yaml
algo: { crc: { width: 8, poly: 0x1D, init: 0xFF, refin: false, refout: false, xorout: 0xFF } }
```

For text protocols the checksum covers everything between the start delimiter and the separator, and is written as
uppercase hex (NMEA style).

---

## custom extensions

Language-specific settings live in a block named after the backend (`cpp:`, `c:`, `js:`, `python:`).

```yaml
custom:
  types:
    bcd_time:
      wire_size: 3            # binary: fixed size if known (else max_wire_size: N for bounds)
      cpp:
        header: "my_codecs.hpp"
        type: "my::Time"
        decode: "my::decode_bcd_time"
        encode: "my::encode_bcd_time"
  checksums:
    add16:
      size: 2
      cpp: { header: "my_codecs.hpp", function: "my::add16" }
  framers:
    len_byte:
      cpp: { header: "my_codecs.hpp", type: "my::LengthByteFramer" }
```

Required C++ signatures. The generated code checks them with `static_assert`:

| extension | signature |
|-----------|-----------|
| binary decoder | `gmsg::Result<T> decode(gmsg::ByteReader&)` |
| binary encoder | `gmsg::VoidResult encode(gmsg::ByteWriter&, const T&)` |
| text decoder | `gmsg::Result<T> decode(std::string_view token)` |
| text encoder | `gmsg::VoidResult encode(gmsg::text::TextWriter&, const T&)`. Write the token content with `w.raw(...)`; the separator is already handled. |
| checksum | `<unsigned integer> fn(gmsg::bytes_view covered)` |
| framer | `struct F { static gmsg::FrameScan scan(gmsg::bytes_view); };` |
| printing (optional) | `void gmsg_print(gmsg::Printer&, const T&)` next to `T`, used by `gmsg::print`; without it the value prints as `<?>` |

A `FrameScan` is one of three results:
- `need_more(hint)`
- `frame(size, consume, consume_on_error)`
- `skip(n, error)`

`consume_on_error` controls resynchronisation after a corrupt frame. Use 1 to rescan from the next byte, or `size` to
drop the whole frame.

For the C backend (`--lang c`) add a `c:` block with C names. It is needed only for the custom items that the C
output uses, so a schema written for C++ still generates C until it uses them:

```yaml
    bcd_time:
      wire_size: 3
      c: { header: "my_codecs.h", type: "my_time", decode: "my_time_decode", encode: "my_time_encode", print: "my_time_print" }
    add16: { size: 2, c: { header: "my_codecs.h", function: "my_add16" } }
    len_byte: { c: { header: "my_codecs.h", scan: "my_len_byte_scan" } }
```

| extension (C) | signature |
|---------------|-----------|
| binary decoder | `gmsg_error decode(gmsg_reader* r, T* out)` |
| binary encoder | `gmsg_error encode(gmsg_writer* w, const T* v)` |
| text decoder | `gmsg_error decode(const char* s, size_t n, T* out)` (the token, not NUL terminated) |
| text encoder | `gmsg_error encode(gmsg_text_writer* w, const T* v)`: write the token content with `gmsg_text_raw(w, s, n)`; the separator is already written |
| checksum | `<uint8_t / uint16_t / uint32_t / uint64_t> fn(const uint8_t* data, size_t n)` |
| framer | `gmsg_frame_scan scan(const uint8_t* buf, size_t n)` (key `scan`, not `type`) |
| printing (optional) | `void print(gmsg_printer* p, const T* v)` (key `print`), used by the generated `_print` functions; write with `gmsg_print_text(p, s, n)`. Without it a value whose C type is a standard scalar (`double`, `uint16_t`, ...) prints as that number, anything else as `<?>` |

The generated `.c` file checks these with `_Static_assert` + `_Generic`. On a view, a custom field's accessor decodes
on demand: `gmsg_error <view>_<field>(const view*, T* out)`.

For the JavaScript backend (`--lang js`) add a `js:` block: the module exporting the functions (required as written;
in a browser page looked up in the global `gmsgExtensions`) and their names. Only the items the output uses need one:

```yaml
    bcd_time: { wire_size: 3, js: { module: "./my_codecs.js", decode: decodeBcdTime, encode: encodeBcdTime, print: printTime } }
    add16: { size: 2, js: { module: "./my_codecs.js", function: add16 } }
    len_byte: { js: { module: "./my_codecs.js", scan: lenByteScan } }
```

| extension (JavaScript) | signature |
|------------------------|-----------|
| binary decoder / encoder | `decode(reader: gmsg.Reader) → value`, `encode(writer: gmsg.Writer, value)`; throw `gmsg.GmpError` on bad input |
| text decoder / encoder | `decode(token: string) → value`, `encode(writer: gmsg.text.TextWriter, value)` (write with `writer.raw(text)`) |
| checksum | `fn(covered: Uint8Array) → number or BigInt` |
| framer | `scan(bytes: Uint8Array) → gmsg.FrameScan.needMore(hint) / .frame(size, consume, consumeOnError) / .skip(n, error)` |
| printing (optional) | `print(printer: gmsg.Printer, value)`; without it numbers, BigInts and strings print as such, anything else as `<?>` |

For the Python backend (`--lang python`) add a `python:` block: the module to import (a leading `.` imports it relative
to the generated module's package) and the function names:

```yaml
    bcd_time: { wire_size: 3, python: { module: my_codecs, decode: decode_bcd_time, encode: encode_bcd_time, print: print_time } }
    add16: { size: 2, python: { module: my_codecs, function: add16 } }
    len_byte: { python: { module: my_codecs, scan: len_byte_scan } }
```

| extension (Python) | signature |
|--------------------|-----------|
| binary decoder / encoder | `decode(reader: gmsg.Reader) -> value`, `encode(writer: gmsg.Writer, value)`; raise `gmsg.GmpError` (`gmsg.fail(Err.X)`) on bad input |
| text decoder / encoder | `decode(token: str) -> value`, `encode(writer: gmsg.text.TextWriter, value)` (write with `writer.raw(text)`) |
| checksum | `fn(covered: bytes-like) -> int` |
| framer | `scan(buf: bytes-like) -> gmsg.FrameScan.need_more(hint) / .frame(size, consume, consume_on_error) / .skip(n, error)` |
| printing (optional) | `print(printer: gmsg.Printer, value)` (write with `printer.raw_text(s)`); without it numbers and strings print as such, anything else as `<?>` |
