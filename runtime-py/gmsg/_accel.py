"""The optional compiled accelerator (gmsg._native, built from the C runtime).

`native` is the extension module, or None when it is not installed or GMSG_PURE=1 is set. Every function that can use
it has a pure-Python implementation with the same results; the test suite runs both.
"""

from __future__ import annotations

import os
from types import ModuleType

native: ModuleType | None = None

if os.environ.get("GMSG_PURE", "") in ("", "0"):
    try:
        from . import _native as native  # type: ignore[no-redef]
    except ImportError:
        native = None


def use_native(enabled: bool) -> bool:
    """Switch the accelerator on or off at run time (for tests and benchmarks). Returns whether it is now in use."""
    global native
    if not enabled:
        native = None
        return False
    try:
        from . import _native as mod
    except ImportError:
        native = None
        return False
    native = mod
    return True
