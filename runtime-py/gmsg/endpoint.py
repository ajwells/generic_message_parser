"""A protocol stream joined to a byte source, a byte sink and a clock (gmp/endpoint.hpp).

Endpoint (blocking, like the C++ / C endpoint):

    ep = Link.endpoint(serial.Serial("COM3", 115200, timeout=0), idle_timeout=0.05)   # source and sink
    ep.on(Status, lambda v, h: ...)
    ep.send(Command(opcode=1))
    ack = ep.transact(Command(opcode=2), Ack, timeout=0.1)     # send, then wait up to 100 ms for an Ack
    while True:
        ep.service()                                           # read what has arrived, deliver it

AsyncEndpoint (asyncio, frames processed as they arrive like the JavaScript endpoint):

    reader, writer = await asyncio.open_connection(host, port)
    ep = Link.async_endpoint(reader, writer)
    ack = await ep.transact(Command(opcode=2), Ack, timeout=0.1)

Sources (blocking endpoint): a pyserial-like object (read(n), and in_waiting when it has it), a socket (recv; a
non-blocking or timed-out recv counts as nothing yet), a file or BytesIO (read(n)), a gmsg.ByteRing filled by another
thread (overruns included), or a callable source(n) -> bytes. Reads must not block for long: open serial ports with a
small timeout. A source may have take_overrun() -> bool (input was lost before the next read).

Sinks: an object with write(bytes) (pyserial, files, BytesIO) or send(bytes) (sockets) returning the bytes taken (None:
all), a ByteRing (for a writer thread), or a callable. With a room method writable() -> int (not the bool of io
objects) or a ByteRing, send_policy 'whole' (default) refuses a frame that does not fit now (BUFFER_FULL, nothing
written); otherwise a short write fails with BUFFER_FULL. 'retry' writes the rest until it is out, or TIMEOUT after
send_timeout seconds (None: no limit).

wait_for(M, timeout, match) reads and processes input until a message M (class or name) arrives for which
match(view) (or match(view, header)) is true, and returns it decoded (the dataclass). Other messages go to the
listeners meanwhile, errors to the 'error' listeners; the awaited message does not. Raises GmpError(Err.TIMEOUT).
transact() delivers what has arrived first (so an old reply is not taken for the answer), sends, then waits.

Times are in the clock's unit: seconds of time.monotonic by default (clock=..., idle=... replace them, e.g. for tests).
"""

from __future__ import annotations

import asyncio
import enum
import inspect
import socket
import time
from dataclasses import dataclass
from typing import Any, Callable

from .core import to_bytes
from .errors import Err, GmpError, fail
from .ring import ByteRing
from .stream import Received, StreamParser, _key


@dataclass
class Reply:
    """A message with the header of its frame (wait_for_reply)."""

    header: Any
    message: Any


class RequestState(enum.IntEnum):
    """State of a Pending request (same values as gmsg::RequestState / gmsg_request_state)."""

    IDLE = 0     # not started, or cancelled
    WAITING = 1  # sent (or expected), waiting for the reply
    DONE = 2     # finished: the reply, or Err.TIMEOUT


class Pending:
    """A request that does not block (Endpoint.request / expect): later service() calls complete it with the reply or
    Err.TIMEOUT, then on_done(pending) runs once. result() returns the reply or raises the GmpError."""

    def __init__(self, what: Any, match: Callable | None = None, on_done: Callable[["Pending"], None] | None = None) -> None:
        self.what = what
        self.key = _key(what)
        self.on_done = on_done
        self.state = RequestState.IDLE
        self.error = Err.NONE
        self.reply: Any = None
        self._m = _matcher(match)
        self._ep: "Endpoint | None" = None
        self._start: float = 0
        self._timeout: float = 0
        self._candidate: Any = None

    @property
    def waiting(self) -> bool:
        return self.state == RequestState.WAITING

    @property
    def done(self) -> bool:
        return self.state == RequestState.DONE

    def result(self) -> Any:
        """The reply (the decoded message); raises GmpError(Err.TIMEOUT) when it timed out."""
        if not self.done:
            fail(Err.INVALID_VALUE, "the request is not done")
        if self.error != Err.NONE:
            raise GmpError(self.error, f"no {self.key} within {self._timeout}")
        return self.reply

    def cancel(self) -> None:
        """Stop waiting; no callback runs."""
        if self._ep is not None:
            self._ep._unlink(self)
        if self.state == RequestState.WAITING:
            self.state = RequestState.IDLE

    def __repr__(self) -> str:
        return f"Pending({self.key}, {self.state.name}{', ' + str(self.error) if self.error else ''})"


def _matcher(match: Callable | None) -> Callable[[Received], bool] | None:
    """match(view) or match(view, header), whichever it accepts."""
    if match is None:
        return None
    two = False
    try:
        params = [p for p in inspect.signature(match).parameters.values()
                  if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD, p.VAR_POSITIONAL)]
        two = len(params) >= 2 or any(p.kind == p.VAR_POSITIONAL for p in params)
    except (TypeError, ValueError):
        pass
    if two:
        return lambda f: bool(match(f.message, f.header))
    return lambda f: bool(match(f.message))


def _sleep_briefly() -> None:
    time.sleep(0.0005)


_POLICIES = ("whole", "retry")


class _Base:
    """What both endpoints share: the stream, listeners, policies."""

    def __init__(self, protocol: Any, capacity: int | None, send_policy: str, send_timeout: float | None) -> None:
        self.protocol = protocol
        self.stream = StreamParser(protocol, capacity)
        self.set_send_policy(send_policy, send_timeout)

    @property
    def stats(self):
        return self.stream.stats

    @property
    def last_error(self):
        return self.stream.last_error

    def on(self, what: Any, fn: Callable) -> Callable[[], None]:
        """Listen as on StreamParser: a message (name or class), 'frame' or 'error'. Returns a function removing it."""
        return self.stream.on(what, fn)

    def set_send_policy(self, policy: str, timeout: float | None = None) -> None:
        """'whole' (all or nothing) or 'retry' (until it is out; TIMEOUT after `timeout`, None: no limit)."""
        if policy not in _POLICIES:
            fail(Err.INVALID_VALUE, f"send policy {policy!r} (whole | retry)")
        self.send_policy = policy
        self.send_timeout = timeout

    def _encode(self, message: Any, header: Any) -> bytes:
        return self.protocol.encode(message) if header is None else self.protocol.encode(message, header)

    def _taker(self, what: Any, match: Callable | None, with_header: bool):
        """accept(frame) for StreamParser._step: decodes a matching frame (a failed decode raises: a bad frame)."""
        key = _key(what)
        m = _matcher(match)
        hit: list = []

        def accept(f: Received) -> None:
            if type(f.message).message_name != key or (m is not None and not m(f)):
                return
            message = f.message.to_struct()
            if with_header:
                hit.append(Reply(f.header.to_struct() if f.header is not None else None, message))
            else:
                hit.append(message)

        return accept, hit


class Endpoint(_Base):
    """The blocking endpoint (see the module docstring)."""

    def __init__(self, protocol: Any, source: Any = None, sink: Any = None, *, capacity: int | None = None,
                 clock: Callable[[], float] = time.monotonic, idle: Callable[[], None] | None = _sleep_briefly,
                 idle_timeout: float = 0, send_policy: str = "whole", send_timeout: float | None = None) -> None:
        super().__init__(protocol, capacity, send_policy, send_timeout)
        self.source = source
        self.sink = sink if sink is not None else (source if source is not None and _is_sink(source) else None)
        self.clock = clock
        self.idle = idle
        self._carry = b""
        self._pending: list[Pending] = []    # waiting, oldest first
        self._completed: list[Pending] = []  # finished, callbacks not run yet
        self._depth = 0                      # nested service() / waits
        self.set_idle_timeout(idle_timeout)

    def set_idle_timeout(self, t: float) -> None:
        """Give up an incomplete frame after `t` (clock units) without new bytes; 0 = off."""
        self.stream.set_idle_timeout(t)

    # ---- receive ----
    def service(self, max_bytes: int | None = None) -> int:
        """Read what the source has (at most max_bytes), deliver every frame to the listeners and run the idle
        resync. Returns the number of frames delivered."""
        before = self.stream.stats.frames
        self._depth += 1
        try:
            while self._next():
                pass
            total = 0
            while max_bytes is None or total < max_bytes:
                n = self._read_input(None if max_bytes is None else max_bytes - total)
                total += n
                while self._next():  # also reports an overrun the read took over
                    pass
                if not n:
                    break
            self.stream.tick(self.clock())
            self._expire()
        finally:
            self._depth -= 1
        return self.stream.stats.frames - before

    # ---- requests that do not block ----
    def request(self, request: Any, what: Any, timeout: float, match: Callable | None = None, header: Any = None,
                on_done: Callable[[Pending], None] | None = None) -> Pending:
        """Deliver what has arrived (not when called from a callback), send `request` and return at once. Later
        service() calls complete the returned Pending with the reply (`what` accepted by `match`) or Err.TIMEOUT
        after `timeout`. A send error is raised (nothing is waiting then)."""
        p = Pending(what, match, on_done)
        if self._depth == 0:
            self.service()
        self.send(request, header)
        self._arm(p, timeout)
        return p

    def expect(self, what: Any, timeout: float, match: Callable | None = None,
               on_done: Callable[[Pending], None] | None = None) -> Pending:
        """Wait for `what` without sending anything, without blocking (see request())."""
        p = Pending(what, match, on_done)
        self._arm(p, timeout)
        return p

    @property
    def pending(self) -> int:
        """Requests still waiting."""
        return len(self._pending)

    def wait_for(self, what: Any, timeout: float, match: Callable | None = None) -> Any:
        """Wait up to `timeout` for message `what` accepted by `match`; return it decoded."""
        return self._wait(what, timeout, match, False)

    def wait_for_reply(self, what: Any, timeout: float, match: Callable | None = None) -> Reply:
        """wait_for() returning Reply(header, message)."""
        return self._wait(what, timeout, match, True)

    def transact(self, request: Any, what: Any, timeout: float, match: Callable | None = None, header: Any = None) -> Any:
        """Deliver what has arrived, send `request` (with header values, if given), then wait_for(what)."""
        self.service()
        self.send(request, header)
        return self.wait_for(what, timeout, match)

    # ---- send ----
    def send(self, message: Any, header: Any = None) -> int:
        """Encode `message` and write it to the sink with the send policy. Returns the frame size."""
        return self.send_raw(self._encode(message, header))

    def send_raw(self, data: Any) -> int:
        """Write bytes as they are, with the send policy."""
        b = memoryview(to_bytes(data))
        total = len(b)
        if self.sink is None:
            fail(Err.BUFFER_FULL, "the endpoint has no sink")
        if self.send_policy == "whole":
            room = self._room()
            if room is not None and room < total:
                fail(Err.BUFFER_FULL, f"the sink cannot take {total} bytes now")
            if self._write(b) < total:
                fail(Err.BUFFER_FULL, "the sink took part of the frame")
            return total
        start = self.clock()
        while True:
            b = b[self._write(b):]
            if not len(b):
                return total
            if self.send_timeout is not None and self.clock() - start >= self.send_timeout:
                fail(Err.TIMEOUT, f"{len(b)} of {total} bytes not sent")
            self._idle()

    # ---- internals ----
    def _idle(self) -> None:
        if self.idle is not None:
            self.idle()

    def _wait(self, what: Any, timeout: float, match: Callable | None, with_header: bool) -> Any:
        take, hit = self._taker(what, match, with_header)
        taken: list[Pending] = []

        def accept(f: Received) -> None:
            take(f)
            if not hit:
                self._offer(f, taken)

        start = self.clock()
        expired = False  # after the limit: one last look at what the final read brought in
        self._depth += 1
        try:
            while True:
                while True:
                    taken.clear()
                    k, v = self.stream._step(accept)
                    if k == "need_more":
                        break
                    if k == "error":
                        self.stream._report(v)
                        continue
                    if hit:
                        self._fire()
                        return hit.pop()
                    if taken:
                        self._complete_reply(taken[0])
                    else:
                        self.stream._deliver(v)
                    self._fire()
                if expired:
                    fail(Err.TIMEOUT, f"no {_key(what)} within {timeout}")
                n = self._read_input()
                self.stream.tick(self.clock())
                self._expire()
                if self.clock() - start >= timeout:
                    expired = True
                elif not n:
                    self._idle()
        finally:
            self._depth -= 1

    def _next(self) -> bool:
        """One step; frames go to a waiting request before the listeners."""
        if not self._pending:
            more = self.stream.next()
            self._fire()
            return more
        taken: list[Pending] = []
        k, v = self.stream._step(lambda f: self._offer(f, taken))
        if k == "error":
            self.stream._report(v)
        elif k == "frame":
            if taken:
                self._complete_reply(taken[0])
            else:
                self.stream._deliver(v)
        self._fire()
        return k != "need_more"

    def _offer(self, f: Received, taken: list) -> None:
        for p in self._pending:
            if type(f.message).message_name == p.key and (p._m is None or p._m(f)):
                p._candidate = f.message.to_struct()  # a failed decode raises: the frame is dropped
                taken.append(p)
                return

    def _arm(self, p: Pending, timeout: float) -> None:
        p.state = RequestState.WAITING
        p.error = Err.NONE
        p.reply = None
        p._start = self.clock()
        p._timeout = timeout
        p._ep = self
        self._pending.append(p)

    def _unlink(self, p: Pending) -> None:
        if p in self._pending:
            self._pending.remove(p)
        p._ep = None

    def _complete_reply(self, p: Pending) -> None:
        self._unlink(p)
        p.reply, p._candidate = p._candidate, None
        p.state = RequestState.DONE
        p.error = Err.NONE
        self._completed.append(p)

    def _expire(self) -> None:
        now = self.clock()
        for p in list(self._pending):
            if now - p._start >= p._timeout:
                self._unlink(p)
                p.state = RequestState.DONE
                p.error = Err.TIMEOUT
                self._completed.append(p)
        self._fire()

    def _fire(self) -> None:
        while self._completed:
            p = self._completed.pop(0)
            if p.on_done is not None:
                p.on_done(p)

    def _read_input(self, max_bytes: int | None = None) -> int:
        """Move source bytes into the stream without processing them."""
        src = self.source
        if src is None:
            return 0
        if isinstance(src, ByteRing):
            return self.stream.push(src, max_bytes)
        take = getattr(src, "take_overrun", None)
        if take is not None and take():
            self.stream.push_gap()
        n = self._read_some(max_bytes)
        if not n and take is not None and take():
            self.stream.push_gap()  # the read found the loss: mark it now, then read what follows
            n = self._read_some(max_bytes)
        return n

    def _read_some(self, max_bytes: int | None) -> int:
        room = self.stream.space
        if max_bytes is not None:
            room = min(room, max_bytes)
        if room <= 0:
            return 0
        if self._carry:
            data, self._carry = self._carry[:room], self._carry[room:]
        else:
            data = self._read(room)
            if len(data) > room:  # a callable that returned more than asked: keep the rest
                data, self._carry = data[:room], bytes(data[room:])
        if not data:
            return 0
        self.stream.push(data)
        return len(data)

    def _read(self, n: int) -> bytes:
        src = self.source
        if callable(src) and not hasattr(src, "read") and not hasattr(src, "recv"):
            return to_bytes(src(n) or b"")
        if hasattr(src, "recv"):
            try:
                return src.recv(n) or b""
            except (BlockingIOError, InterruptedError, socket.timeout):
                return b""
        waiting = getattr(src, "in_waiting", None)
        if waiting is not None:
            if not waiting:
                return b""
            n = min(n, waiting)
        return src.read(n) or b""

    def _room(self) -> int | None:
        k = self.sink
        if isinstance(k, ByteRing):
            return k.room
        fn = getattr(k, "writable", None)
        if callable(fn):
            r = fn()
            if isinstance(r, int) and not isinstance(r, bool):
                return r
        return None

    def _write(self, b: memoryview) -> int:
        k = self.sink
        if isinstance(k, ByteRing):
            return k.push_bytes(b[:k.room])
        if callable(k) and not hasattr(k, "write") and not hasattr(k, "send"):
            r = k(bytes(b))
        elif hasattr(k, "send") and not hasattr(k, "write"):
            try:
                r = k.send(b)
            except (BlockingIOError, InterruptedError, socket.timeout):
                return 0
        else:
            r = k.write(b)
            flush = getattr(k, "flush", None)
            if callable(flush):
                flush()
        if r is None:
            return len(b)
        return max(0, min(int(r), len(b)))


def _is_sink(obj: Any) -> bool:
    """A source that is also the output: a serial port (in_waiting) or a socket (recv / send). Files and BytesIO are
    not: writing would go into the input."""
    return (hasattr(obj, "in_waiting") and hasattr(obj, "write")) or (hasattr(obj, "recv") and hasattr(obj, "send"))


class AsyncEndpoint(_Base):
    """The asyncio endpoint: input is read by a task (start(), or the first wait) and processed as it arrives; a frame
    that a pending wait accepts settles it, every other frame goes to the listeners.

    reader: an asyncio.StreamReader (or anything with `async read(n)`); writer: an asyncio.StreamWriter, an object
    with write(bytes) (and optionally `async drain()` / writable()), or a callable (sync or async)."""

    def __init__(self, protocol: Any, reader: Any = None, writer: Any = None, *, capacity: int | None = None,
                 idle_timeout: float = 0, send_policy: str = "whole", send_timeout: float | None = None,
                 retry_delay: float = 0.001, chunk: int = 4096) -> None:
        super().__init__(protocol, capacity, send_policy, send_timeout)
        self.reader = reader
        self.writer = writer
        self.retry_delay = retry_delay
        self.chunk = chunk
        self._waiters: list[tuple[Callable, list, asyncio.Future]] = []
        self._task: asyncio.Task | None = None
        self._idle_handle: asyncio.TimerHandle | None = None
        self._closed = False
        self.set_idle_timeout(idle_timeout)

    async def __aenter__(self) -> "AsyncEndpoint":
        self.start()
        return self

    async def __aexit__(self, *exc: Any) -> None:
        await self.close()

    def set_idle_timeout(self, t: float) -> None:
        """Give up an incomplete frame after `t` seconds without input; 0 = off."""
        self.stream.set_idle_timeout(t)
        self._arm_idle()

    # ---- input ----
    def receive(self, data: Any) -> int:
        """Process a chunk of input (bytes, or a ByteRing to drain). Returns the number of frames that arrived."""
        before = self.stream.stats.frames
        if isinstance(data, ByteRing):
            while True:
                n = self.stream.push(data)
                self._process()
                if not n:
                    break
        else:
            b = memoryview(to_bytes(data))
            while True:
                n = self.stream.push(b)
                b = b[n:]
                self._process()
                if not len(b):
                    break
        self.stream.tick(_now())
        self._arm_idle()
        return self.stream.stats.frames - before

    def start(self) -> None:
        """Start the task that reads the reader (also started by the first wait)."""
        if self._task is None and self.reader is not None and not self._closed:
            self._task = asyncio.get_running_loop().create_task(self._read_loop())

    async def close(self) -> None:
        """Stop reading, cancel the idle timer and fail the pending waits."""
        self._closed = True
        if self._idle_handle is not None:
            self._idle_handle.cancel()
            self._idle_handle = None
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
            self._task = None
        for _, _, fut in self._waiters:
            if not fut.done():
                fut.set_exception(GmpError(Err.INVALID_VALUE, "endpoint closed"))
        self._waiters.clear()

    # ---- waiting ----
    async def wait_for(self, what: Any, timeout: float | None = None, match: Callable | None = None) -> Any:
        """The next message `what` accepted by `match`, decoded; GmpError(Err.TIMEOUT) after `timeout` seconds."""
        return await self._settle(self._add_waiter(what, match, False), what, timeout)

    async def wait_for_reply(self, what: Any, timeout: float | None = None, match: Callable | None = None) -> Reply:
        """wait_for() returning Reply(header, message)."""
        return await self._settle(self._add_waiter(what, match, True), what, timeout)

    async def transact(self, request: Any, what: Any, timeout: float | None = None, match: Callable | None = None,
                       header: Any = None) -> Any:
        """Send `request`, then wait for `what` (timeout counted from the send). A reply arriving while the request
        is being written is not missed."""
        w = self._add_waiter(what, match, False)
        try:
            await self.send(request, header)
        except BaseException:
            self._drop(w)
            raise
        return await self._settle(w, what, timeout)

    # ---- send ----
    async def send(self, message: Any, header: Any = None) -> int:
        """Encode `message` and write it with the send policy. Returns the frame size."""
        return await self.send_raw(self._encode(message, header))

    async def send_raw(self, data: Any) -> int:
        """Write bytes as they are, with the send policy."""
        b = memoryview(to_bytes(data))
        total = len(b)
        k = self.writer
        if k is None:
            fail(Err.BUFFER_FULL, "the endpoint has no writer")
        stream_writer = hasattr(k, "drain") and hasattr(k, "transport")
        if self.send_policy == "whole":
            if stream_writer:
                t = k.transport
                if t.is_closing() or t.get_write_buffer_size() > t.get_write_buffer_limits()[1]:
                    fail(Err.BUFFER_FULL, "the writer's buffer is full")
                k.write(bytes(b))
                return total
            room = self._room()
            if room is not None and room < total:
                fail(Err.BUFFER_FULL, f"the writer cannot take {total} bytes now")
            if await self._write(b) < total:
                fail(Err.BUFFER_FULL, "the writer took part of the frame")
            return total
        loop = asyncio.get_running_loop()
        deadline = None if self.send_timeout is None else loop.time() + self.send_timeout
        if stream_writer:
            k.write(bytes(b))
            try:
                await asyncio.wait_for(k.drain(), None if deadline is None else max(0.0, deadline - loop.time()))
            except asyncio.TimeoutError:
                fail(Err.TIMEOUT, f"{total} bytes not drained within {self.send_timeout}")
            return total
        while True:
            b = b[await self._write(b):]
            if not len(b):
                return total
            if deadline is not None and loop.time() >= deadline:
                fail(Err.TIMEOUT, f"{len(b)} of {total} bytes not sent")
            await asyncio.sleep(self.retry_delay)

    # ---- internals ----
    def _room(self) -> int | None:
        k = self.writer
        if isinstance(k, ByteRing):
            return k.room
        fn = getattr(k, "writable", None)
        if callable(fn):
            r = fn()
            if isinstance(r, int) and not isinstance(r, bool):
                return r
        return None

    async def _write(self, b: memoryview) -> int:
        k = self.writer
        if isinstance(k, ByteRing):
            return k.push_bytes(b[:k.room])
        r = k(bytes(b)) if callable(k) and not hasattr(k, "write") else k.write(bytes(b))
        if inspect.isawaitable(r):
            r = await r
        if r is None:
            return len(b)
        return max(0, min(int(r), len(b)))

    def _add_waiter(self, what: Any, match: Callable | None, with_header: bool):
        loop = asyncio.get_running_loop()
        accept, hit = self._taker(what, match, with_header)
        w = (accept, hit, loop.create_future())
        if self._closed:
            w[2].set_exception(GmpError(Err.INVALID_VALUE, "endpoint closed"))
        else:
            self._waiters.append(w)
            self.start()
        return w

    def _drop(self, w) -> None:
        if w in self._waiters:
            self._waiters.remove(w)

    async def _settle(self, w, what: Any, timeout: float | None) -> Any:
        try:
            if timeout is None:
                return await w[2]
            return await asyncio.wait_for(asyncio.shield(w[2]), timeout)
        except asyncio.TimeoutError:
            self._drop(w)
            if w[2].done() and not w[2].cancelled() and w[2].exception() is None:
                return w[2].result()  # it arrived just as the time ran out
            raise GmpError(Err.TIMEOUT, f"no {_key(what)} within {timeout} s") from None

    def _process(self) -> None:
        """Deliver every complete frame: to the first waiter accepting it, else to the listeners."""
        while True:
            taken: list = []

            def accept(f: Received) -> None:
                for w in self._waiters:
                    w[0](f)
                    if w[1]:
                        taken.append(w)
                        return

            k, v = self.stream._step(accept)
            if k == "need_more":
                return
            if k == "error":
                self.stream._report(v)
                continue
            if taken:
                w = taken[0]
                self._drop(w)
                if not w[2].done():
                    w[2].set_result(w[1].pop())
            else:
                self.stream._deliver(v)

    async def _read_loop(self) -> None:
        while not self._closed:
            data = await self.reader.read(self.chunk)
            if not data:
                break
            self.receive(data)

    def _arm_idle(self) -> None:
        if self._idle_handle is not None:
            self._idle_handle.cancel()
            self._idle_handle = None
        t = self.stream.idle_timeout
        if not t or self._closed or not self.stream.buffered:
            return
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            return
        wait = max(0.001, self.stream._last_activity + t - _now())

        def fire() -> None:
            self._idle_handle = None
            self.stream.tick(_now())
            self._process()
            self._arm_idle()

        self._idle_handle = loop.call_later(wait, fire)


def _now() -> float:
    return time.monotonic()
