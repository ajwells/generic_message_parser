# Schema Studio tests

Headless browser tests for the Studio (`index.html`), its decoder and printer, and the Guide (`guide.html`). They run
as part of `ctest` (test `gmp_studio_tests`) when Python 3 and Edge, Chrome or Chromium are available. If no browser is
found, the test reports as *skipped*.

```sh
ctest --test-dir build -R studio --output-on-failure
# or directly (after building gmpc and gmp_studio_crossref):
py tools/schema-studio/tests/run_tests.py --gmpc build/generator/gmpc.exe --crossref build/tests/roundtrip/gmp_studio_crossref.exe
py tools/schema-studio/tests/run_tests.py --only ui guide -v     # one or more pages; -v also lists passes
```

| page | what it checks |
|------|----------------|
| `unit.html` | Hex and text input parsing, expressions, integer literals, number formatting (`std::to_chars` rules), and the example schema's example input. |
| `crosscheck.html` | Studio against the generated C++ (reference data from `crossref.cpp`, built as `gmp_studio_crossref`). It covers every built-in checksum on two inputs, single messages, and noisy byte streams. For messages and streams, the decoded values are compared as `gmp::print` text, and streams must yield the same valid frames in the same order. It also validates every schema in `tests/schemas` and `examples` (Studio must report no errors) and checks that YAML import and export is stable. |
| (runner) | With `--gmpc`, each exported YAML must make `gmpc` generate the same C++ as the original schema. |
| (gmpc engine) | `crosscheck.html` also decodes every C++ reference message and stream with the gmpc engine (`gmpc/gmpc.js` → generated JavaScript) and checks that Quick and gmpc agree. ctest `gmp_studio_gmpc_fresh` (`gmpc/build.py --check`) fails when `gmpc.js` is older than the generator sources. |
| `ui.html` | `index.html?example` in a frame. It selects every outline item, opens every field editor, and checks that **every control has a tooltip**. It also covers the tabs, the decode tester with its Fields / Print / One line views, and every decode target. Any JavaScript error fails the test. |
| `guide.html` | Every Guide section and live demo renders and decodes, including the printing demos. |

- **Offline:** schema import uses js-yaml from cdnjs. Without network, the tests that need it are reported as
  skipped, not failed. `--offline` blocks the network on purpose to check that path.
- **Results:** each page writes `PASS` / `FAIL` / `SKIP` lines into `<pre id="result">` through the small framework
  in `lib.js` (`T.test`, `T.eq`, `T.eqList`, `T.until`, `T.skip`). The runner reads them with `--dump-dom`, using a
  fresh browser profile for each page.
- **Adding a test:** add `T.test('name', async () => { ... })` to the matching page. For a new C++ comparison, add a
  case to `crossref.cpp`. Its schemas must be part of `gmp_test_schemas` in `tests/roundtrip/CMakeLists.txt`, and
  listed in `CROSSREF_SCHEMAS` in `run_tests.py`.
