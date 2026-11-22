# GMP development session log

Read this first when starting a new working session. It records what exists, why it was built that way, what is
known to be rough, and what is planned. Update it at the end of each session (newest entries at the top of
"Session history").

Last updated: 2026-11-21.

---

## 1. Project in one paragraph

GMP (generic_message_parser) is a C++23 YAML-driven code generator for **existing** binary and text wire protocols.
`gmpc` reads a schema (`messages`, `enums`, `protocols`, `custom`) and writes one header `<schema>.gmp.hpp` with
eager structs, zero-copy Views, and Protocol types (`parse`, `decode`, `encode`, `Stream<>`). The runtime
(`runtime/include/gmp/`) is header-only: no heap, no exceptions, no RTTI, `std::expected` errors. Schema Studio
(`tools/schema-studio/`) is a static web app for building schemas visually, decoding test bytes and learning
the features (`guide.html`). An AI-assistant skill lives in `skills/gmp/`.

## 2. Environment and commands (Windows, CLion toolchain)

- Toolchain PATH (Git Bash):
  `export PATH="/c/Users/Tabarious/AppData/Local/Programs/CLion/bin/mingw/bin:/c/Users/Tabarious/AppData/Local/Programs/CLion/bin/ninja/win/x64:/c/Users/Tabarious/AppData/Local/Programs/CLion/bin/cmake/win/x64/bin:$PATH"`
  (MinGW GCC 15, Ninja, CMake; yaml-cpp and Catch2 via FetchContent).
- Debug build + tests: `cmake --build build && ctest --test-dir build` (141 tests, all passing, including `gmp_studio_tests`).
- Release build: `cmake --build build-release` (Release, bench ON, tests and examples OFF; it is a compile check only).
- Regenerate golden files after an intentional generator change:
  `GMP_UPDATE_GOLDEN=1 ctest --test-dir build -R golden`, then check that only the expected files changed.
- Generator directly: `./build/generator/gmpc.exe --out <dir> schema.yaml` / `--check`.
- Examples run from `build/examples/*/example_*.exe` (6 programs).
- Quick standalone compiles need `-static-libgcc -static-libstdc++` (full `-static` fails to link with this MinGW;
  binaries without static libstdc++ fail with 0xc0000139 when the DLLs are not on PATH).
- Python is the `py` launcher (3.9, no PyYAML). There is no Node.js. Tools that need Python 3.10+ (e.g. the
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
| `runtime/include/gmp/` | header-only runtime: endian, byte/bit readers and writers, checksums, text codec, containers, framing (`framing/fixed.hpp`, `length_prefixed.hpp`, `delimited.hpp`, `escape.hpp`, `frame.hpp`), `stream_parser.hpp`, `error.hpp`, `print.hpp` |
| `generator/src/schema/` | `loader.cpp` (YAML → IR, key checks, renamed-key hints), `expr.cpp`, `validator.cpp` |
| `generator/src/ir/` | `ir.hpp` (language-neutral IR), `lowering.cpp` (resolution, sizes, offsets, escape tables, all semantic checks) |
| `generator/src/backend/cpp/` | C++ emitters: `emit_binary_message.cpp`, `emit_text_message.cpp`, `emit_protocol.cpp`, `cpp_backend.cpp`, `cpp_common.cpp` |
| `cmake/GmpGenerate.cmake` | `gmp_generate(TARGET … SCHEMAS …)` |
| `tests/runtime`, `tests/generator` (golden + diagnostics), `tests/roundtrip`, `tests/schemas` | test suites; goldens in `tests/generator/golden/` |
| `examples/` | binary_telemetry, nmea_text, bitpacked_can, custom_codec, sensirion_sht4x (two schemas), reserved_sync |
| `docs/schema.md` | compact schema reference (source for the skill's `references/schema.md`) |
| `tools/schema-studio/` | Studio (`index.html`) and Guide (`guide.html`); JS in `js/` (see its README for the file roles); tests in `tests/` (`run_tests.py`, pages `unit` / `crosscheck` / `ui` / `guide`, C++ reference generator `crossref.cpp`) |
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
- Guide page `guide.html` (`js/guide.js`): 14 sections (incl. "Printing for logs"), 10 live demos built from real
  schemas, "Open in Studio". Demo runs accept `view: 'print' | 'line'`.
- Example inputs for the decode tester (SHT4x, NMEA, Link, any escaped protocol).
- Decode tester **View** switch: Fields / Print / One line. `js/print.js` mirrors `gmp::print` exactly; the decoder
  now keeps raw values on its nodes (`v`, `field`, `bit`, `content`, `absent`, `msg`, `top`) for it.

AI skill: `skills/gmp/` (see section 7).

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
- **The AI skill bundles its references** (generated from docs/README/examples by a script) so it works when copied
  outside the repo.

## 6. Known issues and gaps

- The Studio suite does not cover every interactive edit, e.g. the drag splitter, renaming a type and its references,
  or adding and removing fields through the UI. Older scratchpad tests for those (position, rename, escape UI) were
  not recreated.
- **Corrupted length below max_frame** delays good frames on an idle link (no timeout / per-type size check). See
  future task "bad length handling".
- Studio import and the Guide's live demos need internet for js-yaml (cdnjs); the rest works offline.
- `build-release` does not build tests or examples; run tests in `build`.
- Text protocols with escaping are allowed only for delimited framing; `checksum.over: wire` is binary-only.
- No git: keep backups before large refactors.

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
| Guide language selector | the guide is C++-specific today; when other target languages get backends, add a language picker to `guide.html` so code, API names and notes match the chosen language (schema content stays shared) |

Each planned task has a detailed note (with open questions to ask the user) in the Claude memory directory for this
project.

## 8. Session history

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
