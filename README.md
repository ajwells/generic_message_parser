# generic_message_parser (GMP)

Describe a wire protocol in YAML and get efficient C++ decode/encode code. GMP is like protobuf, but the schema
describes **existing** protocols: binary or ASCII, any framing, any bit layout. It does not impose its own format.

- **Generator** `gmpc`: YAML → language-neutral IR → backend (C++ today, more can be added).
- **Runtime** `gmp::runtime`: header-only and standard-library only. No heap, no exceptions, no RTTI.
- **Two input modes**: whole buffer per message (`Proto::parse(buf)`) or streaming
  (`Proto::Stream<>::feed(chunk, handler)`) with resync on garbage.
- **Two access styles** per message:
  - lazy zero-copy **views** that decode fields straight from the bytes on access
  - eager **structs** with fixed-capacity storage

**Contents:**
- [Quick start](#quick-start)
- [How a schema fits together](#how-a-schema-fits-together)
- [Schema reference](#schema-reference)
  - [Top level](#top-level) · [defaults](#defaults) · [enums](#enums) · [messages](#messages) · [fields](#fields)
  - [bit fields](#bit-fields) · [lengths and counts](#lengths-and-counts) · [expressions](#expressions)
  - [text messages](#text-messages) · [protocols](#protocols) · [framing](#framing) · [checksum](#checksum)
  - [message_type](#message_type) · [custom extensions](#custom-extensions)
- [Generated C++ API](#generated-c-api) · [Streaming](#streaming) · [Errors](#errors)
- [Building](#building) · [CMake integration](#cmake-integration) · [gmpc command line](#gmpc-command-line)
- [Project layout](#project-layout) · [Adding a language backend](#adding-a-language-backend)

---

## Quick start

```yaml
# link.yaml
schema_version: 1
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
    messages: [Status]
```

```cmake
add_subdirectory(generic_message_parser)
gmp_generate(TARGET link_msgs SCHEMAS link.yaml)
target_link_libraries(app PRIVATE link_msgs)
```

```cpp
#include "link.gmp.hpp"

// encode
demo::Status s;
s.set_voltage_scaled(12.6);
s.samples = {1, 2, 3};                       // `count` is filled in automatically
std::array<std::uint8_t, demo::Link::max_frame> buf;
auto n = demo::Link::encode(s, buf);         // gmp::Result<std::size_t>

// whole buffer
if (auto msg = demo::Link::parse(gmp::bytes_view(buf.data(), *n)))
    std::get<demo::StatusView>(*msg).voltage_scaled();

// streaming
demo::Link::Stream<> stream;
stream.feed(chunk, gmp::overloaded{
    [](const demo::StatusView& v) { /* ... */ },
    [](const gmp::FrameError& e) { /* garbage / bad checksum, optional */ },
});
```

Working programs are in [examples/](examples): `binary_telemetry`, `nmea_text`, `bitpacked_can` and `custom_codec`.

**Prefer a visual editor?** Open [`tools/schema-studio/index.html`](tools/schema-studio) in a browser. You build
schemas with forms on the left and see a size-proportional layout of every message and frame on the right, then
download the YAML. Its **Decode tester** tab decodes pasted raw frames against the schema, so you can check them before
generating code.

---

## How a schema fits together

- **Messages** describe the payload: an ordered list of fields. A message can be used on its own
  (`Status::decode(bytes)`, `StatusView::from(bytes)`) without any protocol.
- **Protocols** wrap messages in a frame on the wire:

  ```
  | prefix (sync / start) | header | body = one message | checksum | suffix (end) |
  ```

  A protocol says:
  - how frames are delimited (**framing**)
  - how they are validated (**checksum**)
  - how the message type is chosen (**message_type**)

- **Enums** and **custom** types/checksums/framers are shared building blocks.

Schemas are strict. Unknown keys are errors, so a typo like `feilds` is caught. Every diagnostic carries
`file:line:col`.

---

## Schema reference

### Top level

| key | required | description |
|-----|----------|-------------|
| `schema_version` | no (default `1`) | Schema language version. Only `1` exists. |
| `namespace` | no | C++ namespace of the generated code, e.g. `acme::telemetry`. |
| `defaults` | no | Defaults applied to every message and protocol (see below). |
| `enums` | no | Named enumerations usable as field types. |
| `custom` | no | User-supplied field codecs, checksums and framers. |
| `messages` | no | Message definitions, keyed by name. |
| `protocols` | no | Protocol definitions, keyed by name. |

All type-level names (messages, enums, custom types, protocols) share one namespace. They must be valid identifiers
and must not shadow built-in type names. Each message `M` also reserves `MView`, and each protocol `P` reserves
`PHeader`.

### defaults

| key | values | default | effect |
|-----|--------|---------|--------|
| `endian` | `big`, `little` | `big` | Byte order of multi-byte fields in messages, and of protocol headers and checksums. |
| `bit_order` | `msb_first`, `lsb_first` | `msb_first` | Bit numbering inside `bits` groups (see [bit fields](#bit-fields)). |
| `format` | `binary`, `text` | `binary` | Whether messages and protocols are binary or token-separated text. |
| `separator` | one character | `,` | Field separator for text messages and protocols. |
| `access` | `view`, `struct`, `both`, or a list like `[view, struct]` | `both` | Which C++ access styles to generate (see [messages](#messages)). |

Every default can be overridden per message, per protocol, and (for `endian`/`bit_order`) per field.

### enums

```yaml
enums:
  Mode:
    type: u8                                  # underlying integer type, default u8
    values: { idle: 0, run: 1, fault: 2 }     # name -> integer (hex/binary literals allowed)
    doc: Operating mode                       # copied into the generated comment
```

- Generates `enum class Mode : std::uint8_t` and `constexpr std::string_view to_string(Mode)`. `to_string`
  returns an empty string for unlisted values.
- Values that are not listed still decode; the enum simply holds the raw number.
- Enum constants can be used in expressions as `Mode.fault`, and as `type_id` values when `message_type` reads an enum header
  field.

### messages

```yaml
messages:
  Status:
    type_id: 1
    format: binary
    endian: big
    bit_order: msb_first
    separator: ","
    access: [view, struct]
    doc: Periodic status report.
    fields: [ ... ]
```

| key | description |
|-----|-------------|
| `type_id` | Value a protocol's [message_type](#message_type) field or token must have for a frame to be this message. An unquoted number is an integer (`1`, `0x10`). Anything else is a string (`GPGGA`, or `"1"` quoted), which is either a text token or the name of an enum value. Only needed when the message is in a protocol with `message_type`. |
| `format` | `binary` or `text`. Text messages are parsed from separator-delimited tokens (see [text messages](#text-messages)). |
| `endian` | Default byte order for this message's fields. |
| `bit_order` | Default bit order for this message's `bits` groups. |
| `separator` | Text field separator. Must match the protocol's separator if used in a protocol. |
| `access` | `view` generates only the zero-copy `MView` class, `struct` generates only the eager `struct M`, and `both` (the default) generates both. Messages used by a protocol need `view` (message selection uses views). A nested message must support every access style its parent uses. |
| `doc` | Documentation comment for the generated types. |
| `fields` | Ordered list of fields. On the wire they appear in exactly this order. |

#### Views vs structs

- **View**:
  - `MView::from(bytes)` checks bounds, constants and nested structure once, then accessors read straight from the
    buffer.
  - Nothing is copied. The view references your buffer, so the buffer must outlive it.
  - Best when you read a few fields of each message, or on memory-constrained targets.
- **Struct**:
  - `M::decode(bytes)` copies every field into a plain value type.
  - Containers have fixed capacity (`std::array`, `gmp::static_vector`, `gmp::static_string`), so no heap is used.
  - Variable-length fields therefore need `max:`.
  - Structs are what you fill in to encode.

### fields

```yaml
fields:
  - { name: voltage, type: u16, scale: 0.01, unit: V }
  - { pad: 2 }
```

#### Field types

| `type` | binary wire format | struct member | view accessor returns |
|--------|--------------------|---------------|-----------------------|
| `u8` `u16` `u32` `u64` `i8` `i16` `i32` `i64` | fixed-width two's complement integer | `std::uintN_t` / `std::intN_t` | the value |
| `f32` `f64` | IEEE-754 float/double | `float` / `double` | the value |
| `bool` | 1 byte, non-zero = true | `bool` | the value |
| `char` | 1 byte | `char` | the value |
| *EnumName* | the enum's underlying integer | the enum | the enum |
| `bits` | group of bit fields packed into whole bytes | struct `<Message>_<field>` | that struct, unpacked |
| `bytes` | raw bytes; size from `length` / `terminator` | `std::array<uint8_t,N>` (fixed) or `gmp::static_vector<uint8_t,max>` | `gmp::bytes_view` |
| `string` | characters; size from `length` / `terminator` | `gmp::static_string<N or max>` | `std::string_view` |
| `array` | `count` repeated elements of type `of` | `std::array<T,N>` (fixed) or `gmp::static_vector<T,max>` | a lazy array view (see below) |
| *MessageName* | the nested message's encoding | that message's struct | that message's view |
| *custom type* | whatever the user codec reads/writes | the user type | `gmp::Result<T>` (decoded on access) |
| `pad` / `{ pad: N }` | N bytes: skipped on decode, zero on encode (text: N ignored tokens) | none | none |

Aliases: `uint8`…`uint64`, `int8`…`int64`, `float` (= f32), `double` (= f64).

Array views:
- `gmp::ScalarArrayView<T>` for scalars and enums (random access)
- `gmp::FixedArrayView<V, N>` for fixed-size messages (random access)
- `gmp::SeqArrayView<V>` for variable-size messages (forward iteration)

#### Field keys

| key | applies to | description |
|-----|-----------|-------------|
| `name` | all except `pad` | Field identifier. Names that clash with C++ keywords or generated members (`data`, `read`, `encode`, `from`, …) get a trailing `_`, e.g. field `data` gives accessor `data_()`. |
| `type` | all | See the table above. |
| `endian` | multi-byte numerics | Overrides the message default for this field only. |
| `const` | integer / enum / bool / char | Fixed (magic) value. Decode fails with `BadConst` on mismatch. Encode writes it automatically. There is no struct member or view accessor; the struct gets a `static constexpr <name>_value`. |
| `scale`, `offset` | numeric scalars | Engineering conversion: `value = raw * scale + offset`. The raw value is stored and transmitted. Adds `<name>_scaled()` to struct and view, and `set_<name>_scaled(double)` to the struct (rounds to nearest for integers). |
| `unit` | all | Documentation only; appears in generated comments. |
| `doc` | all | Documentation comment. |
| `length` (alias `size`) | `bytes`, `string`, `pad` | Number of bytes: a number, an expression, or `remaining`. See [lengths and counts](#lengths-and-counts). |
| `count` | `array` | Number of elements: a number, an expression, or `remaining`. |
| `terminator` | `bytes`, `string` | End marker byte (e.g. `0` for C strings). Read up to and including it; written after the data. Cannot be combined with `length`. |
| `max` | variable-length `bytes`/`string`/`array`; text `string` | Capacity of the struct container. Required when the message has a struct. Decode fails with `OutOfRange` if the wire data is longer. |
| `padding` | fixed-length `string` | Pad character: a character such as `" "`, or a byte value. Default `\0`. Stripped on decode; fills the remainder on encode. |
| `of` | `array` | Element type: a type name (`of: i16`) or a mapping (`of: { type: u16, endian: little }`). Elements may be scalars, enums, messages, or custom types (custom only with `access: struct`). Nested arrays are not supported; wrap the inner array in a message. |
| `when` | binary fields | Condition [expression](#expressions). The field is on the wire only when it is true, and becomes `std::optional` in struct and view. On encode it is written when the condition holds, and fails with `MissingField` if the optional is empty. |
| `optional` | text fields | An empty token is accepted and maps to `std::nullopt`. Without it, an empty token is a `MissingField` error. |
| `fields` | `bits` | The bit fields of the group. |
| `bit_order` | `bits` | Overrides the message default for this group. |
| `base` | text integers / enums | Number base for parsing and writing (`10` default, `16` for hex). |
| `width` | text numbers | Minimum digits when encoding, zero padded (`width: 2` gives `08`). Ignored on decode. |
| `precision` | text floats | Decimal places when encoding (`precision: 3` gives `4807.038`). The default `-1` gives the shortest exact representation. |

### bit fields

```yaml
- name: flags
  type: bits
  bit_order: lsb_first
  fields:
    - { name: armed, bits: 1 }                     # 1 bit defaults to bool
    - { name: mode,  bits: 3, enum: Mode }         # enum-typed
    - { name: level, bits: 4, type: i8 }           # signed, sign-extended on decode
    - { name: rpm,   bits: 14, type: u16, scale: 0.5 }
    - { name: ver,   bits: 4, const: 1 }           # validated on decode, written on encode
    - { pad: 4 }                                   # unused bits, written as 0
```

| key | description |
|-----|-------------|
| `name` | Member name inside the generated `<Message>_<field>` struct. |
| `bits` | Width, 1–64. |
| `type` | Storage type: `bool` or an integer type (`u8`…`u64`, `i8`…`i64`). Defaults to `bool` for 1 bit, otherwise the smallest unsigned type that fits. Signed types sign-extend. |
| `enum` | Store as this enum (alternative to `type`). |
| `scale`, `offset` | Adds `<name>_scaled()` to the bits struct. |
| `const` | Required value; checked on decode, written on encode, no member. |
| `{ pad: N }` | N unused bits. |

- The group must total a whole number of bytes. The error message suggests the `pad` to add.
- Bit 0 is the first bit of the group's first byte:
  - **`msb_first`** (network / Motorola order): bit 0 is the MSB of byte 0, and multi-bit values are read
    most-significant bit first.
  - **`lsb_first`** (Intel order, CAN DBC `@1`): bit 0 is the LSB of byte 0, and values are read least-significant bit
    first, continuing into the next byte.
- Bit-field members can be referenced in expressions as `flags.mode`.

### lengths and counts

`length`, `size` and `count` accept:

| form | example | meaning |
|------|---------|---------|
| number | `length: 8` | Fixed size. The field has a static size and offset, which keeps the message fixed-layout when every field is fixed. |
| field reference | `length: name_len` | Taken from an earlier field. The referenced field becomes **derived**: on encode it is written from the container's actual size, so you never set it by hand. On decode it is read and used. |
| expression | `count: "n * 2 + 1"` | Computed from earlier fields. On encode the container's size must equal the expression, otherwise `BadLength`. |
| `remaining` | `length: remaining` | Everything to the end of the message (or the frame body). Only allowed on the last field. For arrays of fixed-size elements, the remaining size must be a multiple of the element size. |

`terminator: <byte>` is the alternative for `bytes`/`string`. Data runs until the terminator byte, which is consumed.

### expressions

Used by `when`, and by `length`/`count` when they are not plain numbers.

- **Literals**: `10`, `0x1F`, `0b101`.
- **Field references**: `count`, `flags.mode` (a member of a bits group). Only **earlier** fields of the same message,
  only integer/bool/char/enum fields (no floats), and never conditional (`when`) fields.
- **Enum constants**: `Mode.fault`.
- **Operators** (C precedence): `!` `-` `~`, `*` `/` `%`, `+` `-`, `<<` `>>`, `&`, `^`, `|`,
  `==` `!=` `<` `<=` `>` `>=`, `&&`, `||`, and parentheses.

Values are evaluated as 64-bit signed integers. When encoding, references to derived length/count fields use the
container's actual size, so conditions like `when: "n > 2"` stay consistent with what is written.

### text messages

Text messages (`format: text`) are records of tokens separated by `separator`, like NMEA 0183 or CSV:

```yaml
messages:
  GGA:
    format: text
    type_id: GPGGA
    fields:
      - { name: utc_time, type: f64, precision: 2 }
      - { name: latitude, type: f64, optional: true }
      - { name: lat_hemi, type: char, optional: true }
      - { name: quality, type: FixQuality }        # enum parsed from its integer value
      - { name: satellites, type: u8, width: 2 }
      - { name: text, type: string, max: 61 }
      - { pad: 4 }                                 # ignore the next 4 tokens
```

- Each field consumes exactly one token, except `pad: N`, which skips N tokens.
- Numeric types are parsed from text: `"32"` becomes `32`, and `base: 16` reads hex. A `bool` is any non-zero integer.
  A `char` must be exactly one character.
- `string` takes the whole token. No `length` is allowed, and `max` is required for structs.
- Missing trailing tokens are treated as empty. Extra tokens are ignored.
- View accessors parse on demand and return `gmp::Result<T>`, or `gmp::Result<std::optional<T>>` for `optional`
  fields. String accessors return `std::string_view` directly.
- Not supported in text messages: `bits`, `bytes`, `array`, nested messages and `when`.

### protocols

```yaml
protocols:
  BinaryLink:
    format: binary
    endian: big
    separator: ","
    doc: Link layer for the telemetry radio.
    header:
      - { name: msg_id, type: u8 }
      - { name: len, type: u16 }
    framing: { ... }
    checksum: { ... }
    message_type: { field: msg_id }
    messages: [Status, Command]
```

| key | description |
|-----|-------------|
| `format` | `binary` or `text`. Every listed message must have the same format. |
| `endian` | Byte order of header fields and the checksum. Message fields keep their own defaults. |
| `separator` | Text protocols: the separator after the id token. Must match the messages' separator. |
| `header` | Binary only. Fixed-size fields between the prefix and the body, using the same syntax as message fields (no `when`, no variable sizes). Typically holds the message id, a length and flags. Generated as a message `<Protocol>Header` / `<Protocol>HeaderView`. `const` header fields (protocol version, address) are validated and written automatically. |
| `framing` | How frames are found in the byte stream (below). Default `none`. |
| `checksum` | Optional integrity check (below). |
| `message_type` | How the message type is chosen (below). |
| `messages` | Names of the messages this protocol carries. |

### framing

| `type` | keys | how frames are found |
|--------|------|----------------------|
| `none` | | The whole buffer is one frame. Use it for datagrams, CAN payloads, or anything already split by a lower layer. Whole-buffer mode only; there is no stream parser. |
| `fixed` | `sync`, `size` | Every frame is `size` bytes, optionally starting with a `sync` pattern. Without `size`, every message must have the same fixed size and the frame size is derived. Bodies shorter than the frame are zero padded on encode. Without `sync` the parser cannot resynchronise, so a bad frame is dropped whole. |
| `length_prefixed` | `sync`, `length` | A header field gives the size. `length.field` names the header field (unsigned integer). `length.covers` says what the value measures: `body` (default), `header_body`, or `all` (prefix through checksum). `length.adjust` is added to the value: covered size = value + adjust. On encode the field is filled in automatically. |
| `delimited` | `start`, `end`, `escape` | Frames bounded by delimiters. `start` is optional (without it, frames are separated by `end` only, like lines); `end` is required. If a new `start` appears before `end`, the partial frame is dropped (`BadDelimiter`). When `start` and `end` are the same (HDLC `0x7E`), one flag may close a frame and open the next. |
| `custom` | `framer` | A user framer from `custom.framers` decides frame boundaries. |

Shared framing keys:

| key | description |
|-----|-------------|
| `sync` / `start` | Bytes at the start of every frame. Synonyms: use `sync` for binary sync words and `start` for delimiters. |
| `end` | Bytes ending every frame (`delimited`). |
| `max_frame` | Largest frame in bytes. It bounds the length check and sizes the stream buffer. Computed automatically for binary protocols whose messages are all bounded (doubled when escaping is on). Required for text protocols and unbounded messages. |
| `escape` | Byte stuffing for `delimited` frames, so delimiter bytes can appear in the payload. `{ char: 0x7D, xor: 0x20 }` is HDLC/PPP style: the escape byte and every delimiter byte are sent as `char, byte ^ xor`. `{ char: 0xDB, map: { 0xC0: 0xDC, 0xDB: 0xDD } }` is SLIP style, with explicit replacement codes (up to 4 entries). Decoding unescapes in place, so `parse`/`open`/`decode` of escaped protocols take a mutable buffer. |

Byte patterns (`sync`, `start`, `end`) can be written three ways:
- a list `[0xAA, 0x55]`
- a quoted string `"$"` or `"\r\n"`, taken as its characters
- an unquoted integer `0xA5A5`, written big endian in the minimal number of bytes

Patterns are at most 8 bytes.

### checksum

```yaml
checksum:
  algo: crc16_ccitt
  covers: header_body
  endian: big
  separator: "*"
  required: true
```

| key | description |
|-----|-------------|
| `algo` | A built-in name, the name of a `custom.checksums` entry, or inline CRC parameters (below). |
| `covers` | Binary only. The bytes the checksum is computed over: `header_body` (default; becomes `body` when there is no header), `body`, or `all` (prefix + header + body). The checksum is stored right after the body. |
| `endian` | Byte order of the stored checksum. Defaults to the protocol's `endian`. |
| `separator` | Text only. The character before the hex checksum (default `*`, as in NMEA). |
| `required` | Text only. If `true` (default), sentences without a checksum are rejected. If `false`, a missing checksum is accepted and a present one is still verified. |

A mismatch fails with `BadChecksum`.

For text protocols, the checksum covers everything between the start delimiter and the separator. It is written as
uppercase hex, two digits per byte; either case is accepted on decode.

Built-in algorithms (all `constexpr` and table driven):

| name | description | name | description |
|------|-------------|------|-------------|
| `crc8` | CRC-8/SMBUS | `crc16_arc` | CRC-16/ARC |
| `crc8_maxim` | CRC-8/MAXIM (1-Wire) | `crc24_openpgp` | CRC-24/OPENPGP |
| `crc8_autosar` | CRC-8/AUTOSAR | `crc32` | CRC-32 (zip, Ethernet) |
| `crc15_can` | CRC-15/CAN | `crc32c` | CRC-32C (iSCSI) |
| `crc16_ccitt` | CRC-16/CCITT-FALSE | `crc32_mpeg2` | CRC-32/MPEG-2 |
| `crc16_xmodem` | CRC-16/XMODEM | `crc64_ecma` | CRC-64/ECMA-182 |
| `crc16_kermit` | CRC-16/KERMIT | `xor8` | XOR of all bytes (NMEA) |
| `crc16_x25` | CRC-16/X.25 (HDLC) | `sum8` / `sum16` | Modulo 2⁸ / 2¹⁶ sum |
| `crc16_modbus` | CRC-16/MODBUS | `lrc8` | Two's complement sum (Modbus ASCII, Intel HEX) |
| `fletcher16` | Fletcher-16 | `fletcher8_ubx` | u-blox UBX CK_A/CK_B |

Any other CRC can be defined inline using the Rocksoft model parameters:

```yaml
algo: { crc: { width: 8, poly: 0x1D, init: 0xFF, refin: false, refout: false, xorout: 0xFF } }
```

### message_type

`message_type` tells a protocol which message type a frame contains, after framing and checksum checks pass.

| form | use | behaviour |
|------|-----|-----------|
| `message_type: { field: msg_id }` | binary | Reads the named **header field** (integer or enum) and selects the message whose `type_id` equals it. Type ids may be integers, or enum value names when the field is an enum. On encode the field is set from the message being encoded. |
| `message_type: { token: 0 }` | text | The first token of the record (e.g. `GPGGA`) is compared with each message's `type_id`. The message's fields start **after** that token, and encode writes it automatically. Only token 0 is supported. |
| *(omitted)* | either | Allowed only when the protocol has exactly one message; every frame is that message. |

- `type_id` values must be unique within a protocol.
- A frame with an unrecognised type id fails with `UnknownType`. In streaming mode it is reported to your `FrameError`
  handler.
- `parse()` returns a `std::variant` of the possible views (`AnyView`). Stream handlers are called with whichever
  overload matches; message types the handler does not accept are ignored.

### custom extensions

Language-specific settings live in a block named after the backend (`cpp:`), so the same schema can later serve other
languages.

```yaml
custom:
  types:
    bcd_time:
      wire_size: 3              # binary: fixed size on the wire, if known
      max_wire_size: 3          # binary: upper bound if variable (helps compute max_frame)
      cpp:
        header: "my_codecs.hpp" # included by the generated header ("..." or <...>)
        type: "my::Time"        # C++ type of the decoded value
        decode: "my::decode_bcd_time"
        encode: "my::encode_bcd_time"   # required if a struct uses the type
  checksums:
    add16:
      size: 2                   # bytes on the wire
      cpp: { header: "my_codecs.hpp", function: "my::add16" }
  framers:
    len_byte:
      cpp: { header: "my_codecs.hpp", type: "my::LengthByteFramer" }
```

- A custom **type** is used like any other field type: `{ name: t, type: bcd_time }`. It also works in text messages,
  with the text signatures below.
- Without `wire_size`, the codec decides the size. The view's `from()` decodes the field once to find where the next
  field starts. With `wire_size`, the codec is given exactly that many bytes.

Required C++ signatures. The generated header checks them with `static_assert`, so a mismatch gives a readable error:

| extension | signature |
|-----------|-----------|
| binary decoder | `gmp::Result<T> decode(gmp::ByteReader&)` |
| binary encoder | `gmp::VoidResult encode(gmp::ByteWriter&, const T&)` |
| text decoder | `gmp::Result<T> decode(std::string_view token)` |
| text encoder | `gmp::VoidResult encode(gmp::text::TextWriter&, const T&)`. Write the token content with `w.raw(...)`; separators are handled for you. |
| checksum | `<unsigned integer> fn(gmp::bytes_view covered)` |
| framer | `struct F { static gmp::FrameScan scan(gmp::bytes_view); };` |

A framer's `scan` receives the unconsumed bytes at the current stream position and returns one of:
- `FrameScan::need_more(hint)`: not enough data yet.
- `FrameScan::frame(size, consume, consume_on_error)`: a frame of `size` bytes starts here. `consume` is how much to
  drop after a good frame. `consume_on_error` is how much to drop if validation fails: 1 to rescan from the next byte,
  `size` to drop the whole frame.
- `FrameScan::skip(n, error)`: drop `n` garbage bytes, reporting `error` (or `Error::None` to skip silently).

---

## Generated C++ API

`gmpc` writes one header per schema, `<schema>.gmp.hpp`, inside your `namespace`. It depends only on `<gmp/gmp.hpp>`
and the headers of your custom extensions.

### `struct M` (binary message)

| member | description |
|--------|-------------|
| fields | One member per field, default-initialised. Conditional fields are `std::optional`. `const` and `pad` fields have no member. |
| `min_wire_size`, `max_wire_size`, `wire_size` | Encoded size bounds. `max_wire_size` is present when bounded; `wire_size` when fixed. |
| `<name>_value` | `static constexpr` value of each `const` field. |
| `<name>_scaled()`, `set_<name>_scaled(double)` | Engineering-unit access for fields with `scale`/`offset`. |
| `static Result<M> decode(bytes_view)` | Decode from the start of a buffer. Trailing bytes are ignored. |
| `Result<size_t> encode(mut_bytes out) const` | Encode into `out`. Returns bytes written, or `BufferFull`. |
| `read(ByteReader&)` / `write(ByteWriter&)` | Lower-level building blocks (used for nesting). |
| `operator==` | Member-wise comparison. |

### `class MView` (binary message)

| member | description |
|--------|-------------|
| `static Result<MView> from(bytes_view)` | Validate bounds, constants, nested messages and variable layout once. |
| `static MView unchecked(bytes_view)` | Wrap data already validated by `from()`. |
| field accessors | `name()` reads the value from the buffer. Bits groups return the unpacked struct, `bytes` a `bytes_view`, `string` a `string_view`, arrays an array view, nested messages a view, custom types `Result<T>`, and conditional fields `std::optional<...>`. |
| `<name>_scaled()` | Engineering value. |
| `data()`, `wire_size()` | The exact bytes of this message. |
| `to_struct()` | Eager decode (when the struct is generated). |

Fixed-layout messages use compile-time offsets. Messages with variable fields compute their field offsets once in
`from()`, storing 4 bytes per field in the view.

### Text messages

- **`struct M`**: members (`optional` fields are `std::optional`), `token_count`, `separator`,
  `static decode(bytes_view)`, `static decode_text(std::string_view)`, `encode(mut_bytes)`, and `read_text`/`write_text`.
- **`class MView`**:
  - `from(std::string_view)` / `from(bytes_view)` tokenise once
  - field accessors parse on demand and return `Result<T>`
  - `token(i)` gives a raw token, `present_tokens()` the number of tokens found, `raw()` the whole record
  - `to_struct()`

### `struct P` (protocol)

| member | description |
|--------|-------------|
| `max_frame`, `prefix_size`, `header_size`, `trailer_size`, `suffix_size`, `prefix`, `suffix` | Frame layout constants. |
| `Header`, `HeaderView` | Header struct / view (binary protocols with a header). |
| `AnyView` | `std::variant` of all message views. |
| `AnyMessage` | `std::variant` of all message structs (when every message has a struct). |
| `static Result<AnyView> parse(buffer)` | Whole-buffer mode: find one frame at the start of `buffer`, validate it, select the message type. |
| `static Result<AnyMessage> decode(buffer)` | `parse` followed by `to_struct()`. |
| `static Result<size_t> encode(const M&, mut_bytes out)` | Build a complete frame: prefix, header (id and length filled in), body, padding, checksum, escaping, suffix. One overload per message. |
| `static Result<size_t> encode(Header, const M&, mut_bytes)` | Same, with explicit values for the other header fields. |
| `static Result<size_t> encode(const AnyMessage&, mut_bytes)` | Encode whichever message the variant holds. |
| `template <size_t Cap = max_frame> using Stream` | The stream parser for this protocol (see [Streaming](#streaming)). |
| `scan`, `open`, `visit_frame`, `select_message`, `process` | Building blocks. `scan` finds a frame boundary. `open` validates a frame and splits it into `Frame{header, body}` (binary) or `Frame{type_id, body}` (text, where `type_id` is the first token). `visit_frame`/`select_message` pick the message type. `process` is the hook used by the stream parser. |

---

## Streaming

```cpp
demo::Link::Stream<> parser;                    // fixed internal buffer of max_frame bytes
parser.feed(chunk, gmp::overloaded{
    [](const demo::Link::HeaderView& h, const demo::StatusView& v) { /* header + message */ },
    [](const demo::CommandView& v) { /* message only */ },
    [](const gmp::FrameError& e) { /* optional: e.error, e.data */ },
});
```

- **`feed(bytes, handler)`** accepts chunks of any size: one byte at a time, partial frames, or many frames at once.
  It returns the number of frames delivered.
- **`push(bytes)` / `next(handler)`** is the pull-style alternative. `push` buffers as much as fits; `next` processes
  at most one frame and returns `false` when it needs more input.
- **Handler overloads** can take `(HeaderView, MView)` or `(MView)`. Message types without an overload are ignored. A
  `const gmp::FrameError&` overload receives dropped data: garbage before a sync, bad checksums, unknown ids, oversize
  frames. Without that overload, errors are silently skipped.
- **Resynchronisation**:
  - After a bad frame the parser rescans from the next byte (length-prefixed and fixed framing with sync) or after the
    frame (delimited), so a corrupt frame does not cause the next good frame to be lost.
  - If the buffer fills without a complete frame, one byte is dropped (`BufferFull`) and scanning continues.
- **`stats()`** returns `frames`, `frame_errors`, `bytes_skipped` and `overflows`. `reset()` clears the buffer and
  stats; `buffered()` is the number of pending bytes.
- Views passed to the handler point into the parser's buffer and are valid only during the call. Copy what you need,
  or call `to_struct()`.

## Errors

Every fallible operation returns `gmp::Result<T>` (`std::expected<T, gmp::Error>`). `gmp::to_string(Error)` gives a
readable name.

| error | meaning |
|-------|---------|
| `Truncated` | Input ended before the message/frame was complete. |
| `BadSync` | Bytes before a sync word / start delimiter were skipped. |
| `BadChecksum` | Checksum mismatch, or a text checksum missing or malformed. |
| `BadLength` | Length field out of range, frame larger than `max_frame`, or container size inconsistent with a length expression on encode. |
| `BadDelimiter` | A new start delimiter appeared inside a frame. |
| `BadConst` | A `const` field or bit field did not match. |
| `OutOfRange` | Data exceeds a container's `max`, or a value does not fit. |
| `UnknownType` | No message has the frame's `type_id`. |
| `BufferFull` | Output buffer too small, or stream buffer full without a frame. |
| `ParseError` | A text token is not a valid number/character. |
| `MissingField` | A required text token is empty, or a conditional field's optional is empty on encode. |
| `InvalidValue`, `Custom` | For use by custom codecs. |

---

## Building

Requires CMake ≥ 3.25 and a C++23 compiler with `<expected>` (GCC 13+, Clang 17+, MSVC 19.36+). The generator pulls in
yaml-cpp and the tests pull in Catch2, both via FetchContent. The runtime and generated code have no dependencies.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

| CMake option | default | description |
|--------------|---------|-------------|
| `GMP_BUILD_GENERATOR` | `ON` | Build `gmpc`. Turn off when cross-compiling only the runtime and pointing `gmp_generate` at a host `gmpc`. |
| `GMP_BUILD_TESTS` | `ON` when top level | Runtime, generator and round-trip tests. |
| `GMP_BUILD_EXAMPLES` | `ON` when top level | Programs in `examples/`. |
| `GMP_BUILD_BENCH` | `OFF` | Throughput benchmark `gmp_bench` (build in Release). |

## CMake integration

```cmake
gmp_generate(TARGET <name>
             SCHEMAS <file.yaml>...
             [OUT_DIR <dir>]          # default ${CMAKE_CURRENT_BINARY_DIR}/gmp_generated/<name>
             [LANG <backend>]         # default cpp
             [GENERATOR <path>])      # default: the in-tree gmpc target
```

This creates an INTERFACE library `<name>` with:
- the generated headers on its include path
- `gmp::runtime` linked in
- a build dependency that regenerates whenever a schema or `gmpc` changes

If your custom codec headers live elsewhere, add their directory with
`target_include_directories(<name> INTERFACE ...)`.

## gmpc command line

```
gmpc [options] schema.yaml...
  --lang NAME        output language backend (default: cpp)
  --out DIR          output directory (default: current directory)
  --check            validate schemas only, write nothing
  --stdout           print generated code instead of writing files
  --opt KEY=VALUE    backend specific option (passed through to the backend)
  --list-backends    list available backends
  -h, --help         show help
```

- Exit status is 0 on success, 1 on schema errors, 2 on usage errors.
- Output files are rewritten only when their content changes, so builds don't redo work needlessly.

## Project layout

```
runtime/include/gmp/     header-only runtime: framing, stream parser, checksums, bit/byte codecs, text, containers
generator/src/           schema loader, expression parser, IR + lowering, validator, backends
cmake/GmpGenerate.cmake  gmp_generate() build integration
tests/                   runtime unit tests, generator golden + diagnostics tests, round-trip tests
examples/  bench/  docs/
```

A more compact schema reference is in [docs/schema.md](docs/schema.md).

## Adding a language backend

1. Implement `gmpc::Backend` (`generator/src/backend/backend.hpp`). It consumes the lowered `ir::Schema`, in which
   types are resolved, layouts and offsets computed, and expressions parsed into trees.
2. Register the backend in `backend/registry.cpp`.
3. Put language-specific schema settings in a block named after the backend inside `custom` entries
   (e.g. `py: {...}`).

To update golden files after an intentional change to the generated code, run the generator tests with
`GMP_UPDATE_GOLDEN=1` and review the diff.
