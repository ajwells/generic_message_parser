// A minimal test harness for the Node test scripts (no dependencies). Each test runs its checks and stops counting
// after the first few failures, so one broken area does not flood the output.
'use strict';

const tests = [];
let current = null;
const verbose = process.argv.includes('-v');

const show = (v) => (typeof v === 'bigint' ? v + 'n' : JSON.stringify(v));

module.exports = {
  test(name, fn) { tests.push({ name, fn }); },
  eq(got, want, what = '') {
    current.checks++;
    if (got === want || (Number.isNaN(got) && Number.isNaN(want))) return;
    current.failures.push(`${what ? what + ': ' : ''}got ${show(got)}, want ${show(want)}`);
  },
  eqList(got, want, what = '') {
    current.checks++;
    if (got.length === want.length && got.every((g, i) => g === want[i])) return;
    current.failures.push(`${what ? what + ': ' : ''}got [${got.map(show)}], want [${want.map(show)}]`);
  },
  skip(reason) { current.skipped = reason; },
  async run() {
    let failed = 0, skipped = 0, checks = 0;
    for (const t of tests) {
      current = { checks: 0, failures: [], skipped: null };
      try {
        await t.fn();
      } catch (e) {
        current.failures.push('threw ' + (e && e.stack ? e.stack : e));
      }
      checks += current.checks;
      if (current.failures.length) {
        failed++;
        console.log(`FAIL ${t.name} (${current.failures.length} of ${current.checks} checks)`);
        for (const f of current.failures.slice(0, 8)) console.log('    ' + f);
      } else if (current.skipped) {
        skipped++;
        console.log(`SKIP ${t.name}: ${current.skipped}`);
      } else if (verbose) {
        console.log(`PASS ${t.name} (${current.checks} checks)`);
      }
    }
    console.log(`${tests.length - failed - skipped} passed, ${failed} failed, ${skipped} skipped (${checks} checks)`);
    process.exitCode = failed ? 1 : 0;
  },
};
