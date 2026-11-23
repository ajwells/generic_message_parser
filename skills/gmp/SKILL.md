---
name: gmp
description: How to use GMP (generic_message_parser), the YAML-driven C++23 code generator for binary and text wire protocols. Use this skill whenever the user is writing or editing a GMP schema (.yaml with schema_version / messages / protocols / framing), running gmpc or gmp_generate(), using generated *.gmp.hpp code (Views, structs, Protocol parse / encode / Stream), or wants to describe an existing serial, sensor, CAN, I2C, UART, NMEA or other packet format so C++ can decode and encode it. Also use it when the user asks to "parse this datasheet / packet layout into C++", add framing, checksums, CRCs, byte stuffing or sync bytes to a protocol, or debug gmpc diagnostics, even if they don't say "GMP" by name but the project contains gmp/ headers or .gmp.hpp files.
---

# GMP: describe a wire protocol in YAML, get C++ that parses it

GMP is a protobuf-like toolchain for **existing** wire formats. A YAML schema describes byte layouts, framing,
checksums and message selection. `gmpc` turns it into one header (`<schema>.gmp.hpp`) with:

- **structs** (eager value types, fixed-capacity containers) for decode + encode
- **Views** (zero-copy accessors over the input bytes) for decode
- **Protocols** with `parse()`, `decode()`, `encode()` and a streaming parser `Stream<>`

The runtime is header-only C++23 (`std::expected`), with **no heap, no exceptions, no RTTI**, so the generated code
runs on microcontrollers and desktops alike. Keep that in mind: never suggest `std::vector`, `std::string` or
exceptions in code that works with GMP types; GMP uses `gmp::static_vector`, `gmp::static_string` and `gmp::Result<T>`.

## Workflow

1. **Understand the format first.** Collect: byte order, every field's width and meaning, units / scaling, how frames
   start and end, the checksum (algorithm and what it covers), and how one message type is told apart from another.
   If a datasheet or capture is ambiguous about any of these, ask the user rather than guess; a wrong guess produces a
   schema that compiles but silently decodes garbage.
2. **Write the schema.** Start from the closest recipe in `references/recipes.md`. Put payload layouts in `messages`,
   framing in `protocols`. A message is usable on its own without any protocol.
3. **Validate:** `gmpc --check schema.yaml`. Diagnostics are `file:line:col: error: …` and are precise; fix them
   rather than working around them. Unknown keys are errors (typos are caught), and renamed keys say what they became.
4. **Generate** through CMake (preferred) or the CLI (below).
5. **Use** the generated API (below and `references/cpp-api.md`).
6. **Verify with real bytes.** Decode a known-good capture and check values; round-trip `encode` → `decode`.
   The Schema Studio (`tools/schema-studio/index.html` in the GMP repo) has a decode tester that shows exactly which
   bytes went into which field. Its guide page (`guide.html`) shows every feature with decoded examples.

## Schema at a glance

```yaml
schema_version: 1
namespace: acme::link                 # C++ namespace
defaults: { endian: big }             # also bit_order, format, separator, access, strict_enums

enums:
  Mode: { type: u8, values: { idle: 0, run: 1, fault: 2 } }

messages:
  Status:
    type_id: 1                        # how a protocol recognises this message
    fields:
      - { name: magic, type: u8, const: 0x5A }                    # checked on decode, written on encode
      - { name: voltage, type: u16, scale: 0.001, unit: V }       # voltage_scaled() = raw * 0.001
      - name: state
        type: bits                                                # bit fields, whole bytes
        fields:
          - { name: armed, bits: 1 }
          - { name: mode, bits: 2, enum: Mode }
          - { pad: 5 }
      - { name: count, type: u8 }
      - { name: samples, type: array, of: i16, count: count, max: 32 }   # count filled in on encode
      - { name: fault_code, type: u16, when: "state.mode == Mode.fault" } # optional field

protocols:
  Link:
    header:
      - { name: id, type: u8 }
      - { name: len, type: u16 }
    framing: { type: length_prefixed, sync: [0xAA, 0x55], length: { field: len } }
    checksum: { algo: crc16_ccitt }            # covers header + body by default
    message_type: { field: id }                # header.id selects the message by its type_id
    messages: [Status]
```

### Field types

| type | notes |
|------|-------|
| `u8…u64`, `i8…i64`, `f32`, `f64`, `bool`, `char` | scalars; `endian:` per field overrides |
| enum name | stored as the enum's integer type; `strict: true` rejects undeclared values |
| `bits` | `fields: [{ name, bits, type/enum/const/scale }, { pad: N }]`, must total whole bytes; `bit_order: msb_first \| lsb_first` |
| `bytes`, `string` | `length: 8` · `length: other_field` · `length: "n * 2"` · `length: remaining` (last field) · `terminator: 0` |
| `array` | `of: <type>`, `count:` same forms as length |
| message name | nested message |
| custom type | your C++ codec (`custom.types`) |
| `{ pad: N }` | reserved bytes (text: skipped tokens) |

Useful field keys: `const`, `scale` / `offset` / `unit`, `conversions: { degf: { scale, offset, unit } }`,
`crc: crc8_nrsc5` (checksum stored right after this value), `when: "<expr>"`, `max` (container capacity),
`min_value` / `max_value` (limits on the scaled value) and `min_raw` / `max_raw` (on the wire value), `doc`. Messages can also declare `computed: { serial: { type: u32, value: "(hi << 16) | lo" } }`.

### Framing (protocols)

| `framing.type` | use for |
|----------------|---------|
| `none` | the buffer already is one message (datagram, CAN payload, I2C read) |
| `fixed` | every frame has the same size, optional `sync:` |
| `length_prefixed` | a header field gives the size: `length: { field, covers: body \| body_checksum \| header_body \| all, adjust }` |
| `delimited` | `start:` (optional) … `end:` patterns, e.g. `"$"` … `"\r\n"`, HDLC `0x7E` flags |
| `custom` | your own C++ framer |

- **Checksum:** `checksum: { algo, covers: header_body | body | all, endian }`. Built-ins include `crc8`, `crc8_maxim`,
  `crc8_nrsc5`, `crc16_ccitt`, `crc16_modbus`, `crc16_x25`, `crc32`, `crc32c`, `xor8`, `sum8`, `fletcher16`, or inline
  `{ crc: { width, poly, init, refin, refout, xorout } }`.
- **Message selection:** `message_type: { field: <header field> }` (binary) or `{ token: 0 }` (text, e.g. `GPGGA`).
- **Byte stuffing / reserved bytes:** `escape: { char: 0x7D, xor: 0x20 }` (HDLC),
  `{ char: 0x81, code: index, reserved: [0x80, 0x81] }` (escape + index byte), `{ char: 0xDB, map: {…} }` (SLIP).
  Works with delimited, fixed and length_prefixed framing.

Full details of every key: `references/schema.md`.

## Generating code

```cmake
add_subdirectory(generic_message_parser)                 # or FetchContent
gmp_generate(TARGET link_msgs SCHEMAS link.yaml)         # INTERFACE lib: generated headers + gmp::runtime
target_link_libraries(app PRIVATE link_msgs)
```

CLI: `gmpc --out gen/ link.yaml` · `gmpc --check link.yaml` (validate only) · `gmpc --stdout link.yaml`.
Requires a C++23 compiler with `<expected>` (GCC 13+, Clang 17+, MSVC 19.36+).

Other languages from the same schema: `--lang c` (C99 or later, `<schema>.gmp.h` + `.gmp.c`, runtime `runtime-c/`,
names prefixed from the namespace: `acme_link_status_decode(...)`) and `--lang js` (one UMD module `<schema>.gmp.js` on
`runtime-js/gmsg.js`: `Status.decode(bytes)`, `Link.encode(msg)`, `Link.stream().on('Status', fn)`, errors are thrown
`gmsg.GmpError`). Custom types / checksums / framers then need `c:` or `js:` blocks (see references/schema.md); the
README sections "C output" and "JavaScript output" (in references/cpp-api.md) list the API.

## Using the generated code

```cpp
#include "link.gmp.hpp"   // everything lives in the schema's namespace

// Encode: fill a struct, let the protocol add sync, header (id, len), checksum
acme::link::Status s;
s.set_voltage_scaled(12.6);
s.state.armed = true;
s.state.mode = acme::link::Mode::run;
s.samples = {1, 2, 3};                         // `count` is derived from the container
std::array<std::uint8_t, acme::link::Link::max_frame> buf{};
gmp::Result<std::size_t> n = acme::link::Link::encode(s, buf);
if (!n) { /* gmp::to_string(n.error()) */ }

// Whole buffer (datagram): returns a std::variant of views
if (auto v = acme::link::Link::parse(gmp::bytes_view(buf.data(), *n)))
    if (auto* st = std::get_if<acme::link::StatusView>(&*v))
        std::printf("%.3f V\n", st->voltage_scaled());

// Byte stream (UART etc.): feed chunks of any size; garbage and bad frames are skipped
acme::link::Link::Stream<> stream;
stream.feed(chunk, gmp::overloaded{
    [](const acme::link::StatusView& v) { /* valid only during this call; to_struct() to keep */ },
    [](const gmp::FrameError& e) { /* optional: e.error, e.data */ },
});

// Store the handler in the parser, so the receive path (UART driver, socket loop) only passes bytes:
auto bound = acme::link::Link::make_stream(gmp::overloaded{ /* same lambdas */ });   // compile-time, zero cost
bound.feed(chunk);
acme::link::Link::CallbackStream<> link;          // runtime listeners; a named type, usable as a member/global
link.on<&App::on_status>(app)                     // slot chosen from the last parameter: a View, AnyView or FrameError
    .on<&App::on_drop>(app);                      // (HeaderView, View) gets the header, (View) does not
link.feed(chunk);

// Or pull: ask for the next message (views valid until the next push/poll/feed; Debug builds check it)
parser.push(chunk);
while (auto m = parser.poll()) { std::visit(handlers, *m); }   // also poll_frame() (header), poll_message()
auto reply = link.poll_for<acme::link::StatusView>();          // (struct copy), poll_event(), poll_for<V>()

// A message on its own, no framing
auto st = acme::link::Status::decode(bytes);     // gmp::Result<Status>
auto view = acme::link::StatusView::from(bytes); // gmp::Result<StatusView>
```

**Printing for logs and debugging:** don't hand-write printf chains for every field. Every struct, view and bits group
has `print_to()`, so `gmp::println(msg, stdout)` dumps all fields, with enums as `run (0x01)` (undeclared values:
`<unknown> (0x07)`), scaled values as `1234 (12.34 V)`, bytes as hex and absent conditional fields as `<absent>`.
Sinks are `FILE*`, a `char[N]` / `std::span<char>` buffer, `gmp::static_string<N>` or `std::string` (appends), any
`std::ostream`, or a callable taking `std::string_view`. Use `{.style = gmp::PrintStyle::single_line}` for one line
per message. It never allocates. Bounded sinks keep what fits and return `BufferFull`. Custom types print through
`void gmp_print(gmp::Printer&, const T&)` next to the type. Details: "Printing" in `references/cpp-api.md`.

Errors are values (`gmp::Result<T>` = `std::expected<T, gmp::Error>`): `Truncated`, `BadSync`, `BadChecksum`,
`BadLength`, `BadDelimiter`, `BadConst`, `OutOfRange`, `UnknownType`, `UnknownEnum`, `ValueOutOfLimits`,
`BufferFull`, `ParseError`, `MissingField`. Full API and streaming details: `references/cpp-api.md`.

## Things that trip people up

- **`max` is capacity, not a value limit.** `max: 32` sizes the struct's fixed container; decoding more fails with
  `OutOfRange`. Every variable-length field in a message with a struct needs `max`. gmpc rejects `max` anywhere
  else. For a valid range use `min_value` / `max_value` (scaled value, e.g. humidity `min_value: 0, max_value: 100`)
  or `min_raw` / `max_raw` (wire value); values outside fail decode and encode with `ValueOutOfLimits`. They work on
  numeric fields, bit fields, array elements (`of: { type: u16, max_raw: 1000 }`) and text tokens.
- **Derived length / count fields:** when `length:` / `count:` is just a field name, that field is written from the
  container size on encode. Don't set it by hand. The protocol's length header field and `message_type` field are
  filled in by `Protocol::encode` too.
- **`const` and `pad` fields have no struct member.** Access a const value as `Msg::<name>_value`.
- **Raw vs scaled:** a field with `scale` keeps its raw integer member; use `<name>_scaled()` / `set_<name>_scaled()`.
- **Name collisions:** field names that clash with C++ keywords or generated members get a `_` suffix (`data` → `data_`).
- **Views borrow the buffer.** In a stream handler the view points into the parser's buffer and dies after the call.
- **Escaped protocols unescape in place**, so their `parse` / `decode` take a mutable span (`gmp::mut_bytes`).
- **Expressions** (`when`, `length`, `count`, `computed`) may only reference earlier, non-conditional fields;
  enum constants are written `Mode.fault`. There is no ternary operator.
- **Lengths with escaping:** a length counts unescaped bytes by default. `counts: wire` (escaped bytes) is only
  allowed with `covers: body` or `body_checksum`, because a length that counts its own escaping cannot always be encoded.
- **Text protocols** (`format: text`): fields are tokens; view accessors parse on demand and return `gmp::Result<T>`;
  use `optional: true` for fields that may be empty, `width` / `precision` to control encoding.
- **Strict enums:** by default undeclared enum values decode fine (check with `is_known()`); set `strict: true` on the
  enum or `defaults.strict_enums: true` to reject them.
- **Per-frame I/O is the app's job.** GMP doesn't own a UART or socket; read bytes yourself and pass them to `feed()`.
  Handlers run synchronously inside `feed()`, so feeding from an ISR runs the checksum, the decode and your handlers
  inside the ISR. On an MCU, keep the ISR short instead:
  - The RX interrupt (or DMA callback) only appends bytes to a single-producer / single-consumer ring buffer. Use
    `std::atomic` head and tail indices, written by one side each, with a power-of-two size. GMP doesn't ship one yet.
  - The main loop or a task drains the ring into `parser.feed(...)` (or `push()` + `poll()`).

  Size the ring for bytes per second × the longest gap between drains.
- **Idle links and corrupted lengths:** a corrupted length can make the parser wait for bytes that never come, holding
  back good frames. Call `parser.set_idle_timeout(t)` once and `parser.tick(now)` from the main loop (any monotonic
  time unit), then `while (parser.next()) {}`; or call `resync()` / `flush()` yourself. With `length_prefixed`
  framing the scanner also rejects lengths the message type cannot have (`BadLength`) as soon as the header arrives.
- **Data after the message is ignored, not rejected** (a longer length, extra text tokens): it shows up as
  `view.extra_bytes()` / `extra_tokens()`, `Received::extra`, `stats().frames_with_extra`, `decode(bytes, extra)`. Set
  `exact_size: true` on the protocol to reject it with `BadLength` (recommended when the checksum doesn't cover the
  length).
- **Stored handlers:** `make_stream(h)` / `StreamWith<H>` keep a compile-time handler; `CallbackStream` keeps runtime
  listeners (one per message, plus `AnyView` for every frame and `gmp::FrameError`; registering again replaces).
  Contexts passed to `on<Fn>(ctx)` must outlive the registration. Messages and enums can't be named `Header`,
  `HeaderView`, `AnyView`, `AnyMessage`, `Frame`, `Stream`, `StreamWith`, `Callbacks`, `CallbackStream` or `Received` (the
  names declared inside each generated protocol struct).

## When modelling a new protocol

- **Only add a protocol for what's actually on the wire around the message.** If each read already gives you exactly
  one message (an I2C or SPI register read, a CAN payload, a USB HID report, a UDP datagram), there's nothing to
  frame. Define the message and decode it directly with `Msg::decode(bytes)` / `MsgView::from(bytes)`. A `fixed` or
  `none` protocol around it adds nothing. A protocol is for sync bytes, a header, a length, a frame checksum,
  delimiters, escaping, or choosing between several message types in one byte stream.
- One message per distinct payload layout. If a device answers different commands with differently shaped replies
  that carry no type id, decode the reply message for the command you sent. Only wrap each reply in its own
  protocol when you need stream parsing (e.g. the replies arrive on a UART).
- Reach for `bits` whenever a datasheet shows sub-byte fields; reach for `scale`/`offset` for "value = raw × k + c"
  formulas (store the exact factor, e.g. `175 / 65535` as `0.0026703288319218739`, and comment it).
- A checksum per value (Sensirion-style "CRC after every word") is a field `crc:`, not a protocol checksum.
- Values split across fields (serial number high / low words) → `computed:`.
- Sync bytes that must never appear in the payload → `escape:` with those bytes reserved.

## References

- `references/schema.md`: every schema key, field type, framing, escape and checksum option.
- `references/cpp-api.md`: generated structs / views / protocols, streaming, errors, CMake and gmpc options.
- `references/recipes.md`: complete, compiling schemas for common situations (framed serial link, NMEA text,
  CAN bit-packed signals, I2C sensor with per-word CRCs, reserved sync byte, custom codec) with usage snippets.
  Read the matching recipe before writing a schema from scratch.
