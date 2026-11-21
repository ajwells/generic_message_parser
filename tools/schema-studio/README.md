# GMP Schema Studio

A local web app for building GMP YAML schemas visually.

- **Left pane:** every schema option. This covers settings and defaults, messages and their fields (including bit
  fields), protocols (header, framing, checksum, message_type, carried messages), enums, and custom extensions.
- **Right pane:** a vertical, size-proportional picture of each message and protocol frame. It shows byte offsets, bit
  layouts, nested messages, conditional and variable-length fields, plus side bars marking checksum and length-field
  coverage. Click any block to edit that field.
- **YAML tab:** the generated schema, with Copy and Download.
- **Issues bar:** live validation that mirrors `gmpc`'s rules. Click an issue to jump to it.
- **Decode tester:** paste raw bytes (hex) or text and see them decoded live against the schema, like a regex
  tester for messages (see below).

## Decode tester

1. Pick what to decode as: a protocol (frames, checksum, message_type) or a single message (no framing).
2. Paste the input:
   - **Hex bytes:** spaces, commas and `0x` are optional, and `#` starts a comment.
   - **Text:** escapes `\r \n \t \xHH` are supported. The **Newlines** option maps line breaks to `\r\n`, `\n`, or
     nothing. It defaults to `\r\n` for protocols that end frames with CRLF, such as NMEA, so pasted logs just work.
3. Read the result:
   - The left column shows every input byte, coloured by the field that owns it and grouped per frame.
     - Frames are marked ✓ or ✗.
     - Skipped noise is drawn dashed.
     - Bytes belonging to a failing field are underlined in red.
   - The right column lists each frame's decoded fields:
     - enum names and scaled engineering values
     - bit fields with their bit positions
     - conditional fields, with the reason they are absent
     - the checksum (stored vs. computed), including per-field `crc` checksums
     - extra `conversions` next to the scaled value (e.g. `25 degC · 77 degF`)
     - the byte range of each field
   - Hovering a field highlights its bytes, and hovering a byte highlights its field.

The decoder (`js/decoder.js`) mirrors the C++ runtime: the same framers (fixed, length-prefixed and delimited, each with optional byte stuffing: XOR, index or
map escaping), checksums, message_type selection, and stream resynchronisation after bad frames. Frames encoded by the
generated C++ code decode to the same values.

Not executed in the browser:
- custom field codecs (shown as raw bytes; a custom type without `wire_size` stops decoding)
- custom checksum functions (shown as "not verified")
- custom framers (the whole input is treated as one frame)

Each "decode as" target remembers its own input. The example schema has sample input with valid, corrupted and
unknown frames (**Example input**). So do the Sensirion SHT4x schemas in `examples/sensirion_sht4x`: measurements and the
serial number with real per-word CRCs (including one corrupted frame), and each command byte. Any message made of
a single constant byte also gets that byte as its example.

## Running

Open `index.html` in a browser. No build step, server or install is needed. Work is autosaved in the browser
(`localStorage`).

- **Import…** loads an existing `.yaml`. This uses js-yaml from cdnjs, so it needs internet access. Everything else
  works offline.
- **Deep links:** `index.html?example&select=Link&field=id&tab=yaml` loads the example, selects an item, opens a field
  and picks a tab. `&target=protocol:Nmea&decode=<input>&format=text` preloads the decode tester.

Always confirm the final file with `gmpc --check schema.yaml`. The in-app checks cover the common rules, not all of
them.

## Code layout

| file | role |
|------|------|
| `js/state.js` | data model, constructors, example schema, persistence, byte-pattern helpers |
| `js/layout.js` | size / offset computation (mirrors `generator/src/ir/lowering.cpp`) |
| `js/decoder.js` | DOM-free decoder: framers, escaping, checksums, message_type, binary + text fields (mirrors `runtime/include/gmp`) |
| `js/decode-view.js` | decode tester tab rendering |
| `js/validate.js` | client-side diagnostics |
| `js/yaml.js` | YAML emitter (no dependency) and importer (js-yaml) |
| `js/editor.js` | left pane rendering |
| `js/viz.js` | right pane rendering |
| `js/app.js` | events, actions, tabs, import / export |

Inputs carry `data-path` attributes pointing into the state object, and `app.js` applies edits generically. To expose
a new schema option, add it to the constructor in `state.js`, a control in `editor.js`, and emit/import logic in
`yaml.js`.
