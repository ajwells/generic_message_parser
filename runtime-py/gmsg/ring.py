"""Single-producer / single-consumer byte queue (gmp/byte_ring.hpp).

The producer (for example a thread reading a serial port) calls push() / push_bytes(); the consumer drains the ring
into a StreamParser with feed(ring) or push(ring), so decoding and listeners run in the consumer. One producer and one
consumer at a time; neither side takes a lock (each index has a single writer).

Overflow: a full ring drops the newest bytes (counted in `dropped`) and marks an overrun; it then drops input until
the consumer has read everything that arrived before the gap. The stream reports the gap as Err.OVERRUN and drops the
frame in progress.
"""

from __future__ import annotations

from typing import Any

from .core import to_bytes
from .errors import Err, fail


class ByteRing:
    """A ring of `capacity` bytes (a power of two)."""

    __slots__ = ("_buf", "_mask", "_w", "_r", "_overrun", "_dropped", "_high")

    def __init__(self, capacity: int) -> None:
        if not isinstance(capacity, int) or capacity < 2 or capacity & (capacity - 1):
            fail(Err.OUT_OF_RANGE, "ring capacity must be a power of two (>= 2)")
        self._buf = bytearray(capacity)
        self._mask = capacity - 1
        self._w = 0  # written by the producer (free-running)
        self._r = 0  # written by the consumer
        self._overrun = False  # set by the producer, cleared by the consumer
        self._dropped = 0
        self._high = 0

    @property
    def capacity(self) -> int:
        return self._mask + 1

    # ---- producer side ----------------------------------------------------------------------------------------
    def push(self, b: int) -> bool:
        """Append one byte. False (the byte is dropped) when the ring is full or an overrun waits to be drained."""
        if self._overrun:
            self._dropped += 1
            return False
        w, r = self._w, self._r
        if w - r == self._mask + 1:
            self._dropped += 1
            self._overrun = True
            return False
        self._buf[w & self._mask] = b
        self._w = w + 1
        if w + 1 - r > self._high:
            self._high = w + 1 - r
        return True

    def push_bytes(self, data: Any) -> int:
        """Append as many bytes as fit (the rest are dropped: an overrun). Returns the number appended."""
        b = to_bytes(data)
        if not b:
            return 0
        if self._overrun:
            self._dropped += len(b)
            return 0
        w, r, cap = self._w, self._r, self._mask + 1
        n = min(len(b), cap - (w - r))
        start = w & self._mask
        first = min(n, cap - start)
        self._buf[start:start + first] = b[:first]
        if n > first:
            self._buf[0:n - first] = b[first:n]
        if n:
            self._w = w + n
            if w + n - r > self._high:
                self._high = w + n - r
        if n < len(b):
            self._dropped += len(b) - n
            self._overrun = True
        return n

    # ---- consumer side ----------------------------------------------------------------------------------------
    def readable(self) -> memoryview:
        """The oldest buffered bytes that are contiguous in storage (a view, valid until consume())."""
        w, r = self._w, self._r
        start = r & self._mask
        return memoryview(self._buf)[start:start + min(w - r, self._mask + 1 - start)]

    def consume(self, n: int) -> None:
        """Release the first `n` bytes of readable() to the producer."""
        self._r += n

    def read(self, max_bytes: int | None = None) -> bytes:
        """Copy up to `max_bytes` bytes out (default: all buffered). The overrun flag is not cleared here."""
        out = bytearray()
        limit = self.size if max_bytes is None else max_bytes
        while len(out) < limit:
            part = self.readable()
            if not len(part):
                part.release()
                break
            n = min(len(part), limit - len(out))
            out += part[:n]
            part.release()
            self.consume(n)
        return bytes(out)

    @property
    def size(self) -> int:
        """Bytes buffered (the producer may add more at any time)."""
        return self._w - self._r

    @property
    def empty(self) -> bool:
        return self._w == self._r

    @property
    def overrun_pending(self) -> bool:
        """True when input was dropped after the buffered bytes; the producer drops input until clear_overrun()."""
        return self._overrun

    def clear_overrun(self) -> None:
        """Accept input again; call only when the ring is empty."""
        self._overrun = False

    @property
    def dropped(self) -> int:
        """Bytes dropped because the ring was full."""
        return self._dropped

    @property
    def high_water(self) -> int:
        """Highest fill level seen by the producer."""
        return self._high
