"""A minimal test harness for the Python test scripts (no dependencies; the embeddable Python has no pip). Each test
runs its checks; only the first few failures of a test are shown, so one broken area does not flood the output."""

from __future__ import annotations

import math
import sys
import traceback
from typing import Any, Callable

_tests: list[tuple[str, Callable[[], None]]] = []
_current: dict[str, Any] = {}
verbose = "-v" in sys.argv


def test(name: str) -> Callable[[Callable[[], None]], Callable[[], None]]:
    def deco(fn: Callable[[], None]) -> Callable[[], None]:
        _tests.append((name, fn))
        return fn

    return deco


def _same(a: Any, b: Any) -> bool:
    if isinstance(a, float) and isinstance(b, float) and math.isnan(a) and math.isnan(b):
        return True
    return a == b and type(a) is type(b) if isinstance(a, bool) or isinstance(b, bool) else a == b


def eq(got: Any, want: Any, what: str = "") -> None:
    _current["checks"] += 1
    if _same(got, want):
        return
    _current["failures"].append(f"{what + ': ' if what else ''}got {got!r}, want {want!r}")


def ok(cond: Any, what: str = "") -> None:
    _current["checks"] += 1
    if not cond:
        _current["failures"].append(what or "condition failed")


def skip(reason: str) -> None:
    _current["skipped"] = reason


def run() -> int:
    failed = skipped = checks = 0
    for name, fn in _tests:
        _current.update(checks=0, failures=[], skipped=None)
        try:
            fn()
        except Exception:
            _current["failures"].append("raised " + traceback.format_exc())
        checks += _current["checks"]
        if _current["failures"]:
            failed += 1
            print(f"FAIL {name} ({len(_current['failures'])} of {_current['checks']} checks)")
            for f in _current["failures"][:8]:
                print("    " + f)
        elif _current["skipped"]:
            skipped += 1
            print(f"SKIP {name}: {_current['skipped']}")
        elif verbose:
            print(f"PASS {name} ({_current['checks']} checks)")
    print(f"{len(_tests) - failed - skipped} passed, {failed} failed, {skipped} skipped ({checks} checks)")
    return 1 if failed else 0
