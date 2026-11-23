# GMP development session log

Read this first when starting a new working session. It records what exists, why it was built that way, what is
known to be rough, and what is planned. Update it at the end of each session (newest entries at the top of
"Session history").

Last updated: 2026-11-22.

---

## 1. Project in one paragraph

GMP (generic_message_parser) is a C++23 YAML-driven code generator for **existing** binary and text wire protocols.
`gmpc` reads a schema (`messages`, `enums`, `protocols`, `custom`) and writes one header `<schema>.gmp.hpp` with
eager structs, zero-copy Views, and Protocol types (`parse`, `decode`, `encode`, `Stream<>`); `--lang c` writes C99
(`runtime-c/`) and `--lang js` a JavaScript module (`runtime-js/gmsg.js`). The runtime
(`runtime/include/gmp/`) is header-only: no heap, no exceptions, no RTTI, `std::expected` errors. Schema Studio
(`tools/schema-studio/`) is a static web app for building schemas visually, decoding test bytes and learning
the features (`guide.html`). An AI-assistant skill lives in `skills/gmp/`.

## 2. Environment and commands (Windows, CLion toolchain)

- Toolchain PATH (Git Bash):
  `export PATH="/c/Users/Tabarious/AppData/Local/Programs/CLion/bin/mingw/bin:/c/Users/Tabarious/AppData/Local/Programs/CLion/bin/ninja/win/x64:/c/Users/Tabarious/AppData/Local/Programs/CLion/bin/cmake/win/x64/bin:$PATH"`
  (MinGW GCC 15, Ninja, CMake; yaml-cpp and Catch2 via FetchContent).
- Debug build + tests: `cmake --build build && ctest --test-dir build` (238 tests, all passing, including `gmp_studio_tests`, the C runtime tests in C11 / C99 / library builds, the generated-C cross-checks, the JS runtime and generated-JS cross-checks and `gmp_studio_gmpc_fresh`). Keep the MinGW `bin` on PATH when running ctest: without it the test executables fail with 0xc0000139.
- Node.js: none on PATH; CMake finds the copy inside emsdk (`~/emsdk/node/24.19.0_64bit/node.exe`, variable `GMP_NODE`). JS tests: `node tests/runtime_js/test_runtime.js build/tests/runtime_js/gmp_js_vectors.exe` and `node tests/roundtrip_js/test_generated.js build/tests/roundtrip_js/gmp_js_crossref.exe build/tests/roundtrip_js/gen`.
- emsdk is installed in `~/emsdk` (latest). It needs Python 3.10+: a portable Python 3.12 is in `~/tools/python312`, and emsdk brings Python 3.13 and Node. `py tools/schema-studio/gmpc/build.py` (with the CLion CMake + Ninja on PATH) builds `gmpc.wasm` in `build-wasm/` (about 20 s) and writes `tools/schema-studio/gmpc/gmpc.js`, `gmpc.stamp`, `gmpc-info.js`. Rebuild after any change under `generator/` (the freshness test fails otherwise).
- Release build: `cmake --build build-release` (Release, bench ON, tests and examples OFF; it is a compile check only).
- Regenerate golden files after an intentional generator change:
  `GMP_UPDATE_GOLDEN=1 ctest --test-dir build -R golden`, then check that only the expected files changed.
- Generator directly: `./build/generator/gmpc.exe --out <dir> schema.yaml` / `--check`.
- Examples run from `build/examples/*/example_*.exe` (6 programs).
- Quick standalone compiles need `-static-libgcc -static-libstdc++` (full `-static` fails to link with this MinGW;
  binaries without static libstdc++ fail with 0xc0000139 when the DLLs are not on PATH).
- Python is the `py` launcher (3.9, no PyYAML). Node.js only comes with emsdk (see above). Tools that need Python 3.10+ (e.g. the
  skill-creator `eval-viewer/generate_review.py`) work from a copy with `from __future__ import annotations` added
  at the top, run with `PYTHONIOENCODING=utf-8`.
- Studio tests are committed (`tools/schema-studio/tests/`, see its README). They run in ctest as `gmp_studio_tests`,
  or directly: `py tools/schema-studio/tests/run_tests.py --gmpc build/generator/gmpc.exe --crossref
  build/tests/roundtrip/gmp_studio_crossref.exe [--only unit crosscheck ui guide] [-v] [--offline]`. Run them after
  every Studio change.
- Ad-hoc Studio checks also use headless Edge:
  `"/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" --headless --disable-gpu --allow-file-access-from-files --user-data-dir=<tmp profile> --virtual-time-budget=15000 --dump-dom file:///<harness>.html`
  (`--screenshot=<png> --window-size=W,H` for visuals). Harness pages load the Studio JS files or embed
  `index.html` / `guide.html` in an iframe and print PASS/FAIL into a `<pre>`.
- Tooling quirk: in this environment, bash heredocs mangle backslash escapes (`\n`, `\\n`) and some quoting. For
  multi-line edits containing backslashes, write a Python script with the Write tool and run it with `py`, or use Edit.
- The directory is a **git repository** (branch `master`). `git diff -- tests/generator/golden` is the quickest way to
  check that a golden update only changed what was intended. Commit only when asked.

## 3. Repository map

| path | what |
|------|------|
| `runtime-c/include/gmp/c/` | C runtime (C99 or later; header-only, or the library build in `runtime-c/src/`): `config.h` (`GMSG_USE_LIBC`, `GMSG_FLOAT_TEXT`, `GMSG_RUNTIME_LIBRARY`, `GMSG_CRC_TABLES`, `GMSG_API`, `GMSG_ASSERT`, `GMSG_C11`, C11 wrappers with C99 fallbacks), `error.h`, `mem.h`, `endian.h`, `bits.h`, `reader.h` (reader + writer), `checksum.h` (+ generated `crc_tables.h`), `types.h`, `framing.h` (scanners, escaping), `stream.h` (`gmsg_stream`, pull support), `float_text.h` (exact float <-> text), `text.h` (tokenizer, integer parsing, text writer), `print.h` (`gmsg_printer`, same text as `gmp::print`), `runtime.h` (includes all). Names use `gmsg_` / `GMSG_`. Targets `gmp::runtime_c` (header-only), `gmp::runtime_c_lib` (compiled) |
| `runtime-c/src/`, `runtime-c/tools/` | library build of the C runtime: one `.c` per library header (checksum, framing, stream, float_text, text, print), each defining `GMSG_IMPL_<PART>_`; `tools/crc_tables.py` writes `crc_tables.h` |
| `runtime-js/gmsg.js` | JavaScript runtime (one UMD file, global `gmsg`): errors (`GmpError`, `Err`), Reader / Writer, bits, checksums (tables; BigInt for 64-bit), escaping, scanners, `StreamParser` (listeners, pull API, iterators, `offset`), exact float text (port of `float_text.h`), text tokens, `Printer` / `print`, `enumType`, helpers for generated code |
| `generator/src/backend/js/` | JS backend: `js_backend.cpp` (UMD wrapper, enums, `js:` checks, reserved names, options runtime / global), `js_common.cpp` (ident, literals, BigInt int64 expressions, print calls), `emit_binary_message.cpp`, `emit_text_message.cpp`, `emit_protocol.cpp` |
| `generator/src/wasm_main.cpp` | `gmpc_generate(yaml, file, lang, options)` → JSON for the WebAssembly build (target `gmpc_wasm`, only with Emscripten) |
| `runtime/include/gmp/` | header-only runtime: endian, byte/bit readers and writers, checksums, text codec, containers, framing (`framing/fixed.hpp`, `length_prefixed.hpp`, `delimited.hpp`, `escape.hpp`, `frame.hpp`), `stream_parser.hpp`, `error.hpp`, `print.hpp` |
| `generator/src/schema/` | `loader.cpp` (YAML → IR, key checks, renamed-key hints), `expr.cpp`, `validator.cpp` |
| `generator/src/ir/` | `ir.hpp` (language-neutral IR), `lowering.cpp` (resolution, sizes, offsets, escape tables, all semantic checks) |
| `generator/src/backend/c/` | C backend: `c_backend.cpp` (files, enums, unsupported-feature checks, global name registry), `c_common.cpp` (naming, types, expressions), `emit_binary_message.cpp` (bits, structs, views, print), `emit_protocol.cpp` (framing, parse / decode / process, callbacks, stream, poll, encode; binary and text), `emit_text_message.cpp` |
| `generator/src/backend/cpp/` | C++ emitters: `emit_binary_message.cpp`, `emit_text_message.cpp`, `emit_protocol.cpp`, `cpp_backend.cpp`, `cpp_common.cpp` |
| `cmake/GmpGenerate.cmake` | `gmp_generate(TARGET … SCHEMAS …)` |
| `tests/runtime_c` | C runtime: `test_c_runtime.c` (built as C11 and C99 with `GMSG_USE_LIBC` 1 and 0, with `GMSG_CRC_TABLES=1`, and against `gmp::runtime_c_lib`; checks every CRC preset against the bit-by-bit CRC, i.e. every table entry), `crosscheck_c.c` + `test_c_vs_cpp.cpp` (C vs C++ runtime on random inputs), `test_text_c_vs_cpp.cpp` (float text incl. `%g`-style digits, the printer against `gmp::Printer`); the cross-check also runs against the library build (C side C99, ctest names end in `[c99, runtime_c_lib]`), `cxx_compat.cpp` (C headers compiled as C++). Strict warnings with `-Werror` |
| `tests/roundtrip_c` | generated C for every supported schema compiled as strict C11 with `-Werror` (`gmp_test_schemas_c`), and the same files as C99 header-only (`gmp_test_schemas_c99`, compile check) and C99 with the library (`gmp_test_schemas_c99_lib`, cross-checked by `gmp_c_generated_tests_lib`), `test_c_generated.cpp` (messages: C vs C++ on valid, truncated, corrupted and random inputs: errors, fields, views, conversions, computed values, re-encoded bytes, print text via `print_compare.hpp`) and `test_c_protocols.cpp` (16 + 4 protocols: whole-buffer parse / decode / process / encode and noisy streams in several chunk sizes: events, header and message bytes, stats) |
| `tests/runtime_js` | `gmsg.js` tests under Node: `test_runtime.js` (unit checks + vectors from `js_vectors.cpp`: checksums, generic CRCs via the C runtime, bits, float text, int parsing, TextWriter, scanners, escaping, stream, printer), `harness.js` |
| `tests/roundtrip_js` | generated JS for every test schema (`gmp_generate(LANG js)` into `build/tests/roundtrip_js/gen`, with `test_codecs.js`) cross-checked against generated C++: `js_crossref.cpp` (messages: decode / print / re-encode / view on seeds, mutations, truncations; protocols: parse / decode / encode and noisy streams through the real `Stream<>`) and `test_generated.js` |
| `tests/runtime`, `tests/generator` (golden + diagnostics), `tests/roundtrip`, `tests/schemas` | test suites; goldens in `tests/generator/golden/<backend>/` (`cpp/`, `c/`, `js/`) |
| `examples/` | binary_telemetry, nmea_text, bitpacked_can, custom_codec, sensirion_sht4x (two schemas), reserved_sync; C versions (`main.c`, `gmp_add_example(... LANG c)`) of binary_telemetry (callbacks), nmea_text (push + poll_event) and custom_codec (`c:` codecs, print hook) |
| `docs/schema.md` | compact schema reference (source for the skill's `references/schema.md`) |
| `tools/schema-studio/` | Studio (`index.html`) and Guide (`guide.html`); JS in `js/` (see its README for the file roles; `exact.js` is the gmpc engine); `gmpc/` (committed `gmpc.js` = gmpc in WebAssembly, `build.py`); tests in `tests/` (`run_tests.py`, pages `unit` / `crosscheck` / `ui` / `guide`, C++ reference generator `crossref.cpp`) |
| `skills/gmp/` | AI skill: `SKILL.md`, `references/` (generated by `build_references.py`), `evals/` (7 test cases for the skill-creator workflow) |
| `skills/gmp-workspace/` | skill-eval results (`iteration-1/`), reference schemas and verified captures (`reference/`), `auto_checks.py` / `write_grades.py`. Not part of the skill; ignored by `.gitignore` |

## 4. Feature inventory (what exists today)

Schema / generator:
- Scalars, enums (with `strict`, `is_known()`, `to_string()`), `bits` groups (msb/lsb first, enums, consts, scale),
  bytes/strings/arrays (fixed, expression, `remaining`, `terminator`), nested messages, `pad`, `const`, `when`
  conditions, custom types / checksums / framers (concept-checked).
- `scale` / `offset` / `unit`, extra `conversions` (e.g. `temperature_degf()`), field-level `crc:` (checksum after
  one value), `computed:` values with automatic or explicit setters (`Expr::Kind::Input` for `value`).
- Text messages (tokens, `optional`, `width`, `precision`, `base`), NMEA-style text checksums.
- Protocols: framing `none` / `fixed` / `length_prefixed` / `delimited` / `custom`; checksum `covers`;
  `message_type` by header field or first token (renamed from dispatch/id: see decisions).
- Length coverage: `body`, `body_checksum`, `header_body`, `all`; `adjust`.
- Stored stream handlers. Per protocol: `StreamWith<H, Cap>` / `make_stream(h)` (compile-time handler), plus
  `Callbacks` and `CallbackStream<Cap>` (runtime listeners, `on<Fn>(ctx)`). Names declared inside protocol structs
  (`Header`, `HeaderView`, `AnyView`, `AnyMessage`, `Frame`, `Stream`, `StreamWith`, `Callbacks`, `CallbackStream`,
  `Received`) are reserved for messages and enums; this is checked by the C++ backend.
- Byte stuffing `escape:` on delimited, fixed and length_prefixed framing: styles `xor` / `index` (with `base`) / `map`,
  up to 16 reserved bytes, `checksum.over: wire`, `length.counts: wire` (only with `covers: body | body_checksum`, and
  `body_checksum` then needs `checksum.covers: body`).
- Strict schema loading: unknown keys are errors, renamed keys report their new name.
- `print_to(gmp::Printer&)` on every struct, view and bits group (binary and text). Same text from a struct and its view.

Runtime:
- `StreamParser<Protocol, Cap, Handler = void>` with resync, stats, `feed` / `push` + `next`. A non-void Handler
  (a value, or a reference such as `Callbacks&`) is stored, and then `feed(bytes)` / `next()` take no handler.
- Pull API on `StreamParser`:
  - `poll()` returns `Polled<AnyView>`
  - `poll_frame()` returns `Polled<Received>`, with header (if any), message and bytes
  - `poll_message()` returns `optional<AnyMessage>`
  - `poll_event()` returns `Polled<variant<AnyView, FrameError>>`
  - `poll_for<V>()` exists only with a stored handler: other messages go to it
  - `last_error()`

  Errors go to the stored handler's FrameError overload, except in `poll_event`, which returns them inline. Message
  listeners are never called by `poll`. `next()` and `poll*` share one `step()`. A frame stays held until the next
  call that changes the parser, and each release bumps a generation counter. `gmp/polled.hpp` `Polled<T>` (optional-like,
  `valid()`) asserts in Debug builds that a view is still valid, via an overridable `GMP_ASSERT`.
- `gmp/callbacks.hpp`: `CallbackSlot<Args...>` (function pointer + context + trampoline, no heap) and
  `callback_traits` / `callback_message_t`. A bound function may take all the slot's arguments or only the last one,
  so the header is passed only when asked for.
- Table-driven `EscapeConfig`
  (`make_xor_escape` / `make_index_escape` / `make_map_escape`, `walk_escaped`, `escape_overhead`).
- CRC presets incl. `crc8_nrsc5`; inline CRC parameters.
- `gmp/print.hpp`: `gmp::print` / `gmp::println(value, sink, PrintOptions)`. Multi-line (default) or single-line
  output. Enums print as `name (0xNN)`, or `<unknown> (0xNN)` for undeclared values. Scaled values print as
  `raw (value unit, ...)`. Bytes print as hex, strings are escaped, and missing optional fields print `<absent>`.
  Text parse failures print `<error: ...>`, computed values are marked `(computed)`, and custom types use an ADL
  hook `gmp_print(Printer&, const T&)` or print `<?>`.
  - Sinks: `FILE*`, `char[N]` / `std::span<char>` (NUL terminated), anything with `append(string_view)`
    (`static_string` gained `append`; `std::string`), anything with `write(const char*, n)` (`std::ostream`), and a
    callable taking `string_view` (it may return `false` to stop).
  - Bounded sinks return `BufferFull`. It never allocates (checked in `embedded_profile`).

Studio (`tools/schema-studio`):
- Editor for every option, live layout cards, YAML export/import (import uses js-yaml from cdnjs), validation
  mirroring gmpc, decode tester mirroring the runtime (including escaping and wire-count lengths).
- Hover tooltips for every editor control (`js/tips.js`, keyed by normalised `data-path`).
- Guide page `guide.html` (`js/guide.js`): 15 sections (incl. "Printing for logs" and "API reference": the `API`
  table in guide.js, rows `[C++ signature, C signature, description]`, grouped by task, with a filter box and jump links), 10 live demos built from real
  schemas, "Open in Studio". Demo runs accept `view: 'print' | 'line'`. A **C++ / C** switch in the top bar
  (`ln(cpp, c)` in guide.js; `?lang=c`; remembered in localStorage `gmp-guide-lang`) swaps code samples, API names,
  error names and the "Generated C++ / C and errors" section; schemas and demos are shared.
- Custom types / checksums / framers have a **C binding** section (the `c:` block: header, type, decode, encode,
  print / function / scan) next to the C++ binding. Validation wants a binding for at least one language; YAML
  export omits an empty `cpp:` block. `G.setPath` creates missing intermediate objects (`lang.c`).
- Example inputs for the decode tester (SHT4x, NMEA, Link, any escaped protocol).
- Decode tester **View** switch: Fields / Print / One line. `js/print.js` mirrors `gmp::print` exactly; the decoder
  now keeps raw values on its nodes (`v`, `field`, `bit`, `content`, `absent`, `msg`, `top`) for it.

AI skill: `skills/gmp/` (see section 7).

JavaScript backend (`gmpc --lang js`): one UMD module `<schema>.gmp.js` on `runtime-js/gmsg.js`; options
`--opt runtime=<module id>` (default `./gmsg.js`), `--opt global=<name>` (default namespace with `::` → `_`).
- Same functionality as C++, idiomatic JS: thrown `gmsg.GmpError`, Numbers (BigInt for u64/i64), char = 1-char
  string, strings latin1, bytes `Uint8Array`, absent = `undefined`, enums `gmsg.enumType` (frozen, `nameOf`,
  `isKnown`), classes with static `decode/read/write/encode` + `m.encode()` (plain objects encode too), views with
  getters over the bytes (private `#b/#o/#p`) + `ranges()` (field byte ranges, keys) and text views with
  `static fields`; conversions / computed as getters + setters; protocols as classes of statics (`scan`, `open`,
  `select`, `process`, `parse`, `decode`, `encode(m, header)`, `stream()`, `views`, `messages`, layout constants).
- Encode also checks what C++ types guarantee: values fit their type, containers within `max` / exact fixed size
  (OutOfRange). Expressions are int64 BigInt (`gmsg.toInt`, `idiv`, `imod`).
- Custom items: `js: { module, decode, encode, print }` / `{ module, function }` / `{ module, scan }`; modules
  `require`d as written, browser global `gmsgExtensions[module]`. Without a print hook custom values print with
  `p.value(v)` (numbers / strings as such, else `<?>`).
- Stream parser: listeners `on(name | class | 'frame' | 'error', fn)` returning `off`; pull API `poll`, `pollFrame`,
  `pollMessage`, `pollEvent`, `pollFor`, iterators `messages(chunk)` / `events(chunk)`; events carry `offset`
  (stream position); polled views own a copy of the frame.
- `gmp_generate(... LANG js)` is a custom target that also copies `gmsg.js` into the output directory.

Studio engines: the decode tester has **Engine: Quick | gmpc** (`dui.engine`, URL `engine=gmpc`). Quick = `decoder.js`
on `gmsg.js` (framers / escaping / checksums / text parsing / print from the runtime; the schema walk stays in Studio).
gmpc = `js/exact.js`: loads `gmpc/gmpc.js` on demand (availability from `gmpc/gmpc-info.js`), generates JS, evaluates it
with a `require` shim (custom modules are stubs that throw `Custom`), decodes with `StreamParser` / views, builds nodes
from `ranges()` (escaped frames mapped through an origin map), prints with the generated `printTo`, and compares with
Quick (warning callout). With gmpc loaded, gmpc diagnostics join the Issues bar. **Code** tab: generated C++ / C / JS.
Custom item editors have a JavaScript binding section (`js:`); validation accepts any one language. Guide: language
switch C++ / C / JavaScript (`ln(cpp, c, js)`; API rows get JS signatures from `API_JS`, keyed by the C++ signature).

C backend (`gmpc --lang c`): `<schema>.gmp.h` + `<schema>.gmp.c`, C99 or later, header compiles as C++. The
`_Generic` signature checks of `c:` functions are behind `#if GMSG_C11`. `gmp_generate(... C_RUNTIME library)` links
`gmp::runtime_c_lib`.
- Supported: everything the C++ backend generates. Enums and binary messages (structs, views, bits, const, pad,
  when, bytes/strings/arrays, nested, field crc incl. inline params, scale/conversions, computed with setters),
  text messages, protocols, custom `c:` extensions, print.
- Print: `<t>_print(const t*, gmsg_printer*)` for structs, views, bits groups (defined in the .c), protocol
  `_any_view_print` / `_any_message_print` (kind 0 prints `<none>`). Custom types: `c: { print: fn }`
  (`void fn(gmsg_printer*, const T*)`, checked with `_Static_assert`), else a built-in printer when the C type is
  a standard scalar (`double`, `uint16_t`, ... as C++ prints them), else `<?>`. Views print `<error: ...>` for
  custom / text fields that fail to decode.
- Names: prefix from `--opt prefix=`, else namespace (`a::b` -> `a_b_`), else the file stem; types snake_case
  (`acme_telemetry_status`), macros upper case. Variable containers get `<f>_len` / `<f>_count`, `when` fields
  `has_<f>`. Generated names are checked for clashes (per message and globally).
- Small functions are `static inline` in the header; read/write/decode/encode and view validation are in the .c.
- Runtime additions: `gmp/c/types.h` (`gmsg_bytes`, `gmsg_str`, `gmsg_str_strip`, `gmsg_round_i64` = llround).
- Sizes: a length / count given by a bare field reference (`length: n`) uses that field as the container size (no
  `<f>_len` / `<f>_count` member); other variable containers get `<f>_len` (bytes, strings) / `<f>_count` (arrays).
- Custom items: `c:` blocks (types: header/type/decode/encode, checksums: function, framers: scan), required only
  for items the C output uses; signatures checked with `_Static_assert` + `_Generic`.
- Text messages: struct with `has_<f>` for optional tokens, `_read_text` / `_write_text`; view tokenises once and
  parses on demand (`gmsg_error` + out parameter). Float text: `GMSG_FLOAT_TEXT` builtin (exact, matches
  to_chars / from_chars), libc, none (`#error` in the generated header when a schema needs it).
- Poll API on streams: `_poll`, `_poll_frame`, `_poll_message`, `_poll_event`, `_poll_for(kind)`,
  `_stream_generation()`.
- Protocols: per protocol `_parse` / `_decode` / `_process` / `_encode_<m>` /
  `_encode_<m>_with_header` / `_encode`, tagged unions `_any_view` / `_any_message` (kind 1..N, 0 none), `_callbacks`
  (one `void* ctx`, `on_<m>(ctx, header_view*, view*)`, `on_frame`, `on_error`), `_stream` (`gmsg_stream base` + `cb`,
  caller buffer >= `_MAX_FRAME`). Escaped protocols take non-const buffers.

C runtime (`runtime-c/`, for the C backend):
- Prefix `gmsg_` / `GMSG_` (not `gmp_`, which GNU MP uses). Umbrella header `gmp/c/runtime.h`.
- C99 or later (`GMSG_C11` 1 for C11 / C++; `GMSG_STATIC_ASSERT` falls back to a negative-size array). Header-only
  by default; `GMSG_RUNTIME_LIBRARY=1` (target `gmp::runtime_c_lib`) makes the large headers (checksum, framing,
  stream, float_text, text, print) declaration-only: public types and prototypes first, bodies under
  `#if GMSG_DEFINE_FUNCTIONS_ || defined(GMSG_IMPL_<PART>_)`, compiled by `runtime-c/src/<part>.c`. Public functions
  use `GMSG_API` (`static inline` / `extern`), internal helpers `GMSG_PRIV_`, always-inline small headers (endian,
  bits, mem, reader, types, error) `GMSG_INLINE`. `GMSG_CRC_TABLES` (default = `GMSG_RUNTIME_LIBRARY`): presets use
  256-entry tables (4-8x faster on x86 -O2); the generic `gmsg_crc_*` stays bit by bit. `GMSG_USE_LIBC` (default 1) picks `<string.h>`/`<assert.h>` or built-in loops; without it only
  `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` are used. `GMSG_FLOAT_TEXT` (builtin default / libc / none) selects the
  float text used by text messages and print (`gmsg_format_general` for print's significant digits).
- `print.h`: `gmsg_printer` (write function + ctx, options as `gmp::PrintOptions`), sinks `_init_buffer` (NUL
  terminated), `_init_file` (with libc), `_init`; `gmsg_print_begin/end/key/absent/unit/note/conversion/text`, values
  `_u64/_i64/_bool/_char/_str/_hex/_enum/_f32/_f64/_error`, lists `gmsg_print_list_begin/item/end`;
  `gmsg_printer_finish(p, newline, &n)` returns `GMSG_ERR_BUFFER_FULL` when cut.
- `gmsg_error` / `GMSG_OK` / `GMSG_ERR_*` (same values and texts as `gmp::Error`), `gmsg_mem*`, `gmsg_load_<t>` / `gmsg_store_<t>` (+ `_be` / `_le`,
  `gmsg_load_uint`), `gmsg_get_bits` / `gmsg_set_bits` (+ signed), `gmsg_reader` / `gmsg_writer` with typed
  `gmsg_read_<t>` / `gmsg_write_<t>` returning `gmsg_error`, generic bit-by-bit CRC `gmsg_crc_params` and all presets and
  simple checksums as `gmsg_NAME_init/update/finish/compute`.

## 5. Design decisions (and why)

- **No heap / exceptions / RTTI** in runtime and generated code, so it runs on MCUs; containers are
  `gmp::static_vector` / `static_string` sized by `max`. `max` therefore means capacity, not a value limit.
- **`message_type` / `type_id`** replaced the old `dispatch` / `id` names everywhere (YAML, generated C++
  `select_message()`, `Frame::type_id`, `Error::UnknownType`, IR, Studio state). The deprecated aliases and the
  Studio saved-state migration were removed on request; the loader still explains renamed keys in its diagnostics.
- **Per-field CRC** is a built-in field feature (`crc:`), not a custom codec, because sensors like the SHT4x put a
  CRC after every word.
- **One protocol per response layout** when responses carry no type id (SHT4x), and the app picks by the command sent.
- **Computed values** cover "value split across fields" (serial high/low) with automatic setters for
  shift / or / plus packing of non-overlapping unsigned fields.
- **Escaping generalised** from delimited-only to all length-based framings with an explicit `reserved` list, so a
  reserved sync byte gives instant resync (a raw reserved byte inside a frame means truncation → `BadDelimiter`).
- **`length.counts: escaped` for whole frames was rejected**: a length that counts its own escaping (or a checksum
  over it) has no solution for some values (e.g. 0x81 → needs 0x82 → 0x82 doesn't escape). Only body /
  body+checksum wire counts are allowed, with a body-only checksum.
- **Studio is dependency-free static HTML/JS** (no build step); it mirrors generator/runtime logic in JS, and
  round-trip tests check that Studio-exported YAML generates byte-identical C++.
- **Guide examples are real schemas** rendered by the same code as Studio, so docs cannot drift from behaviour.
- **Printing is a runtime `Printer` driven by generated `print_to`**. The type-erased sink (function pointer and
  context) keeps `print_to` a plain non-template member, so generated code stays small. Formatting lives in one
  place (the runtime), and the free `gmp::print` template picks the sink adapter. Field names print as written in
  the schema, not as the C++ identifier. `const` and `pad` fields are not printed, which matches the struct members.
  Floats print as the shortest text of their own type, so an f32 is not widened to double first.
- **Stored handlers use a combined design.** `StreamParser` stores any handler (Option 1, zero cost). The generated
  `Callbacks` is just one concrete handler type with runtime slots (Option 2), so there is a single dispatch path.
  `CallbackStream` privately inherits `Callbacks` and passes itself to the parser as `Callbacks&`.
  - **Slot choice:** taken from the function's last parameter, so there are no per-message setter names that could
    collide, and wrong types fail at registration with a clear `static_assert`.
  - **Header (H3):** the slot always receives `(HeaderView, View)` and its adapter drops the header when the function
    takes only `(View)`. Headerless and text protocols only have `(View)`.
  - **Order:** the message listener runs first, then `AnyView` (every frame). There is one listener per slot;
    registering again replaces it, and users forward to more if needed.
- **One JavaScript runtime, two Studio engines** (decided with the user 2026-11-22): `runtime-js/gmsg.js` serves the
  `js` backend and Studio. Studio keeps a schema interpreter (Quick, works while editing) and runs the real generator
  (gmpc compiled to WebAssembly, committed so Studio needs no build) for exact results; Quick is the default.
  Goal from the user: the same functionality in C++, C and JavaScript, each written the way that language is normally
  written, without forcing constructs a language doesn't support (no Result type, no capacity-bound containers, no
  callback context pointers in JS).
- **The AI skill bundles its references** (generated from docs/README/examples by a script) so it works when copied
  outside the repo.

## 6. Known issues and gaps

- The Studio suite does not cover every interactive edit, e.g. the drag splitter, renaming a type and its references,
  or adding and removing fields through the UI. Older scratchpad tests for those (position, rename, escape UI) were
  not recreated.
- **Corrupted length below max_frame** delays good frames on an idle link (no timeout / per-type size check). See
  future task "bad length handling".
- Studio import and the Guide's live demos need internet for js-yaml (cdnjs); the rest works offline.
- The Studio cross-check (`crossref.cpp`) does not include `tests/schemas/framing.yaml` yet, so the Studio decoder is not compared with C++ for length `adjust`, little-endian checksums or `end` after escaped fixed / length-prefixed frames (the YAML round trip is).
- `build-release` does not build tests or examples; run tests in `build`.
- C runtime library build: a different `GMSG_` option value in `gmp::runtime_c_lib` and in the code using it
  (e.g. `GMSG_USE_LIBC`, `GMSG_FLOAT_TEXT`) is not detected; set options on the library target as PUBLIC definitions.
- C99 builds of generated code skip the `_Generic` signature checks of `c:` functions (calls are still type checked).
- Text protocols with escaping are allowed only for delimited framing; `checksum.over: wire` is binary-only.
- **Escaped frames that fail (C++ / C vs JS):** C++ and C restore a failed frame by escaping it again, so with `xor`
  escaping a non-canonical escape pair (escape + code of a byte that needs no escaping) comes back as the plain byte;
  JS keeps the bytes exactly. Resync is the same; only `FrameError::data` contents can differ.
- C `_poll_message` restores a frame whose struct decode fails after the view decoded, but no test reaches that path
  for an escaped protocol (the C++ encoder cannot build such a frame); a planted bug there is not caught.
- JS cannot keep the payload of f32 signalling NaNs (they become quiet NaNs), so such values re-encode differently.
  Accepted by the user (2026-11-22); revisit only if a protocol needs bit-exact NaNs in JS.
- The gmpc engine cannot run custom codecs (user code); frames using them fail with `Custom` in that engine.

## 7. Future tasks (planned, not started)

| task | notes |
|------|-------|
| GMP skill evals, iteration 2 | Run all 7 cases in `skills/gmp/evals/` (4 new, harder ones), with the skill and with a baseline; runs must not read `skills/gmp/evals`. See memory note `skill-eval-todo`. |
| Warn on `max` for scalars | `gmpc` silently ignores `max:` on a scalar field (users mistake it for a value limit). Make it a warning or error; part of the value-range work. |
| ASCII numeric fields in binary messages | `{ type: u16, encoding: ascii, size: 4 }`, usable as length/count sources |
| Field value ranges | min/max *value* limits (not `max`, which is capacity): reject or clamp, raw or scaled, encode too |
| Bad length handling | per-message size check from the type id; `flush_partial()` for idle links; optional header checksum. Also: a frame whose length field is *longer* than its message is accepted and the extra bytes are ignored (found by a skill-eval run). Decide whether to offer an exact-size check. |
| Stream I/O + callbacks | decided: do all four, one at a time. **A** stored handler / callbacks: **done**. **C** ISR-safe ring buffer: design presented, **tabled** (open questions C1–C6 in the memory note). **B** pull API: **done**. Remaining: **C** (when resumed) and **D**, an endpoint with source/sink adapters, `send(msg)` and a blocking `wait_for<V>(timeout)` / `transact`. |
| `gmp::to_text<N>(msg)` (small, optional) | printing helper returning a fresh `gmp::static_string<N>` instead of appending to a sink |
| Rename C++ runtime macros/namespace? | The C runtime uses `gmsg_` / `GMSG_` to avoid GNU MP (`gmp_`, `GMP_`). Consider whether the C++ runtime should match: `namespace gmp` and `GMP_ASSERT` don't clash with GNU MP today (its C++ wrapper `gmpxx.h` has no `gmp` namespace), but the names are confusable and a future `GMP_*` macro could clash. Would touch the runtime, generated code, goldens, docs, Studio text and the skill. Ask the user. |
| JS follow-ups (suggested, not requested yet) | A JS example program (`examples/*/main.js`, run as a test like the C examples), TypeScript declarations (`.d.ts`) for gmsg.js and generated modules (possibly a gmpc output), an npm package layout for the runtime (package.json, ES module entry besides UMD). Memory note `studio-js-runtime-todo`. |

Each planned task has a detailed note (with open questions to ask the user) in the Claude memory directory for this
project.

## 8. Session history

### 2026-11-22 (JS findings decided: checksum-first unescape, unbounded float tokens, f32 NaN accepted)
- Decisions (user): 1. C++ / C unescape only after the checksum passes; 2. enlarge the C++ float token buffer as
  needed; 3. accept JS f32 NaN payload loss and document it (kept as a watch item in memory `js-findings-todo`).
- 1: runtime `gmp::check_escaped` / `gmp::for_each_unescaped` (C: `gmsg_check_escaped`, `gmsg_unescape_iter`).
  Escaped binary frames verify a built-in checksum (and `over: wire`) over the escaped bytes before unescaping.
  Failures that come later (custom checksum functions, header, unknown type, body, struct decode in
  `poll_message`) restore the frame with `restore_frame` / `_restore_` (re-escaping in place), so resync always
  scans the bytes as received. (The restore goes beyond the user's choice: checksum-first alone would leave those
  cases destructive.)
  C++: `Frame::unescaped_size`, `restore_frame`, `StreamParser::open_view` (all `poll*`). C: `_split_`,
  `_restore_`, `_select_restore_`.
- Tests: `js_crossref.cpp` compares JS with the real `Stream<>` (NonDestructive wrapper removed).
  `test_c_protocols.cpp` checks that C and C++ leave the same bytes and that a failed frame is left as received;
  the random sample messages now favour reserved bytes. Before that, the C mutants were not caught. 7 of 8 planted
  bugs caught (restore in C++ / C / poll helper, checksum range, iterator decode, dropped run, code check). Not
  caught: the C `poll_message` restore (section 6).
- 2: C++ `TextWriter::float_token` and C `gmsg_text_float_` format straight into the output buffer (no 128 /
  1100-character temporary). New test "C and C++ text writers write huge fixed-notation floats"; the JS vectors no
  longer skip failing tokens (buffer 1024, failure is fatal). The old C++ code fails both.
- Goldens: only the escaped schemas changed (C++ and C; JS unchanged). gmpc.js rebuilt. README (escaping rules, JS
  known differences, `poll_frame`), docs/schema.md, skill references.
- End state: 238 ctest pass, Studio suite passes, Release build compiles. Not committed.

### 2026-11-22 (JavaScript GMP: runtime, js backend, Studio engines)
- Decisions (user): keep functionality the same across C++ / C / JS but idiomatic per language; Studio gets both a
  Quick engine (no gmpc) and a gmpc engine (gmpc.wasm, committed), Quick by default; plain JS + JSDoc, UMD; API
  direction (throw GmpError, closures, iterators, BigInt for 64-bit only). Work all phases.
- Phase 1: `runtime-js/gmsg.js`; `tests/runtime_js` cross-check with the C++ runtime (~82k checks); mutants caught
  (one equivalent until a push-compaction test was added).
- Phase 2: Studio decoder / printer on gmsg.js (checksums, scanners with the generator's configs, escaping, text
  parsing, Printer); `framing.yaml` added to the Studio cross-check (a planted framing bug is caught only by it).
- Phase 3: `generator/src/backend/js`, goldens `golden/js`, diagnostics test, `tests/roundtrip_js` (60 suites,
  ~20.5k checks vs C++; endian / strict enum / crc / derived count / precision / escape mutants caught). `js:` blocks
  in `hdlc.yaml` / `custom.yaml` (C++ / C goldens unchanged). Found the in-place unescape divergence (section 6).
- Phase 4: emsdk installed; `wasm_main.cpp`, `gmpc_wasm` target, `tools/schema-studio/gmpc/build.py` (+ `--check`,
  ctest `gmp_studio_gmpc_fresh`); output identical to the goldens; `js/exact.js`, engine switch, compare callout,
  gmpc issues, Code tab; Studio tests: gmpc engine on every crossref case (Quick and gmpc agree), UI test.
- Phase 5: Studio JavaScript binding editor + tooltips + validation; Guide JavaScript language (code samples,
  streamingJs / generatedJs, API reference JS column, error names) + test; README "JavaScript output", custom `js:`
  table, CMake / CLI / layout; docs/schema.md; Studio READMEs; SKILL.md + references.
- End state: 236 ctest pass, Studio 191 checks pass, Release build compiles. Committed later as `f06d5db`.
- The user deferred the open decisions (in-place unescape in C++/C streams, C++ `float_token` buffer, f32 NaN
  payloads in JS) to a later session; see section 7 and memory note `js-findings-todo`.

### 2026-11-22 (Studio guide: API reference; end of session)
- New guide section 13 "API reference" (`apiRef()` / `API` / `filterApi()` in `js/guide.js`, styles `.g-api*`): every
  generated and runtime function grouped by task (decode, views, field access, encode, text messages, enums and bit
  groups, protocols, streaming, pull API, printing, sizes and constants, errors, checksums) for C++ and for C, names
  checked against the goldens. Filter box (kept across the language switch) and jump links. New `tests/guide.html`
  case (groups, key names per language, no cross-language names, filter, no-match message); 151 Studio checks.
- End state: 231 ctest entries pass; Studio 151 checks pass; committed as `cf1fe08` (C support) and
  `91ae979` (API reference). Next candidates are in
  section 7 (stream I/O step C / D, bad length handling, value ranges, skill evals iteration 2, C++ renaming question).

### 2026-11-22 (C99 mode, compiled C runtime library with CRC tables)
- C99: `config.h` accepts C99 (`#error` only below C99), `GMSG_C11`, C99 fallbacks for `GMSG_STATIC_ASSERT` (extern
  array of size -1) and `GMSG_ALIGNOF` (offsetof). Generated `.gmp.c` wraps the `_Generic` checks in `#if GMSG_C11`;
  header comments say "C99 or later". C goldens updated; C++ goldens unchanged.
- Library build: the six large headers were split (public types + prototypes, then the bodies behind
  `GMSG_DEFINE_FUNCTIONS_ || GMSG_IMPL_<PART>_`); `runtime-c/src/*.c` (one per header, so a static library links only
  what is used; built with `-ffunction-sections -fdata-sections`); CMake `gmp::runtime_c_lib` (defined when C is
  enabled; the root CMake now enables C before the runtime targets) with PUBLIC `GMSG_RUNTIME_LIBRARY=1`;
  `gmp_generate(C_RUNTIME header|library)`; `gmp_add_test(... TEST_SUFFIX)` for building the same Catch2 sources twice.
- CRC tables: `runtime-c/tools/crc_tables.py` -> `gmp/c/crc_tables.h` (13 tables, shared by presets with the same
  width / poly / refin; each a function-local static so unused tables cost nothing). Test checks every preset against
  the bit-by-bit CRC for every byte from 5 start registers and 50 random split inputs; planted bugs (one table entry,
  the shift of the normal update) were caught.
- New ctest entries: runtime unit tests C99 libc 1/0, CRC tables, library; `gmp_c_vs_cpp_tests_lib` and
  `gmp_c_generated_tests_lib` (C99 + library); `gmp_test_schemas_c99` compiles the generated C as C99 (188 -> 231).
- Studio guide (C): C99 / C11, "Header-only or compiled runtime", a paragraph on `C_RUNTIME library` (guide test
  checks it). README: C output says C99 or later, new "C runtime: header-only or library" section, options,
  `gmp_generate` `C_RUNTIME`, layout; skill references regenerated.

### 2026-11-22 (C backend: print, C examples, Studio support for C)
- Print: runtime `gmp/c/print.h` (port of `gmp::Printer`) and `gmsg_format_general` in `float_text.h` (exact
  `%.*g`: all digits of the exact value, half-even rounding, the printf fixed / scientific rule); generated `_print`
  for structs, views, bits groups, text messages and protocol unions; `c: { print: fn }` hook for custom types
  (`hdlc.yaml` uses `test_time_print`).
- Checks: `print_compare.hpp` compares the C text with `gmp::print` (5 option sets + a 23-byte buffer) for every
  decoded struct / view in the three generated-C suites; `test_text_c_vs_cpp.cpp` adds `%g` digits against
  `to_chars(general, p)` (~400k values) and a printer scenario against `gmp::Printer` (every value kind, 360 option
  combinations, every buffer size). Planted bugs (separator, max_bytes, precision, enum width, escapes, truncation,
  empty block, tie rounding, optional text, computed unit, custom hook) are all caught; the empty-block bug was only
  caught after adding the runtime scenario (no schema prints an empty block). `>=` vs `>` for max_items is an
  equivalent mutant.
- C examples: `main.c` in binary_telemetry, nmea_text (pull style) and custom_codec (`meter.yaml` got `c:` blocks,
  `meter_codecs.h`); same output as the C++ programs; clean with the strict C warning set. The root CMake enables C
  for examples too.
- Studio: C binding editors for custom items, tooltips, validation (C-only items are valid), no empty `cpp:` in
  YAML, `setPath` creates `lang.c`; Guide C++ / C switch. New tests in `ui.html` and `guide.html` (150 checks);
  planted bugs in `setPath` and a C guide sample were caught.
- Docs: README "C output" (no longer "preview"; print, examples), custom-extension tables (`print` row) in README and
  docs/schema.md, Studio README; skill references regenerated.

### 2026-11-21 (C backend: custom extensions, poll API, text messages and float text)
- Decisions: order 1 (custom `c:` blocks) -> 4 (poll) -> 2 (text + exact float text); see the memory note.
- Custom extensions: `c:` blocks for types / checksums / framers, compile-time signature checks, C test codecs in
  `tests/roundtrip_c/test_codecs_c.h`; `hdlc.yaml` / `custom.yaml` got `c:` blocks and are in the C goldens and
  cross-checks (Batch with a BCD custom type, Hdlc / Summed with a custom checksum, a custom framer).
- Found: hdlc's `name_len` length field clashed with the C member `name_len`. Changed the C structs: a bare length
  field is the container size (no extra member). Same encoded bytes as C++ (C++ derives the field from the size).
- Found: Studio dropped `c:` (all non-cpp) language blocks on YAML import/export. Fixed (`yaml.js`, `state.js` keep
  them in `lang`); the Studio round trip now also compares `gmpc --lang c` output (it failed on the old Studio code).
- Poll API: runtime `gmsg_stream_step` / `gmsg_stream_pull` / generation; generated poll functions with a capture
  mode in the stream's process hook. Cross-checked against C++ for all 5 poll functions on every streamed protocol.
- Float text: `gmp/c/float_text.h` (Burger-Dybvig shortest with the to_chars fixed/scientific choice, exact fixed
  precision, exact parsing). Found by the cross-check: libstdc++ prints the exact integer for fixed-form values whose
  neighbours are 2+ apart (e.g. float 441382624), and reports out of range before trailing characters. About 13M
  assertions against to_chars / from_chars / parse_int / TextWriter (`test_text_c_vs_cpp.cpp`).
- Text messages and text protocols in C (`emit_text_message.cpp`, text branch of `emit_protocol.cpp`), cross-checked
  in `test_c_text.cpp` and `test_c_protocols.cpp` (Nmea, TextPos). Planted bugs (width, hex case, empty tokens,
  poll_for delivery, poll_event errors) were all caught.
- A third C runtime test build uses `GMSG_FLOAT_TEXT_LIBC`.

### 2026-11-21 (C backend: protocols)
- Design agreed with the user: caller-provided stream buffer, one `void* ctx` per callbacks struct, header view
  always passed when the protocol has a header, whole-frame `_process()` (named after C++ `P::process`; "dispatch"
  was rejected because `dispatch` was renamed to `message_type`).
- C runtime: `framing.h` and `stream.h`, cross-checked against C++ in `tests/runtime_c/test_framing_c_vs_cpp.cpp`
  (9 scanner configs, 3 escape styles, the stream with 2 capacities and 3 chunk sizes; about 1M assertions). The
  input generator inserts whole sync / delimiter tokens; a coverage check showed that random bytes alone rarely
  formed frames for 2-byte syncs.
- C backend `emit_protocol.cpp`; 16 protocols generated from the test schemas, all compiled with `-Werror`.
- New schema `tests/schemas/framing.yaml` (length adjust, `header_body` / `all` coverage, little-endian length and
  checksum, `end` after escaped fixed and length-prefixed frames). Planted bugs in the emitter for those options
  (checksum endian, adjust sign, escaped suffix) passed the tests until this schema was added; now all are caught.
- The new schema found a Studio bug: `end` was only supported for delimited framing (editor, YAML export, default
  reserved bytes). Fixed in `editor.js`, `yaml.js`, `state.js`, tooltip; docs/schema.md and README list `end` for
  fixed and length_prefixed.
- C++ goldens: only `framing.gmp.hpp` added. C goldens regenerated (protocols).

### 2026-11-21 (C backend: binary messages)
- Added the C backend (`generator/src/backend/c/`, registered as `c`) for enums and binary messages; see section 4.
  Its semantics mirror `backend/cpp/emit_binary_message.cpp` (same checks in the same order, same error codes).
- Goldens in `tests/generator/golden/c/` (all schemas except `hdlc`, which uses a custom type). Diagnostics tests for
  unsupported features, name clashes, skipped text/protocols and prefixes.
- `tests/roundtrip_c/`: 11 schemas generated and compiled as C11 with `-Werror`; `test_c_generated.cpp` (9 cases,
  about 28k assertions) compares C and C++. Checked by injecting bugs into the backend: wrong endian, no padding
  strip, a missing strict-enum check and a wrong CRC endian were all caught. Using the length member instead of
  the container size inside write-side expressions is not observable with the current schemas.
- `GMP_C_WARNINGS` moved to `tests/CMakeLists.txt`. README: "C output (preview)", `--lang c`, `LANG c`.

### 2026-11-21 (C runtime prefix)
- Renamed the C runtime prefix from `gmp_` / `GMP_` to `gmsg_` / `GMSG_` (GNU MP uses `gmp_` functions and
  `GMP_ERROR_*` macros), and the umbrella header `gmp/c/gmp.h` to `gmp/c/runtime.h`. The path `gmp/c/` is kept.
- Noted a future task: consider the same change for the C++ runtime (section 7).

### 2026-11-21 (C runtime core)
- Decisions for the C backend: C11 (C99 later), header-only runtime (`.c` library later), `.h` + `.c` generated
  output, first version = binary messages + protocols/stream/callbacks, `GMP_FLOAT_TEXT` builtin by default,
  `GMP_USE_LIBC` on by default. Details and the list of later work are in the memory note `c-backend-todo`.
- Added `runtime-c/include/gmp/c/` (section 4) and `gmp::runtime_c`; the root CMake enables C only for tests.
  `_gmp_language_layout` has a `c` entry (`.gmp.h` + `.gmp.c`), though gmpc has no C backend yet.
- Tests in `tests/runtime_c/` (8 ctest entries). CRCs are checked against the reveng check values and against the
  C++ runtime. Checked by injecting a wrong CRC preset and a wrong bit mask: both were caught.
- The CRC is bitwise (no tables) to stay header-only; a table-driven variant is planned with the `.c` library.

### 2026-11-21 (C backend evaluation and prep)
- Evaluated adding a C backend; findings and open decisions are in the memory note `c-backend-todo` and section 7.
- Prep refactors (the user skipped sharing the `cpp_common` helpers):
  - lowering now checks that protocol messages have view access and (text) the protocol's separator; this used to
    live in the C++ backend. The view error is now per message: "message 'A' needs view access to be part of
    protocol 'P'". New diagnostics test.
  - goldens moved to `tests/generator/golden/cpp/` (renames only); `test_golden.cpp` checks every output file per
    backend (`check_goldens(lang)`), and the determinism test runs for every registered backend.
  - `test_diagnostics.cpp` `compile(yaml, backend)` takes a backend name.
  - `gmp_generate` takes per-language output suffixes and runtime target from `_gmp_language_layout`; output with
    sources becomes a STATIC library. README "Adding a language backend" updated.
- Removed the stale "No git" known issue.

### 2026-11-21 (GMP skill evaluation)
- Iteration 1 used 3 cases, each run with and without the skill: 17/18 with the skill, 18/18 without.
  - The cases didn't discriminate: gmpc's diagnostics and the runtime headers were enough for a model without the
    skill.
  - The one miss: the with-skill run wrapped an I2C read in a `fixed` protocol, copying the SHT4x recipe.
  - Results are in `skills/gmp-workspace/iteration-1/`.
- SKILL.md now says when not to add a protocol, and describes the ISR → SPSC ring → `feed()` pattern. The recipe
  note says I2C replies decode directly.
- Added 4 harder cases (not yet run). Their captures were generated and CRC-verified from the reference schemas in
  `skills/gmp-workspace/reference/`:
  - LEN counting CMD + payload + CRC
  - a CRC over the stuffed bytes
  - a HID report with a strict enum, bits, a computed serial, `when` and an array
  - `max` mistaken for a value limit
- Found: gmpc silently accepts and ignores `max:` on scalar fields.

### 2026-11-21 (Studio test suite)
- Added `tools/schema-studio/tests/`:
  - `lib.js`: a small framework
  - `unit.html`
  - `crosscheck.html`, against `crossref.cpp` (built as `gmp_studio_crossref`): 40 checksum values, 15 messages and
    11 noisy streams compared as `gmp::print` text, plus validation and stable YAML export of all 16 schemas
  - `ui.html`: outline, field editors, a tooltip on every control, tabs, decode views and targets
  - `guide.html`
  - `run_tests.py`: headless Edge/Chrome, a fresh profile per page, the gmpc round trip of exported YAML, `--offline`;
    exit 77 means skipped
- Registered in ctest as `gmp_studio_tests` (128 checks, about 3 s).
- Verified by breaking the Studio on purpose: a print change, a missing tooltip and a dropped YAML key were all caught.
- Fixed outdated `unit` tooltips, which said "documentation only" although units now appear in `gmp::print` output.

### 2026-11-21 (stream I/O, part B: pull API)
- Tabled C (ISR ring buffer) after presenting its design; the open questions are in the memory note.
- B decisions:
  - both `poll()` and `poll_event()`
  - `poll_frame()` for the header
  - views and struct copies
  - `poll_for<V>()`, which sends other messages to the listeners
  - poll calls the error listener only
  - a Debug check for expired views

  The blocking `wait_for(timeout)` was moved to D.
- `next()` was refactored onto the shared `step()`. Its behaviour is unchanged, and the existing tests pass.
- Tests: `test_stream_poll.cpp` (9 cases) and the embedded profile. Docs: README "Pulling messages (`poll`)", the
  Guide, the skill, and `Received` added to the reserved names.

### 2026-11-21 (stream I/O, part A: stored handlers)
- Discussed stream I/O directions A–D; the user chose to do all of them, one at a time.
- Implemented A as the combined design with H3 header delivery:
  - runtime: stored `Handler` in `StreamParser`, and `gmp/callbacks.hpp`
  - generated per protocol: `StreamWith` / `make_stream`, `Callbacks`, `CallbackStream`
  - generator: reserved-name check
  - tests: `test_stream_callbacks.cpp` (5 cases), a backend diagnostics case, and the embedded profile
  - docs: README "Storing the handler", a Guide streaming subsection and the skill
  - the `binary_telemetry` example now uses `make_stream`
- Goldens regenerated (additions, plus a reworded `Stream` comment).

### 2026-11-21 (pretty printing)
- Noted a future task: a language selector on the guide page once non-C++ backends exist.
- Added pretty printing:
  - runtime `gmp/print.hpp`
  - generated `print_to` on structs, views and bits groups (binary and text), with `print_to` reserved as a member name
  - `static_string::append`
  - tests `test_print.cpp` (10 cases) and the embedded profile
  - Studio View switch and `js/print.js`, cross-checked against C++
  - a Guide "Printing for logs" section
  - README "Printing", plus a `gmp_print` row in the custom-extension tables
  - skill note, and the `binary_telemetry` example now uses `gmp::println`
- The cross-check found that f32 values printed widened to double (`0.8999999761581421`). Fixed with a regression test.
- Goldens regenerated (additions only).

### 2026-11-20 → 2026-11-21
- Renamed dispatch/id → message_type/type_id across YAML, C++, IR and Studio; removed aliases and migration.
- SHT4x example from the datasheet (two variants: messages per command, strict command enum); field `crc:`,
  `conversions:`, `crc8_nrsc5`, strict enums (decode + encode), `computed:` values with setters; Studio decode examples.
- Byte stuffing generalised (index/xor/map, fixed + length_prefixed, `checksum.over: wire`), `length.counts: wire`,
  `covers: body_checksum`; `examples/reserved_sync`.
- Studio: tooltips for every editor control, `guide.html` visual guide, decode-tester spacing fix, dark scrollbars.
- `skills/gmp/` AI skill with generated references; README section "Using GMP with an AI assistant".
- Notes saved for future work: ASCII numeric fields, value ranges, bad length handling, stream I/O + callbacks,
  skill eval prompts.
