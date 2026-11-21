// lib.js - a tiny test framework for the Studio test pages (run headless by run_tests.py).
//
// Results are written to <pre id="result"> one per line: "PASS name", "FAIL name: message", "SKIP name: reason",
// then "DONE". Data for the runner (e.g. exported YAML) goes to <textarea id="output"> as JSON.
(function () {
  'use strict';

  class Skip extends Error {}
  const tests = [];
  const output = {};

  const fmt = (v) => {
    if (typeof v === 'bigint') return v.toString() + 'n';
    if (typeof v === 'string') return JSON.stringify(v);
    try { return JSON.stringify(v, (k, x) => (typeof x === 'bigint' ? x.toString() + 'n' : x)); } catch (_) { return String(v); }
  };

  /** Show where two strings first differ (multi-line print output is easier to read this way). */
  function diff(actual, expected) {
    if (typeof actual !== 'string' || typeof expected !== 'string') return `\n  expected: ${fmt(expected)}\n  actual:   ${fmt(actual)}`;
    const a = actual.split('\n'), e = expected.split('\n');
    for (let i = 0; i < Math.max(a.length, e.length); i++) {
      if (a[i] !== e[i]) return `\n  first difference at line ${i + 1}:\n  expected: ${fmt(e[i])}\n  actual:   ${fmt(a[i])}`;
    }
    return '';
  }

  window.T = {
    Skip,
    /** Register a test. fn may be async. */
    test(name, fn) { tests.push({ name, fn }); },
    skip(reason) { throw new Skip(reason); },
    ok(cond, msg) { if (!cond) throw new Error(msg || 'assertion failed'); },
    eq(actual, expected, msg) {
      if (actual !== expected) throw new Error((msg || 'values differ') + diff(actual, expected));
    },
    /** Element-wise equality for arrays of strings / numbers. */
    eqList(actual, expected, msg) {
      if (!Array.isArray(actual)) throw new Error(`${msg || 'list'}: not an array: ${fmt(actual)}`);
      if (actual.length !== expected.length)
        throw new Error(`${msg || 'list'}: ${actual.length} items, expected ${expected.length}\n  actual: ${fmt(actual)}`);
      actual.forEach((x, i) => T.eq(x, expected[i], `${msg || 'list'} [${i}]`));
    },
    output(key, value) { output[key] = value; },
    sleep: (ms) => new Promise((r) => setTimeout(r, ms)),
    /** Wait until cond() is truthy (polling), or fail after `ms`. */
    async until(cond, ms = 8000, what = 'condition') {
      const end = Date.now() + ms;
      while (!cond()) {
        if (Date.now() > end) throw new Error(`timed out waiting for ${what}`);
        await T.sleep(25);
      }
    },
    /** js-yaml comes from cdnjs: without network the dependent tests are skipped, not failed. */
    async needYaml() {
      try { await window.GMP.loadJsYaml(); } catch (_) { T.skip('js-yaml could not be loaded from cdnjs (offline?)'); }
    },
    /** Load a script by URL (file:// works with --allow-file-access-from-files). */
    loadScript(src) {
      return new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src = src;
        s.onload = resolve;
        s.onerror = () => reject(new Error('could not load ' + src));
        document.head.appendChild(s);
      });
    },
    async run() {
      const lines = [];
      for (const t of tests) {
        try {
          await t.fn();
          lines.push(`PASS ${t.name}`);
        } catch (e) {
          if (e instanceof Skip) lines.push(`SKIP ${t.name}: ${e.message}`);
          else lines.push(`FAIL ${t.name}: ${e && e.message ? e.message : e}${e && e.stack && !(e.message || '').includes('\n') ? '\n    ' + e.stack.split('\n').slice(1, 3).join('\n    ') : ''}`);
        }
      }
      lines.push('DONE');
      const out = document.getElementById('output') || document.body.appendChild(Object.assign(document.createElement('textarea'), { id: 'output' }));
      out.value = JSON.stringify(output);
      out.textContent = out.value;
      const pre = document.getElementById('result') || document.body.appendChild(Object.assign(document.createElement('pre'), { id: 'result' }));
      pre.textContent = lines.join('\n');
    },
  };
})();
