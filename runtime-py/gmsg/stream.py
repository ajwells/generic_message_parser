"""Incremental frame extraction from a byte stream (gmp/stream_parser.hpp).

`protocol` (a generated protocol class) provides: max_frame, scan(pending) -> FrameScan, and process(frame: bytes)
-> Received (raises GmpError when the frame is invalid). Messages are delivered to listeners (`on(...)`) or pulled
(`poll()`, iterators). The frame handed to process() is an immutable copy, so views never expire.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Iterator

from .core import to_bytes
from .errors import Err, GmpError, fail
from .framing import FrameScan


@dataclass
class Received:
    """One delivered frame: the message view, the header view (None without a header), the frame bytes, its offset in
    the stream and the data after the message (bytes, or tokens for text messages)."""

    message: Any
    header: Any = None
    bytes: bytes = b""
    offset: int = 0
    extra: int = 0


@dataclass
class StreamStats:
    frames: int = 0             # frames delivered successfully
    frame_errors: int = 0       # frames rejected (checksum, unknown message type, decode error)
    bytes_skipped: int = 0      # bytes discarded while resynchronising
    overflows: int = 0          # times the buffer filled without a complete frame
    resyncs: int = 0            # incomplete frames given up by resync() (also from tick())
    flushes: int = 0            # flush() calls that dropped buffered bytes
    frames_with_extra: int = 0  # delivered frames with data after their message


def extra_of(v: Any) -> int:
    """Data after a decoded message: extra_bytes (binary views) or extra_tokens (text views); 0 otherwise."""
    if v is None:
        return 0
    x = getattr(v, "extra_bytes", None)
    if x is None:
        x = getattr(v, "extra_tokens", None)
    return x or 0


def _key(what: Any) -> str:
    return what if isinstance(what, str) else getattr(what, "message_name", None) or getattr(what, "__name__", "")


def _error(code: int, data: bytes, offset: int) -> GmpError:
    e = GmpError(code)
    e.data = data
    e.offset = offset
    return e


class StreamParser:
    """The stream parser. Listeners: on('<Message>' or a generated class, fn(view, header)), on('frame', fn) for every
    message, on('error', fn(GmpError with .data and .offset)) for dropped data."""

    def __init__(self, protocol: Any, capacity: int | None = None) -> None:
        self.protocol = protocol
        self.capacity = capacity or protocol.max_frame
        if not self.capacity or self.capacity <= 0:
            fail(Err.INVALID_VALUE, "stream capacity must be > 0")
        self._buf = bytearray(self.capacity)
        self._head = self._tail = 0
        self._listeners: dict[str, list[Callable]] = {}
        self.stats = StreamStats()
        self.last_error = Err.NONE
        self._consumed = 0
        self._received = 0
        self.idle_timeout = 0
        self._ticked = False
        self._seen = 0
        self._last_activity = 0

    @property
    def buffered(self) -> int:
        """Bytes waiting in the buffer."""
        return self._tail - self._head

    @property
    def position(self) -> int:
        """Stream offset of the next buffered byte (bytes consumed so far)."""
        return self._consumed

    # ---- listeners ----------------------------------------------------------------------------------------------
    def on(self, what: Any, fn: Callable) -> Callable[[], None]:
        """Listen for one message (by name or generated class), 'frame' (every message) or 'error'. Returns a function
        that removes the listener."""
        key = _key(what)
        self._listeners.setdefault(key, []).append(fn)

        def off() -> None:
            lst = self._listeners.get(key)
            if lst and fn in lst:
                lst.remove(fn)

        return off

    def _emit(self, key: str, *args: Any) -> None:
        for fn in list(self._listeners.get(key, ())):
            fn(*args)

    def _deliver(self, f: Received) -> None:
        self._emit(type(f.message).message_name, f.message, f.header)
        self._emit("frame", f.message, f.header)

    def _report(self, e: GmpError) -> None:
        self._emit("error", e)

    # ---- input --------------------------------------------------------------------------------------------------
    def push(self, data: Any) -> int:
        """Copy as much of `data` as fits; returns the number of bytes accepted."""
        b = to_bytes(data)
        if self._head > 0 and self.capacity - self._tail < len(b):
            self._compact()
        n = min(len(b), self.capacity - self._tail)
        self._buf[self._tail:self._tail + n] = b[:n]
        self._tail += n
        self._received += n
        return n

    def feed(self, data: Any) -> int:
        """Push all of `data` and deliver every complete frame to the listeners. Returns the number of frames."""
        b = memoryview(to_bytes(data))
        before = self.stats.frames
        while True:
            n = self.push(b)
            b = b[n:]
            while self.next():
                pass
            if not len(b):
                break
        return self.stats.frames - before

    def next(self) -> bool:
        """One step: deliver the next frame or error to the listeners. False when more input is needed."""
        k, v = self._step()
        if k == "frame":
            self._deliver(v)
        elif k == "error":
            self._report(v)
        return k != "need_more"

    # ---- recovery: they only drop data; frames that then become complete come from next() / feed() / poll() ----
    def resync(self) -> bool:
        """Give up on the frame at the head of the buffer when it is incomplete (e.g. a corrupted length claims more
        bytes than will arrive): report it to the 'error' listeners as Truncated with its buffered bytes, drop its
        first byte and let the next scan resynchronise. False when the buffer is empty or its head frame is complete."""
        if self._head == self._tail:
            return False
        pending = bytes(self._buf[self._head:self._tail])
        if self.protocol.scan(pending).kind != "need_more":
            return False
        self.stats.resyncs += 1
        self.stats.bytes_skipped += 1
        self.last_error = Err.TRUNCATED
        offset = self._consumed
        self._consume(1)
        self._report(_error(Err.TRUNCATED, pending, offset))
        return True

    def flush(self) -> bool:
        """Drop every buffered byte, reported to the 'error' listeners as one Truncated error. False when nothing was
        buffered."""
        if self._head == self._tail:
            return False
        offset = self._consumed
        data = bytes(self._buf[self._head:self._tail])
        self.stats.flushes += 1
        self.stats.bytes_skipped += len(data)
        self.last_error = Err.TRUNCATED
        self._consume(len(data))
        self._report(_error(Err.TRUNCATED, data, offset))
        return True

    def set_idle_timeout(self, t: float) -> None:
        """resync() from tick() once the link has been idle for `t` (unit of the times passed to tick()); 0 = off."""
        self.idle_timeout = t

    def tick(self, now: float) -> bool:
        """Call periodically with the current time (e.g. time.monotonic()). Bytes pushed since the previous tick count
        as activity; after idle_timeout without new bytes an incomplete head frame is given up as by resync()
        (returns True). The next resync needs another idle period."""
        if not self._ticked or self._received != self._seen:
            self._ticked = True
            self._seen = self._received
            self._last_activity = now
            return False
        if not self.idle_timeout or now - self._last_activity < self.idle_timeout:
            return False
        self._last_activity = now
        return self.resync()

    # ---- pull API -----------------------------------------------------------------------------------------------
    def poll(self) -> Any:
        """The next valid message view, or None when more input is needed. Errors go to the 'error' listeners."""
        f = self._pull()
        return f.message if f else None

    def poll_frame(self) -> Received | None:
        """Like poll(), with the header view, the frame bytes, its stream offset and the data after the message."""
        return self._pull()

    def poll_message(self) -> Any:
        """Like poll(), decoded into the message dataclass (a frame whose struct decode fails is reported as an
        error)."""
        while True:
            k, v = self._step(lambda f: setattr(f, "struct", f.message.to_struct()))
            if k == "frame":
                return v.struct  # type: ignore[attr-defined]
            if k == "need_more":
                return None
            self._report(v)

    def poll_event(self) -> Received | GmpError | None:
        """The next message (Received) or error (GmpError with .data and .offset) in stream order, or None. Errors are
        returned here and not passed to the listeners."""
        k, v = self._step()
        return None if k == "need_more" else v

    def poll_for(self, what: Any) -> Any:
        """Wait (without blocking) for a message of one type; other messages go to the listeners meanwhile."""
        key = _key(what)
        while True:
            f = self._pull()
            if f is None:
                return None
            if type(f.message).message_name == key:
                return f.message
            self._deliver(f)

    def messages(self, data: Any = b"") -> Iterator[Any]:
        """The messages in `data` (pushed in pieces as room allows), then anything already buffered."""
        b = memoryview(to_bytes(data))
        while True:
            n = self.push(b)
            b = b[n:]
            while (m := self.poll()) is not None:
                yield m
            if not len(b):
                return

    def events(self, data: Any = b"") -> Iterator[Received | GmpError]:
        """Like messages(), yielding poll_event() results (messages and errors)."""
        b = memoryview(to_bytes(data))
        while True:
            n = self.push(b)
            b = b[n:]
            while (e := self.poll_event()) is not None:
                yield e
            if not len(b):
                return

    def reset(self) -> None:
        self._head = self._tail = 0
        self.stats = StreamStats()
        self.last_error = Err.NONE
        self._consumed = 0
        self._ticked = False

    # ---- internals ----------------------------------------------------------------------------------------------
    def _pull(self) -> Received | None:
        while True:
            k, v = self._step()
            if k == "frame":
                return v
            if k == "need_more":
                return None
            self._report(v)

    def _step(self, accept: Callable[[Received], None] | None = None) -> tuple[str, Any]:
        """Scan to the next frame or reportable error (StreamParser::step). Consumed bytes are dropped at once:
        frames and error data are copies."""
        while True:
            if self._head == self._tail:
                self._head = self._tail = 0
                return "need_more", None
            pending = memoryview(self._buf)[self._head:self._tail]
            s: FrameScan = self.protocol.scan(pending)
            offset = self._consumed
            if s.kind == "frame":
                data = bytes(pending[:s.size])
                pending.release()
                try:
                    f = self.protocol.process(data)
                    f.bytes = data
                    f.offset = offset
                    f.extra = extra_of(f.message)
                    if accept:
                        accept(f)
                    error = Err.NONE
                except GmpError as e:
                    error = e.code
                if error == Err.NONE:
                    self.stats.frames += 1
                    if f.extra:
                        self.stats.frames_with_extra += 1
                    self._consume(s.consume)
                    return "frame", f
                self.stats.frame_errors += 1
                self.stats.bytes_skipped += s.consume_on_error
                self._consume(s.consume_on_error)
                self.last_error = error
                return "error", _error(error, data, offset)
            if s.kind == "skip":
                n = max(1, min(s.size, len(pending)))
                self.stats.bytes_skipped += n
                if s.error == Err.NONE:
                    pending.release()
                    self._consume(n)
                    continue
                data = bytes(pending[:n])
                pending.release()
                self._consume(n)
                self.last_error = s.error
                return "error", _error(s.error, data, offset)
            full = len(pending) >= self.capacity
            pending.release()
            if full:
                self.stats.overflows += 1
                self.stats.bytes_skipped += 1
                data = bytes(self._buf[self._head:self._head + 1])
                self._consume(1)
                self.last_error = Err.BUFFER_FULL
                return "error", _error(Err.BUFFER_FULL, data, offset)
            if self._head > 0:
                self._compact()
            return "need_more", None

    def _consume(self, n: int) -> None:
        n = min(n, self._tail - self._head)
        self._consumed += n
        self._head += n
        if self._head == self._tail:
            self._head = self._tail = 0

    def _compact(self) -> None:
        if self._head == 0:
            return
        self._buf[0:self._tail - self._head] = self._buf[self._head:self._tail]
        self._tail -= self._head
        self._head = 0
