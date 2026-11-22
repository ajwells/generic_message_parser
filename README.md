# generic_message_parser (GMP)

Describe a wire protocol in YAML and get efficient C++ (or C) decode/encode code. GMP is like protobuf, but the schema
describes **existing** protocols: binary or ASCII, any framing, any bit layout. It does not impose its own format.

- **Generator** `gmpc`: YAML → language-neutral IR → backend: C++23 (default) or C99 / C11 (`--lang c`, see
  [C output](#c-output)); more can be added.
- **Runtime** `gmp::runtime`: header-only and standard-library only. No heap, no exceptions, no RTTI.
- **Two input modes**: whole buffer per message (`Proto::parse(buf)`) or streaming
  (`Proto::Stream<>::feed(chunk, handler)`) with resync on garbage.
- **Two access styles** per message:
  - lazy zero-copy **views** that decode fields straight from the bytes on access
  - eager **structs** with fixed-capacity storage
- **Printing** for logs: `gmp::println(msg, stdout)` shows every field, with enum names and values (`run (0x01)`) and
  engineering units.

**Contents:**
- [Quick start](#quick-start)
- [How a schema fits together](#how-a-schema-fits-together)
- [Schema reference](#schema-reference)
  - [Top level](#top-level) · [defaults](#defaults) · [enums](#enums) · [messages](#messages) · [fields](#fields)
  - [bit fields](#bit-fields) · [lengths and counts](#lengths-and-counts) · [expressions](#expressions)
  - [text messages](#text-messages) · [protocols](#protocols) · [framing](#framing) · [checksum](#checksum)
  - [message_type](#message_type) · [custom extensions](#custom-extensions)
- [Generated C++ API](#generated-c-api) · [Streaming](#streaming) · [Printing](#printing) · [Errors](#errors) · [C output](#c-output)
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

Working programs are in [examples/](examples): `binary_telemetry`, `nmea_text`, `bitpacked_can`, `custom_codec`,
`sensirion_sht4x` (an I²C humidity sensor with per-word CRCs, built from its datasheet; `sht4x.yaml` has one message per
command, `sht4x_enum.yaml` has the commands as an enum) and `reserved_sync` (a serial link where `0x80` is reserved as the
sync byte, escaped as `81 00` / `81 01` everywhere else). `binary_telemetry`, `nmea_text` and `custom_codec` also
have a C version (`main.c`, see [C output](#c-output)).

**Prefer a visual editor?** Open [`tools/schema-studio/index.html`](tools/schema-studio) in a browser. You build
schemas with forms on the left and see a size-proportional layout of every message and frame on the right, then
download the YAML. Its **Decode tester** tab decodes pasted raw frames against the schema, so you can check them before
generating code. New to GMP? Open [`tools/schema-studio/guide.html`](tools/schema-studio/guide.html) for a visual
tour of every feature, with live decoded examples.

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
| `strict_enums` | `true`, `false` | `false` | Default for each enum's `strict`: reject undeclared enum values on decode (see [enums](#enums)). |

Every default can be overridden per message, per protocol, and (for `endian`/`bit_order`) per field.

### enums

```yaml
enums:
  Mode:
    type: u8                                  # underlying integer type, default u8
    values: { idle: 0, run: 1, fault: 2 }     # name -> integer (hex/binary literals allowed)
    strict: true                              # reject undeclared values on decode (default: defaults.strict_enums)
    doc: Operating mode                       # copied into the generated comment
```

- Generates `enum class Mode : std::uint8_t`, `constexpr std::string_view to_string(Mode)` and `constexpr bool
  is_known(Mode)` (true for declared values). `to_string` returns an empty string for unlisted values.
- By default, values that are not listed still decode; the enum simply holds the raw number (check with
  `is_known()`).
- With `strict: true` (or `defaults: { strict_enums: true }`), decoding fails with `UnknownEnum` instead. This applies
  wherever the enum appears: plain fields, bit fields, array elements, nested messages, and text fields. The struct's
  `read`/`decode` and the view's `from()` check binary fields. Text views parse lazily, so the field's accessor
  returns the error. Encoding checks too: `write`/`encode` (and protocol `encode`) fail with `UnknownEnum` before
  writing an undeclared value. `strict: false` on one enum opts it out of a `strict_enums` default.
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
| `computed` | Values derived from the fields, with accessors but no bytes on the wire. See [computed values](#computed-values). |

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
| `conversions` | numeric scalars | Additional named conversions next to `scale`/`offset`, e.g. a temperature in both °C and °F. A mapping of name → `{ scale, offset, unit }`. Each adds `<name>_<conversion>()` to struct and view, and `set_<name>_<conversion>(double)` to the struct. See [field checksums and conversions](#field-checksums-and-conversions). |
| `crc` | binary scalars and enums | A checksum of this field's value bytes, stored right after them, e.g. `crc: crc8_nrsc5` for Sensirion sensors. Takes the same values as a protocol checksum's `algo`. See [field checksums and conversions](#field-checksums-and-conversions). |
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

#### field checksums and conversions

Some devices protect each value separately instead of the whole frame. For example, Sensirion sensors send every
16-bit word followed by its own CRC-8. Use `crc` on the field:

```yaml
- name: temperature
  type: u16
  crc: crc8_nrsc5                  # 2 value bytes, then 1 CRC byte over those two bytes
  scale: 0.0026703288319218739     # 175 / 65535
  offset: -45
  unit: degC
  conversions:
    degf: { scale: 0.0048065918974593731, offset: -49, unit: degF }   # 315 / 65535
```

- The field occupies the value bytes plus the checksum bytes: 3 bytes here. Following fields start after the
  checksum.
- **Decode:** the struct's `read`/`decode` and the view's `from()` verify the checksum and fail with `BadChecksum` on
  a mismatch. View accessors then read the value without re-checking.
- **Encode:** `write`/`encode` compute and write the checksum. The struct has no member for it.
- **Algorithms:** `crc` takes the same values as a protocol checksum's `algo`: a built-in name, a `custom.checksums`
  entry, or inline parameters (`crc: { crc: { width: 16, poly: 0x1021, init: 0xFFFF } }`).
- **Byte order:** a multi-byte checksum is stored in the field's byte order.
- **Restrictions:** allowed on binary scalar and enum fields, including conditional (`when`) and `const` fields. Not
  allowed on array elements or in text messages.

`conversions` adds extra engineering-unit accessors next to `scale`/`offset`, which provides `_scaled()`. With the
field above, the generated code has:
- `temperature_scaled()` for °C
- `temperature_degf()` for °F
- `set_temperature_degf(77.0)` on the struct

Each conversion needs a `scale` and/or an `offset`; `unit` is optional and only documents the value. Conversion names
must be identifiers. `scaled` and `value` are reserved, and `<field>_<conversion>` must not clash with another field.

#### computed values

A computed value is derived from a message's fields instead of being read from its own bytes. Use one when a
single logical value is split across fields. For example, the SHT4x serial number is sent as two 16-bit words,
each followed by a CRC:

```yaml
SerialNumber:
  fields:
    - { name: serial_high, type: u16, crc: crc8_nrsc5 }
    - { name: serial_low,  type: u16, crc: crc8_nrsc5 }
  computed:
    serial:
      type: u32
      value: "(serial_high << 16) | serial_low"
```

This generates `serial()` on the struct and the view, and `set_serial(std::uint32_t)` on the struct:

```cpp
view.serial();               // 0x0F4F5A3C
msg.set_serial(0x0F4F5A3C);  // serial_high = 0x0F4F, serial_low = 0x5A3C
```

| key | description |
|-----|-------------|
| `type` | Result type: `u8`…`u64`, `i8`…`i64`, `f32`, `f64`. |
| `value` | An [expression](#expressions) over the message's fields (scalars, enums, bits members; not conditional fields). It is evaluated in 64-bit integer arithmetic and then converted to `type`. |
| `set` | Optional explicit setter, written as a mapping of field to expression. In these expressions, `value` is the new computed value: `set: { lo: "value & 0xFFFF", hi: "value >> 16" }`. Fields are assigned in order. Targets must be plain scalar or enum fields: not `const`, conditional, or a derived length/count. |
| `unit`, `doc` | Documentation, copied into the generated comment. |

**Setters**
- **Without `set`:** a setter is generated automatically when `value` only packs whole unsigned fields at fixed
  shifts, joined with `|` or `+`, e.g. `(a << 16) | b` or `(b0 << 16) | (b1 << 8) | b2`. The fields must not
  overlap. Each field is set to `(v >> shift) & mask`.
- **No setter:** other expressions, e.g. `b0 + b1` or one that reads a bits member, give a read-only value. Add `set`
  if you need to write it.
- **Views are read-only:** they never get a setter.

Computed values are binary-only. Their names share the field namespace, so they must not clash with a field or a
generated accessor such as `<field>_scaled`.

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

Each generated protocol struct declares `Header`, `HeaderView`, `AnyView`, `AnyMessage`, `Frame`, `Stream`,
`StreamWith`, `Callbacks`, `CallbackStream` and `Received`. These names can't be used for messages or enums in a schema that has
a protocol; `gmpc` reports such a clash.

### framing

| `type` | keys | how frames are found |
|--------|------|----------------------|
| `none` | | The whole buffer is one frame. Use it for datagrams, CAN payloads, or anything already split by a lower layer. Whole-buffer mode only; there is no stream parser. |
| `fixed` | `sync`, `size`, `end`, `escape` | Every frame is `size` bytes, optionally starting with a `sync` pattern and ending with an `end` pattern (not escaped). Without `size`, every message must have the same fixed size and the frame size is derived. Bodies shorter than the frame are zero padded on encode. Without `sync` the parser cannot resynchronise, so a bad frame is dropped whole. |
| `length_prefixed` | `sync`, `length`, `end`, `escape` | A header field gives the size; an optional `end` pattern follows the frame (not escaped). `length.field` names the header field (unsigned integer). `length.covers` says what the value measures: `body` (default), `body_checksum` (body and checksum), `header_body`, or `all` (prefix through checksum). `length.adjust` is added to the value: covered size = value + adjust. With `escape`, `length.counts: wire` makes the value the body size on the wire (see below). On encode the field is filled in automatically. |
| `delimited` | `start`, `end`, `escape` | Frames bounded by delimiters. `start` is optional (without it, frames are separated by `end` only, like lines); `end` is required. If a new `start` appears before `end`, the partial frame is dropped (`BadDelimiter`). When `start` and `end` are the same (HDLC `0x7E`), one flag may close a frame and open the next. |
| `custom` | `framer` | A user framer from `custom.framers` decides frame boundaries. |

Shared framing keys:

| key | description |
|-----|-------------|
| `sync` / `start` | Bytes at the start of every frame. Synonyms: use `sync` for binary sync words and `start` for delimiters. |
| `end` | Bytes ending every frame (`delimited`). |
| `max_frame` | Largest frame in bytes. It bounds the length check and sizes the stream buffer. Computed automatically for binary protocols whose messages are all bounded (doubled when escaping is on). Required for text protocols and unbounded messages. |
| `escape` | Byte stuffing for `delimited`, `fixed` and `length_prefixed` frames, so reserved bytes (such as a sync byte) never appear inside a frame. See [Reserved bytes and escaping](#reserved-bytes-and-escaping). |

#### Reserved bytes and escaping

Some links reserve byte values: for example, `0x80` may only ever mean "start of frame". `escape` replaces every reserved byte
between the sync pattern and the end of the frame with two bytes: the escape `char` and a code.

```yaml
framing:
  type: length_prefixed
  sync: [0x80]
  length: { field: len }
  escape: { char: 0x81, code: index, reserved: [0x80, 0x81] }   # 0x80 -> 81 00, 0x81 -> 81 01
```

| key | description |
|-----|-------------|
| `char` | The escape byte. It is always reserved. |
| `code` | How the code byte is chosen. `xor` (default) sends `b ^ xor`, as in HDLC/PPP. `index` sends the position of `b` in `reserved`, plus `base`. `map` sends `map[b]`, as in SLIP. |
| `reserved` | The bytes to escape, in order (the order gives the `index` codes). The default is the sync/start bytes, then the end bytes, then `char`. With `index`, `char` must be in the list. Up to 16 bytes. |
| `base` | `index` only: the code of the first reserved byte (default `0`). Use it when `0x00` is reserved, because no code may be a reserved byte. |
| `xor` | `xor` only: the mask (default `0x20`). |
| `map` | `map` only: `{ raw: code }`. The keys are the reserved bytes and must include `char`. |

Escaping rules:
- **Sizes:** the `fixed` frame `size` and the `length` field count unescaped bytes. The parser reads the length through the escaping and then walks the escaped bytes to find where the frame ends.
- **Lengths counting wire bytes:** for links whose length field gives the body size *after* escaping, set `length: { field: len, covers: body, counts: wire }`. On encode the body is escaped first and its wire size written to the length field. Use `covers: body_checksum` when the length also counts the escaped checksum; the checksum must then cover only the body (`checksum.covers: body`). Other coverages are rejected: a length that counted its own bytes, or a checksum computed over the length field, would change how many escapes it needs, so some frames could not be encoded at all. A length that ends inside an escape pair fails with `BadLength`.
- **Checksum:** computed over the unescaped bytes by default. Set `checksum.over: wire` when the protocol computes it over the escaped bytes. The checksum bytes are escaped either way.
- **Resync:** when the sync byte is reserved, a sync byte inside a frame can only mean the frame was cut short. The stream parser drops the partial frame (`BadDelimiter`) and starts a new frame at that byte. A sync byte that is not reserved gives a warning.
- **Errors:** an unknown code after the escape byte fails with `InvalidValue`.
- **Buffers:** decoding unescapes in place, so `parse`, `open` and `decode` of escaped protocols take a mutable buffer. `max_frame` is doubled automatically for the worst case.

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
| `over` | With `framing.escape`: `unescaped` (default) computes the checksum over the bytes before escaping; `wire` computes it over the escaped bytes as sent. |
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
| `crc8_nrsc5` | CRC-8/NRSC-5 (Sensirion word CRC) | | |
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
| printing (optional) | `void gmp_print(gmp::Printer&, const T&)` in `T`'s namespace; see [Printing](#printing). |

A framer's `scan` receives the unconsumed bytes at the current stream position and returns one of:
- `FrameScan::need_more(hint)`: not enough data yet.
- `FrameScan::frame(size, consume, consume_on_error)`: a frame of `size` bytes starts here. `consume` is how much to
  drop after a good frame. `consume_on_error` is how much to drop if validation fails: 1 to rescan from the next byte,
  `size` to drop the whole frame.
- `FrameScan::skip(n, error)`: drop `n` garbage bytes, reporting `error` (or `Error::None` to skip silently).

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
| `<name>_<conversion>()`, `set_<name>_<conversion>(double)` | The same for each entry of `conversions`. |
| `<computed>()`, `set_<computed>(v)` | [Computed values](#computed-values). The setter exists when the value can be split back into fields. |
| `static Result<M> decode(bytes_view)` | Decode from the start of a buffer. Trailing bytes are ignored. |
| `Result<size_t> encode(mut_bytes out) const` | Encode into `out`. Returns bytes written, or `BufferFull`. |
| `read(ByteReader&)` / `write(ByteWriter&)` | Lower-level building blocks (used for nesting). |
| `print_to(gmp::Printer&) const` | Used by [`gmp::print`](#printing). |
| `operator==` | Member-wise comparison. |

### `class MView` (binary message)

| member | description |
|--------|-------------|
| `static Result<MView> from(bytes_view)` | Validate bounds, constants, nested messages and variable layout once. |
| `static MView unchecked(bytes_view)` | Wrap data already validated by `from()`. |
| field accessors | `name()` reads the value from the buffer. Bits groups return the unpacked struct, `bytes` a `bytes_view`, `string` a `string_view`, arrays an array view, nested messages a view, custom types `Result<T>`, and conditional fields `std::optional<...>`. |
| `<name>_scaled()`, `<name>_<conversion>()` | Engineering values. |
| `<computed>()` | [Computed values](#computed-values) (read only). |
| `data()`, `wire_size()` | The exact bytes of this message. |
| `print_to(gmp::Printer&) const` | Used by [`gmp::print`](#printing); prints the same text as the struct. |
| `to_struct()` | Eager decode (when the struct is generated). |

Fixed-layout messages use compile-time offsets. Messages with variable fields compute their field offsets once in
`from()`, storing 4 bytes per field in the view.

### Text messages

- **`struct M`**: members (`optional` fields are `std::optional`), `token_count`, `separator`,
  `static decode(bytes_view)`, `static decode_text(std::string_view)`, `encode(mut_bytes)`, `read_text`/`write_text`,
  and `print_to`.
- **`class MView`**:
  - `from(std::string_view)` / `from(bytes_view)` tokenise once
  - field accessors parse on demand and return `Result<T>`
  - `token(i)` gives a raw token, `present_tokens()` the number of tokens found, `raw()` the whole record
  - `to_struct()` and `print_to`

Bits groups are small structs with one member per bit field, `unpack`/`pack`/`check`, `<name>_scaled()` and
`print_to`.

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
| `template <size_t Cap = max_frame> using Stream` | The stream parser for this protocol; the handler is passed on each call (see [Streaming](#streaming)). |
| `StreamWith<H, Cap>`, `make_stream(handler)` | Stream parser that stores a handler: `feed(bytes)` / `next()` take no handler ([Storing the handler](#storing-the-handler)). |
| `Callbacks`, `CallbackStream<Cap>` | Runtime callbacks: `on<&App::fn>(app)` per message, for every frame (`AnyView`) and for `gmp::FrameError`. |
| `Received` | What `Stream::poll_frame()` returns: `header` (protocols with a header), `message` (`AnyView`) and the frame `bytes` ([Pulling messages](#pulling-messages-poll)). |
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
- Handlers run synchronously inside `feed()` / `next()`. If `feed()` is called from an interrupt, the handlers run there
  too.

### Pulling messages (`poll`)

Instead of being called back, code can ask for the next message. This suits a main loop, a state machine, or a
request/response exchange:

```cpp
parser.push(bytes);
while (auto m = parser.poll()) {                 // next valid message; empty = more input needed
    std::visit(gmp::overloaded{
        [](const demo::StatusView& v) { ... },
        [](const demo::CommandView& v) { ... },
    }, *m);
}
```

| call | returns |
|------|---------|
| `poll()` | `Polled<AnyView>`: the next valid message. |
| `poll_frame()` | `Polled<Received>`: `{ header, message, bytes }`. `header` exists only on protocols with a header; `bytes` is the frame. For protocols with escaping, it has been unescaped in place, so only the header and body are meaningful. |
| `poll_message()` | `std::optional<AnyMessage>`: the decoded struct, a copy that stays valid. A frame whose struct decode fails (e.g. more than `max` elements) is dropped like any bad frame. |
| `poll_event()` | `Polled<std::variant<AnyView, gmp::FrameError>>`: messages and errors inline, in stream order. |
| `poll_for<V>()` | `Polled<V>`: wait (without blocking) for one message type, e.g. a reply. Only on parsers with a stored handler (`CallbackStream`, `make_stream`): other messages that arrive meanwhile go to that handler. |

- **Errors:**
  - Garbage and bad frames are skipped and counted in `stats()`, and `last_error()` holds the most recent one.
  - On a parser with a stored handler, `poll()`, `poll_frame()`, `poll_message()` and `poll_for()` pass errors to
    its `FrameError` listener.
  - `poll_event()` returns errors instead of passing them on.
  - `poll` never calls message listeners. `poll_for` calls them only for the other message types.
- **View lifetime:** a polled view points into the parser buffer. It is valid until the next `push`, `poll*`,
  `feed`, `next` or `reset` on that parser.
  - `gmp::Polled<T>` works like `std::optional` and has `valid()`.
  - In Debug builds, `*`, `->` and `value()` assert that the view hasn't expired. Define
    `GMP_ASSERT(cond, msg)` to route that check elsewhere.
  - Copy what you need, or use `poll_message()`.
- `push` and `poll` can be mixed freely with `feed` and `next`. Each frame is consumed by whichever call reaches it
  first.

### Storing the handler

The handler can also live in the parser, so the code that receives bytes only calls `parser.feed(bytes)` (or
`next()`). There are two forms:

**Compile-time handler** (`make_stream`, `StreamWith<H>`): no runtime cost, with the same overload rules as above.

```cpp
auto parser = demo::Link::make_stream(gmp::overloaded{
    [&](const demo::StatusView& v) { ... },
    [](const gmp::FrameError& e) { ... },
});
parser.feed(chunk);

// as a class member: use a named handler type
struct LinkHandler { App& app; void operator()(const demo::StatusView& v) const { app.on_status(v); } };
class App { demo::Link::StreamWith<LinkHandler> parser_{LinkHandler{*this}}; /* ... */ };
```

**Runtime callbacks** (`CallbackStream`, `Callbacks`): the parser type has a name and doesn't depend on the handlers.
It can be a global or a member, be passed to a non-template driver, and have listeners added or changed at runtime.

```cpp
demo::Link::CallbackStream<> parser;
parser.on<&App::on_status>(app)        // void App::on_status(const StatusView&)
      .on<&App::on_command>(app)       // void App::on_command(const Link::HeaderView&, const CommandView&)
      .on<&App::on_frame>(app)         // void App::on_frame(const Link::AnyView&): every valid frame
      .on<&App::on_drop>(app);         // void App::on_drop(const gmp::FrameError&)
parser.feed(chunk);
```

- **The slot is chosen from the function's last parameter:**
  - a message view: that message's listener
  - `AnyView`: the every-frame listener
  - `gmp::FrameError`: the error listener

  There is one listener per slot, and registering again replaces it. If several parts of the program need the same
  message, register one function that forwards to them.
- **Header (protocols with a header):** a function that takes `(HeaderView, View)` gets the header, and one that takes
  `(View)` doesn't. On a protocol without a header, only `(View)` compiles.
- **Order:** for each frame, the message listener runs first, then the every-frame listener. Messages without a
  listener are ignored.
- **What you can bind:**

| form | example |
|------|---------|
| member function | `on<&App::on_status>(app)` |
| function with a context (`C&` or `C*`) | `on<&handle_status>(app)` for `void handle_status(App&, const StatusView&)` |
| captureless lambda | `on<[](App& a, const StatusView& v) { ... }>(app)` |
| function without a context | `on<&log_status>()` |
| function pointer chosen at run time | `on(&c_callback, ctx)` for `void c_callback(void* ctx, const StatusView&)`, or a typed `App*` |

- Signatures are checked when you register, so a mismatch is a compile error. There's no `std::function` and no heap.
  Each listener is a function pointer and a context pointer, called through one indirect call.
- `off<StatusView>()` removes a listener, `has<V>()` checks for one, and `clear()` removes all. `callbacks()` returns
  the `Callbacks` object. A `Callbacks` object also works as a plain handler: `stream.feed(bytes, callbacks)`.
- The context object must outlive the registration. A `CallbackStream` can't be copied.


## Printing

Every generated struct, view and bits group has `print_to(gmp::Printer&)`. `gmp::print` and `gmp::println`
(`<gmp/print.hpp>`, included by every generated header) use it to dump a message for a console or a debug log:

```cpp
gmp::println(status, stdout);                          // std::FILE*: console, or a log opened with fopen
gmp::print(status, std::cout);                         // anything with write(const char*, n), e.g. std::ostream
char line[256];  gmp::print(status, line);             // char array or std::span<char>, always NUL terminated
gmp::static_string<256> s;  gmp::print(status, s);     // appends; also std::string or anything with append()
gmp::print(status, [](std::string_view t) { uart_log(t); });   // callback; return false from it to stop

gmp::print(status, stdout, {.style = gmp::PrintStyle::single_line});
```

```
Status {
  voltage: 1234 (12.34 V)
  flags: {
    armed: true
    mode: run (0x01)
    level: -3
  }
  count: 3
  samples: [10, -20, 30]
  ext: <absent>
  label: "abc"
}
```

Single line: `Status { voltage: 1234 (12.34 V), flags: { armed: true, mode: run (0x01), level: -3 }, count: 3, ... }`.

| value | printed as |
|-------|------------|
| integer, float, bool, char | `42`, `1.5` (shortest text of the field's own type), `true`, `'N'` |
| enum | Name and value in hex, sized to the enum's type: `run (0x01)`. An undeclared value prints as `<unknown> (0x07)`. |
| `scale` / `offset`, `conversions` | The raw value, then each engineering value with its unit: `26214 (25 degC, 77 degF)`. A `unit` without scaling follows the value: `3600 s`. |
| bits group, nested message | An indented `{ ... }` block (`Name { ... }` for messages). |
| `bytes` | Hex: `[0A 1B 2C]`. |
| `string` | Quoted, with `\r \n \t \" \\ \xHH` escapes. |
| array | `[1, 2, 3]`. Arrays of messages print one element per line. |
| conditional field | Its value, or `<absent>`. Text `optional` fields that are empty also print `<absent>`. |
| text view field that fails to parse | `<error: parse error>`. |
| computed value | `serial: 305419896 (computed)`. |
| `const` and `pad` fields | Not printed (they have no member). |
| custom type | `gmp_print(gmp::Printer&, const T&)` declared next to `T` (found by ADL), otherwise `<?>`. |

A view prints exactly the same text as its struct. A `std::variant` (for example the result of `Protocol::parse()`) prints
the message it holds, and a lone enum or number prints too: `gmp::print(Mode::run, stdout)` → `run (0x01)`.

| `gmp::PrintOptions` | default | |
|---------------------|---------|-|
| `style` | `multi_line` | or `single_line` |
| `indent` | `2` | spaces per level (`multi_line`) |
| `max_items` | `64` | array elements shown before `... (+N more)`; `0` shows all |
| `max_bytes` | `64` | bytes shown for a `bytes` field; `0` shows all |
| `precision` | `9` | significant digits of scaled and converted values |

`print` returns `Result<size_t>`, the number of characters written. Bounded sinks (char buffers, `static_string`, a
callback that returns `false`) keep what fits and return `BufferFull`. Printing never allocates. Output for an
unbounded sink such as `std::string` grows that string as usual. A custom type's hook writes with `p.text(...)`
or `p.value(member)`:

```cpp
inline void gmp_print(gmp::Printer& p, const my::Time& t) noexcept {   // 12:34:56
    char b[9];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d", t.h, t.m, t.s);
    p.text(b);
}
```

The Studio's decode tester shows the same text for each decoded frame (**View: Print / One line**).

## Errors

Every fallible operation returns `gmp::Result<T>` (`std::expected<T, gmp::Error>`). `gmp::to_string(Error)` gives a
readable name.

| error | meaning |
|-------|---------|
| `Truncated` | Input ended before the message/frame was complete. |
| `BadSync` | Bytes before a sync word / start delimiter were skipped. |
| `BadChecksum` | Frame checksum or field `crc` mismatch, or a text checksum missing or malformed. |
| `BadLength` | Length field out of range, frame larger than `max_frame`, or container size inconsistent with a length expression on encode. |
| `BadDelimiter` | A new start delimiter appeared inside a frame. |
| `BadConst` | A `const` field or bit field did not match. |
| `OutOfRange` | Data exceeds a container's `max`, or a value does not fit. |
| `UnknownType` | No message has the frame's `type_id`. |
| `BufferFull` | Output buffer too small, stream buffer full without a frame, or a bounded print sink was full. |
| `ParseError` | A text token is not a valid number/character. |
| `MissingField` | A required text token is empty, or a conditional field's optional is empty on encode. |
| `UnknownEnum` | A field of a `strict` enum holds a value that is not declared (on decode, or in a struct being encoded). |
| `InvalidValue`, `Custom` | For use by custom codecs. |

## C output

`gmpc --lang c` (or `gmp_generate(... LANG c)`) writes `<schema>.gmp.h` and `<schema>.gmp.c` for C99 or later, using
the C runtime in `runtime-c/` (header-only, or a compiled library: see [C runtime](#c-runtime-header-only-or-library)).
The generated header also compiles as C++. C programs using the same schemas as the C++ examples are `main.c` in
[examples/binary_telemetry](examples/binary_telemetry) (stream + callbacks), [examples/nmea_text](examples/nmea_text)
(push + `_poll_event`) and [examples/custom_codec](examples/custom_codec) (`c:` codecs and a print hook).

Everything the C++ backend generates is generated for C:
- binary messages: structs and views, with bits, `const`, `pad`, `when`, bytes/strings/arrays, nested messages,
  field `crc:`, `scale`/`conversions` and `computed:`
- text messages: tokens with `optional`, `width`, `precision`, `base`; views parse fields on demand
- protocols: every framing (`none`, `fixed`, `length_prefixed`, `delimited`, custom framers), byte stuffing, frame
  checksums (also `over: wire`, custom checksums, NMEA-style text checksums), whole-buffer parse / decode / encode,
  callbacks, a stream parser and the pull API
- printing: `_print` for every struct, view, bits group and protocol `_any_view` / `_any_message`, with the same text
  as `gmp::print`

Custom types, checksums and framers use `c:` blocks (see [custom extensions](#custom-extensions)). The C output is
checked against the C++ output on valid, corrupted and noisy input, including streams fed in chunks of every size,
and its print text is compared with `gmp::print` for every decoded message.

C99 and C11 compile the same generated code. With C11 the `.gmp.c` also checks the signatures of the functions
named in `c:` blocks (`_Static_assert` + `_Generic`, behind `#if GMSG_C11`), which gives a clear message on a
mismatch; C99 builds skip that check, and the compiler still checks every call.

### C runtime: header-only or library

| | CMake target | what you get |
| --- | --- | --- |
| header-only (default) | `gmp::runtime_c` | every runtime function is `static inline` in `runtime-c/include/gmp/c/`; nothing to compile; CRC presets bit by bit |
| library | `gmp::runtime_c_lib` | checksums, framing, stream, text, float text and print are compiled once from `runtime-c/src/` (one `.c` per header, built as C99 with one section per function); the headers only declare them; CRC presets use 256-entry lookup tables |

```cmake
gmp_generate(TARGET link_msgs LANG c C_RUNTIME library SCHEMAS link.yaml)   # links gmp::runtime_c_lib
target_compile_definitions(gmp_runtime_c_lib PUBLIC GMSG_USE_LIBC=0)       # options: PUBLIC, so both sides agree
```

The library is smaller when several files use the runtime (one copy of each function) and its CRC presets are 4 to 8
times faster (x86, `-O2`), at 256 to 2048 bytes of constant data per table used. Small functions (endian, bits, byte
reader / writer) stay inline in both builds. Every `GMSG_` option must have the same value for the library and its
users; header-only and library code can still be mixed in one program, since the header-only copies are `static`.
Without CMake, compile the files in `runtime-c/src/` with `-DGMSG_RUNTIME_LIBRARY=1` and define the same for your code.
The lookup tables can also be used header-only with `GMSG_CRC_TABLES=1`, and turned off in the library with
`GMSG_CRC_TABLES=0`. The tables are generated by `runtime-c/tools/crc_tables.py`.

Names start with a prefix: the schema `namespace` with `::` replaced by `_` (`acme::telemetry` → `acme_telemetry_`),
or the file name when there is no namespace, or `--opt prefix=...`. The runtime itself uses `gmsg_` / `GMSG_`.

```c
#include "telemetry.gmp.h"

acme_telemetry_status s;
gmsg_error e = acme_telemetry_status_decode(buf, len, &s);      /* GMSG_OK or GMSG_ERR_TRUNCATED, ... */
if (e == GMSG_OK && s.has_ext) use(s.ext.pressure);
for (size_t i = 0; i < s.samples_count; ++i) use(s.samples[i]);

acme_telemetry_status_view v;                                    /* zero copy, points into buf */
if (acme_telemetry_status_view_from(buf, len, &v) == GMSG_OK) {
    double volts = acme_telemetry_status_view_voltage_scaled(&v);
    gmsg_str label = acme_telemetry_status_view_label(&v);       /* { data, size }, not NUL terminated */
}

size_t n = 0;
e = acme_telemetry_status_encode(&s, out, sizeof out, &n);      /* GMSG_ERR_BUFFER_FULL if it does not fit */
```

| schema | C |
|--------|---|
| message `Status` | `typedef struct acme_telemetry_status {...}`, `_read` / `_write` / `_decode` / `_encode`, macros `ACME_TELEMETRY_STATUS_MIN_WIRE_SIZE` / `_MAX_WIRE_SIZE` / `_WIRE_SIZE` |
| view | `acme_telemetry_status_view`, `_view_from()` / `_view_unchecked()` / `_view_to_struct()`, one accessor function per field |
| enum `Mode` | `typedef uint8_t acme_telemetry_mode;`, `ACME_TELEMETRY_MODE_FAULT`, `_to_string()` (NULL if unknown), `_is_known()` |
| bits group `flags` | `acme_telemetry_status_flags` with `_pack()` / `_unpack()` / `_check()` / `_print()` |
| variable bytes / string / array | fixed-capacity array plus its size. With a bare length field (`length: n`, `count: n`) that field is the size; otherwise (expression, `remaining`, terminator, fixed strings) a member `<field>_len` (bytes, strings) or `<field>_count` (arrays). Strings are NUL terminated after decode, and encode uses the size |
| `when` field, `optional` text field | `bool has_<field>` in the struct, `_view_has_<field>()` on the view |
| text message `GGA` | struct `nav_nmea_gga` with `_read_text` / `_write_text` / `_decode` / `_encode`; view accessors parse on demand: `gmsg_error nav_nmea_gga_view_latitude(&v, &out)` (`GMSG_ERR_MISSING_FIELD` for an empty token), `_view_token(&v, i)`, `_view_present_tokens(&v)`; strings as `gmsg_str` |
| array on a view | `_view_<field>_count()` and `_view_<field>_at(i)` |
| `scale` / `conversions` / `computed` | `_<field>_scaled()`, `_set_<field>_scaled()`, `_<name>()`, `_set_<name>()` |
| protocol `BinaryLink` | `acme_telemetry_binary_link_parse()` / `_decode()` / `_process()` / `_encode_<message>()` / `_encode_<message>_with_header()` / `_encode()`, tagged unions `_any_view` / `_any_message` (`kind` = `ACME_TELEMETRY_BINARY_LINK_STATUS`, ...) with `_any_view_print()` / `_any_message_print()`, `_callbacks`, `_stream`, `ACME_TELEMETRY_BINARY_LINK_MAX_FRAME` |
| print | `void acme_telemetry_status_print(const acme_telemetry_status* m, gmsg_printer* p)`, also `_view_print()`; see below |

Protocols with byte stuffing take a non-const buffer in `_parse` / `_decode` / `_process`: frames are unescaped in
place, as in C++.

```c
static void on_status(void* ctx, const acme_telemetry_binary_link_header_view* h, const acme_telemetry_status_view* v) {
    struct app* a = ctx;                                         /* views are valid during the call only */
    a->volts = acme_telemetry_status_view_voltage_scaled(v);
}

static uint8_t rx_buf[ACME_TELEMETRY_BINARY_LINK_MAX_FRAME];     /* any size >= MAX_FRAME */
acme_telemetry_binary_link_stream rx;
acme_telemetry_binary_link_stream_init(&rx, rx_buf, sizeof rx_buf);
rx.cb.ctx = &app;
rx.cb.on_status = on_status;                                     /* NULL slots are skipped; also on_frame, on_error */
acme_telemetry_binary_link_stream_feed(&rx, bytes, n);           /* also _push / _next / _reset / _stats / _last_error */

acme_telemetry_binary_link_process(datagram, len, &rx.cb);       /* one whole frame, same callbacks */
size_t written = 0;
acme_telemetry_binary_link_encode_status(&s, out, sizeof out, &written);  /* sync, header (id, length), body, CRC */
```

Pull API on the stream (as in C++; see [Pulling messages](#pulling-messages-poll)):

```c
acme_telemetry_binary_link_any_view v;
acme_telemetry_binary_link_stream_push(&rx, bytes, n);
while (acme_telemetry_binary_link_stream_poll(&rx, &v)) {        /* also _poll_frame / _poll_message / _poll_event */
    if (v.kind == ACME_TELEMETRY_BINARY_LINK_STATUS) use(&v.as.status);
}
/* wait for a reply; other messages go to the callbacks meanwhile */
if (acme_telemetry_binary_link_stream_poll_for(&rx, ACME_TELEMETRY_BINARY_LINK_COMMAND, &v)) handle(&v.as.command);
```

A polled view stays valid until the next call on that stream (`_stream_generation()` changes when it expires). The
poll functions never call message callbacks (except `_poll_for` for other messages); errors go to `on_error`, except
in `_poll_event`, which returns them.

For each frame the message's callback runs first, then `on_frame` (every frame). Headerless protocols have no `h`
parameter. The stream struct holds no pointer to itself, so it can live anywhere; its buffer is the caller's.

Printing (`gmp/c/print.h`) gives the same text as [`gmp::print`](#printing), with the same options:

```c
gmsg_printer p;
gmsg_printer_init_file(&p, stdout, NULL);                        /* FILE* (needs GMSG_USE_LIBC) */
acme_telemetry_status_print(&s, &p);                             /* or _view_print(&v, &p), _any_view_print(...) */
gmsg_printer_finish(&p, true, NULL);                             /* true: end with a newline */

char line[256];
gmsg_print_options o = gmsg_print_defaults();                    /* style, indent, max_items, max_bytes, precision */
o.style = GMSG_PRINT_SINGLE_LINE;
gmsg_printer_init_buffer(&p, line, sizeof line, &o);             /* always NUL terminated */
acme_telemetry_status_view_print(&v, &p);
size_t n = 0;
gmsg_error e = gmsg_printer_finish(&p, false, &n);               /* GMSG_ERR_BUFFER_FULL when cut */

gmsg_printer_init(&p, uart_write, &uart, NULL);                  /* bool uart_write(void* ctx, const char* s, size_t n) */
```

A view field that fails to parse prints `<error: parse error>` (C++: the same). Custom types print through the `print`
function of their `c:` block. Floats use `GMSG_FLOAT_TEXT` (below); with `GMSG_FLOAT_TEXT_NONE` they print as `<float>`.

Runtime options (define before including, or on the command line): `GMSG_USE_LIBC` (default 1; 0 needs no C library at
all), `GMSG_FLOAT_TEXT`, `GMSG_RUNTIME_LIBRARY` and `GMSG_CRC_TABLES` (see [above](#c-runtime-header-only-or-library)),
`GMSG_ASSERT`, `GMSG_API`. See `runtime-c/include/gmp/c/config.h`.

Floats in text messages and print (`GMSG_FLOAT_TEXT`):
- `GMSG_FLOAT_TEXT_BUILTIN` (default): exact conversions on a small fixed-size big integer (no heap, no `<math.h>`,
  about 4 KB of stack). The text is the same as the C++ runtime (`std::to_chars` / `std::from_chars`): shortest
  round trip, fixed with `precision`, or (print) a number of significant digits, rounded half to even; parsing is
  strict and locale free.
- `GMSG_FLOAT_TEXT_LIBC`: `snprintf` / `strtod`. Smaller, but may differ in the last digit and uses the C locale.
- `GMSG_FLOAT_TEXT_NONE`: no float text; a schema with float fields in text messages fails to compile with a
  message naming the schema.

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
             [C_RUNTIME header|library]  # LANG c: gmp::runtime_c (default) or gmp::runtime_c_lib
             [GENERATOR <path>])      # default: the in-tree gmpc target
```

This creates a library `<name>` (INTERFACE for C++; STATIC for `LANG c`, built from the generated `.c` files) with:
- the generated headers on its include path
- `gmp::runtime` (C++) or `gmp::runtime_c` (C; `gmp::runtime_c_lib` with `C_RUNTIME library`) linked in
- a build dependency that regenerates whenever a schema or `gmpc` changes

If your custom codec headers live elsewhere, add their directory with
`target_include_directories(<name> INTERFACE ...)`.

## gmpc command line

```
gmpc [options] schema.yaml...
  --lang NAME        output language backend: cpp (default) or c (C99 or later, see C output)
  --out DIR          output directory (default: current directory)
  --check            validate schemas only, write nothing
  --stdout           print generated code instead of writing files
  --opt KEY=VALUE    backend specific option (c: prefix=NAME_ sets the C name prefix)
  --list-backends    list available backends
  -h, --help         show help
```

- Exit status is 0 on success, 1 on schema errors, 2 on usage errors.
- Output files are rewritten only when their content changes, so builds don't redo work needlessly.

## Project layout

```
runtime/include/gmp/     header-only runtime: framing, stream parser, checksums, bit/byte codecs, text, containers, print
runtime-c/include/gmp/c/ C runtime for the C backend: endian, bits, byte reader/writer, checksums, framing, stream, text, print
runtime-c/src/           the C runtime's library build (gmp::runtime_c_lib); runtime-c/tools/crc_tables.py
generator/src/           schema loader, expression parser, IR + lowering, validator, backends
cmake/GmpGenerate.cmake  gmp_generate() build integration
tests/                   runtime unit tests (C++ and C), generator golden + diagnostics tests, round-trip tests
examples/  bench/  docs/
```

A more compact schema reference is in [docs/schema.md](docs/schema.md).

## Using GMP with an AI assistant

[`skills/gmp/`](skills/gmp) is an [Agent Skill](https://docs.claude.com/en/docs/agents-and-tools/agent-skills/overview)
that teaches an AI assistant how to use GMP. It covers modelling a datasheet or packet capture as a schema, running
`gmpc`, wiring up CMake, using the generated structs, views and stream parser, and the common pitfalls. It is
self-contained: the full schema reference, the C++ API and six worked recipes from `examples/` are bundled in
`skills/gmp/references/`.

- **Claude Code:** copy or symlink the folder to `~/.claude/skills/gmp` (all your projects) or to
  `<your project>/.claude/skills/gmp` (one project, shared with your team through git). Claude then loads it whenever
  you work on GMP schemas or generated code.
- **Claude apps:** zip the `gmp` folder and upload it as a custom skill.
- **Other assistants:** point their rules or context file (for example `AGENTS.md`) at `skills/gmp/SKILL.md`; it links
  to the reference files.

After changing `docs/schema.md`, the README API sections or `examples/`, run `python skills/gmp/build_references.py` to
refresh the bundled references.

## Adding a language backend

1. Implement `gmpc::Backend` (`generator/src/backend/backend.hpp`). It consumes the lowered `ir::Schema`, in which
   types are resolved, layouts and offsets computed, and expressions parsed into trees. Schema rules that hold for
   every language are checked in lowering; the backend reports only language-specific problems (e.g. names its
   output reserves) to the `Diagnostics` it is given.
2. Register the backend in `backend/registry.cpp`.
3. Add its output file suffixes and runtime target to `_gmp_language_layout` in `cmake/GmpGenerate.cmake`. Header-only
   output gives an INTERFACE library; output with source files gives a STATIC library.
4. Put language-specific schema settings in a block named after the backend inside `custom` entries
   (e.g. `py: {...}`).
5. Add a `check_goldens("<name>")` test case to `tests/generator/test_golden.cpp`. Goldens live in
   `tests/generator/golden/<name>/`, one file per generated output file.

To update golden files after an intentional change to the generated code, run the generator tests with
`GMP_UPDATE_GOLDEN=1` and review the diff.
