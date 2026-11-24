#!/usr/bin/env python3
"""Run the Schema Studio test pages in a headless Chromium browser (Edge or Chrome).

    py tools/schema-studio/tests/run_tests.py [--browser PATH] [--gmpc PATH] [--crossref PATH] [--only NAME ...]

Pages (tools/schema-studio/tests/*.html) write PASS / FAIL / SKIP lines into <pre id="result">; this script loads
each page with --dump-dom and collects them.

- unit       decoder / printer / expression basics (no network)
- crosscheck Studio vs the generated C++ (needs --crossref, the gmp_studio_crossref program built by CMake):
             checksums, decoded values (gmsg::print text) of messages and noisy streams, validation and YAML export
             of every schema in the repository. With --gmpc, every exported schema must generate the same C++
             as the original.
- ui         index.html: outline, field editors, tooltips on every control, tabs, decode tester, print view
- guide      guide.html: all sections and live demos

Tests that need js-yaml (loaded from cdnjs) are skipped when offline. Exit code: 0 = no failures, 1 = failures,
77 = nothing could run (no browser), which ctest reports as skipped.
"""

import argparse
import glob
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
PAGES = ["unit", "crosscheck", "ui", "guide"]
CROSSREF_SCHEMAS = {
    "telemetry": "tests/schemas/telemetry.yaml",
    "can": "tests/schemas/can.yaml",
    "computed": "tests/schemas/computed.yaml",
    "fieldcrc": "tests/schemas/fieldcrc.yaml",
    "framing": "tests/schemas/framing.yaml",
    "enums": "tests/schemas/enums.yaml",
    "limits": "tests/schemas/limits.yaml",
    "stuffed": "tests/schemas/stuffed.yaml",
    "nmea": "tests/schemas/nmea.yaml",
    "sht4x": "examples/sensirion_sht4x/sht4x.yaml",
}
BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
]
BROWSER_NAMES = ["msedge", "microsoft-edge", "google-chrome", "chrome", "chromium", "chromium-browser"]


def find_browser(arg):
    for cand in [arg, os.environ.get("GMP_BROWSER")]:
        if cand and Path(cand).exists():
            return cand
    for p in BROWSERS:
        if Path(p).exists():
            return p
    for n in BROWSER_NAMES:
        p = shutil.which(n)
        if p:
            return p
    return None


def file_url(p):
    return Path(p).resolve().as_uri()


def run_page(browser, page, query, work, extra=()):
    profile = tempfile.mkdtemp(prefix="gmp-studio-profile-", dir=work)
    url = file_url(HERE / f"{page}.html") + (("?" + query) if query else "")
    cmd = [browser, "--headless", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
           "--allow-file-access-from-files", f"--user-data-dir={profile}", "--virtual-time-budget=60000",
           "--window-size=1500,1100", *extra, "--dump-dom", url]
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=240).stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return [("FAIL", f"{page}: browser timed out")], {}
    finally:
        shutil.rmtree(profile, ignore_errors=True)
    m = re.search(r'<pre id="result"[^>]*>(.*?)</pre>', out, re.S)
    text = html.unescape(m.group(1)) if m else ""
    results = []
    current = None
    for line in text.splitlines():
        mm = re.match(r"(PASS|FAIL|SKIP) (.*)", line)
        if mm:
            current = [mm.group(1), f"{page}: {mm.group(2)}"]
            results.append(current)
        elif line == "DONE":
            current = None
        elif current is not None:  # continuation of a multi-line failure message
            current[1] += "\n" + line
    if "DONE" not in text.splitlines():
        results.append(["FAIL", f"{page}: page did not finish (got {len(results)} results; JS error or timeout?)"])
    data = {}
    m = re.search(r'<textarea id="output"[^>]*>(.*?)</textarea>', out, re.S)
    if m and m.group(1).strip():
        try:
            data = json.loads(html.unescape(m.group(1)))
        except json.JSONDecodeError:
            results.append(["FAIL", f"{page}: unreadable output data"])
    return [tuple(r) for r in results], data


def all_schemas():
    paths = sorted(glob.glob(str(REPO / "tests/schemas/*.yaml")) + glob.glob(str(REPO / "examples/**/*.yaml"), recursive=True))
    return [Path(p) for p in paths]


def gmpc_output(gmpc, path, lang="cpp"):
    r = subprocess.run([gmpc, "--lang", lang, "--stdout", str(path)], capture_output=True)
    if r.returncode != 0:
        return None, r.stderr.decode("utf-8", "replace")
    lines = r.stdout.decode("utf-8", "replace").splitlines()
    # the first line names the source file
    return "\n".join(l for l in lines if not l.startswith(("// Generated by gmpc", "/* Generated by gmpc"))), ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--browser")
    ap.add_argument("--gmpc", help="gmpc executable: also check that exported YAML generates identical C++")
    ap.add_argument("--crossref", help="gmp_studio_crossref executable (C++ reference data)")
    ap.add_argument("--work", help="scratch directory (default: a temporary directory)")
    ap.add_argument("--only", nargs="*", choices=PAGES, help="run only these pages")
    ap.add_argument("-v", "--verbose", action="store_true", help="list passing tests too")
    ap.add_argument("--offline", action="store_true", help="block network access in the browser (tests the skip path)")
    a = ap.parse_args()
    for attr in ("gmpc", "crossref"):
        if getattr(a, attr):
            exe = Path(getattr(a, attr)).resolve()
            if not exe.exists() and exe.with_suffix(".exe").exists():
                exe = exe.with_suffix(".exe")
            if not exe.exists():
                print(f"error: --{attr} {getattr(a, attr)} not found")
                return 2
            setattr(a, attr, str(exe))

    browser = find_browser(a.browser)
    if not browser:
        print("SKIP: no Edge / Chrome / Chromium found (use --browser or GMP_BROWSER)")
        return 77
    work = Path(a.work) if a.work else Path(tempfile.mkdtemp(prefix="gmp-studio-tests-"))
    work.mkdir(parents=True, exist_ok=True)
    pages = a.only or PAGES
    results = []

    for page in pages:
        query = ""
        if page == "crosscheck":
            if not a.crossref:
                results.append(("SKIP", "crosscheck: no --crossref program given"))
                continue
            ref = subprocess.run([a.crossref], capture_output=True)
            if ref.returncode != 0:
                results.append(("FAIL", "crosscheck: reference program failed: " + ref.stderr.decode("utf-8", "replace")))
                continue
            data = {
                "schemas": {k: (REPO / v).read_text(encoding="utf-8") for k, v in CROSSREF_SCHEMAS.items()},
                "crossref": json.loads(ref.stdout.decode("utf-8")),
                "roundtrip": [{"path": str(p.relative_to(REPO)).replace("\\", "/"), "yaml": p.read_text(encoding="utf-8")}
                              for p in all_schemas()],
            }
            data_file = work / "crosscheck-data.js"
            data_file.write_text("window.GMP_TEST_DATA = " + json.dumps(data) + ";\n", encoding="utf-8")
            query = "data=" + file_url(data_file)
        page_results, out = run_page(browser, page, query, work,
                                     ["--host-resolver-rules=MAP * ~NOTFOUND"] if a.offline else [])
        results += page_results

        # exported YAML must generate the same C++ as the original schema
        if page == "crosscheck" and out.get("roundtrip"):
            if not a.gmpc:
                results.append(("SKIP", "crosscheck: gmpc round trip (no --gmpc given)"))
            for rel, yaml_text in sorted(out["roundtrip"].items()):
                if not a.gmpc:
                    break
                name = f"crosscheck: gmpc output from exported {rel}"
                exported = work / "roundtrip" / rel
                exported.parent.mkdir(parents=True, exist_ok=True)
                exported.write_text(yaml_text, encoding="utf-8")
                for lang, label in (("cpp", "C++"), ("c", "C")):
                    lname = name if lang == "cpp" else f"{name} (--lang c)"
                    want, err1 = gmpc_output(a.gmpc, REPO / rel, lang)
                    got, err2 = gmpc_output(a.gmpc, exported, lang)
                    if want is None and lang == "c":
                        continue  # the C backend does not generate this schema (e.g. custom items without c:)
                    if want is None:
                        results.append(("FAIL", f"{lname}: gmpc rejects the original: {err1}"))
                    elif got is None:
                        results.append(("FAIL", f"{lname}: gmpc rejects the export: {err2}"))
                    elif got != want:
                        wl, gl = want.splitlines(), got.splitlines()
                        i = next((k for k in range(min(len(wl), len(gl))) if wl[k] != gl[k]), min(len(wl), len(gl)))
                        results.append(("FAIL", f"{lname}: generated {label} differs at line {i + 1}\n"
                                                f"    original: {wl[i] if i < len(wl) else '<end>'}\n"
                                                f"    exported: {gl[i] if i < len(gl) else '<end>'}"))
                    else:
                        results.append(("PASS", lname))

    if not a.work:
        shutil.rmtree(work, ignore_errors=True)

    counts = {k: sum(1 for r in results if r[0] == k) for k in ("PASS", "FAIL", "SKIP")}
    for status, name in results:
        if status != "PASS" or a.verbose:
            print(f"{status} {name}")
    print(f"\nStudio tests: {counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIP']} skipped  ({Path(browser).name})")
    if counts["FAIL"]:
        return 1
    if counts["PASS"] == 0:
        return 77
    return 0


if __name__ == "__main__":
    sys.exit(main())
