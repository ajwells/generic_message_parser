"""gmsg.Endpoint (blocking) and gmsg.AsyncEndpoint (asyncio) on generated protocols: service, wait_for /
wait_for_reply / transact, send with both send policies and the kinds of source and sink, the idle resync, overruns
(take_overrun, push_gap) and close().

    python tests/roundtrip_py/test_endpoint.py <directory with the generated *_gmp.py> [--pure | --native] [-v]
"""

from __future__ import annotations

import asyncio
import io
import os
import socket
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
gen_dir = os.path.abspath(next(a for a in sys.argv[1:] if not a.startswith("-")))
sys.path[:0] = [HERE, os.path.join(HERE, "..", "runtime_py"), gen_dir,
                os.environ.get("GMSG_PATH") or os.path.join(HERE, "..", "..", "runtime-py")]

import gmsg  # noqa: E402
import harness as T  # noqa: E402
from gmsg import Err  # noqa: E402

if "--pure" in sys.argv:
    gmsg.use_native(False)
if "--native" in sys.argv and not gmsg.use_native(True):
    print("FAIL: --native given but gmsg._native cannot be imported")
    sys.exit(1)

import nmea_gmp as nm  # noqa: E402
import telemetry_gmp as tel  # noqa: E402

Link = tel.BinaryLink


def status(v: int) -> tel.Status:
    return tel.Status(voltage=v, label="s")


def command(op: int) -> tel.Command:
    return tel.Command(opcode=op)


def log_msg(level: int) -> tel.Log:
    return tel.Log(level=level, text="t")


def frame(m, header=None) -> bytes:
    return Link.encode(m, header)


def code_of(fn) -> int:
    try:
        fn()
        return 0
    except gmsg.GmpError as e:
        return int(e.code)


class Clock:
    """A clock the idle hook advances by `step`."""

    def __init__(self) -> None:
        self.t = 0
        self.step = 1
        self.idles = 0

    def __call__(self) -> int:
        return self.t

    def idle(self) -> None:
        self.t += self.step
        self.idles += 1


class Chunks:
    """A pyserial-like source: in_waiting / read(n), at most `chunk` bytes per read."""

    def __init__(self, data: bytes = b"", chunk: int = 0) -> None:
        self.data = bytearray(data)
        self.chunk = chunk

    @property
    def in_waiting(self) -> int:
        return len(self.data) if not self.chunk else min(self.chunk, len(self.data))

    def read(self, n: int) -> bytes:
        out = bytes(self.data[:n])
        del self.data[:n]
        return out

    def add(self, b: bytes) -> None:
        self.data += b


def recorder(ep) -> list[str]:
    ev: list[str] = []
    ep.on(tel.Status, lambda v, h: ev.append(f"status {v.voltage}"))
    ep.on(tel.Command, lambda v, h: ev.append(f"command {v.opcode}"))
    ep.on(tel.Log, lambda v, h: ev.append(f"log {v.level}"))
    ep.on("error", lambda e: ev.append(f"error {gmsg.error_text(e.code)}"))
    return ev


# ---- blocking endpoint ------------------------------------------------------------------------------------------

@T.test("service(): reads a source in chunks and delivers every frame to the listeners")
def _():
    w = frame(status(100)) + b"\x13\x37" + frame(command(7)) + frame(log_msg(2))
    src = Chunks(w, chunk=5)
    ep = Link.endpoint(src, clock=Clock())
    ev = recorder(ep)
    T.eq(ep.service(), 3)
    T.eq(ev, ["status 100", "error bad sync", "command 7", "log 2"])
    T.eq(len(src.data), 0)
    T.eq(ep.service(), 0)
    # max_bytes
    src2 = Chunks(frame(status(1)) + frame(status(2)))
    ep2 = Link.endpoint(src2, clock=Clock())
    T.eq(ep2.service(len(frame(status(1))) + 1), 1)
    T.eq(ep2.service(), 1)


@T.test("service(): idle resync with the clock")
def _():
    bad = bytearray(frame(status(5))[:10])
    bad[3], bad[4] = 0, 200  # a length that never arrives
    clk = Clock()
    ep = Link.endpoint(io.BytesIO(bytes(bad) + frame(status(6))), clock=clk, idle_timeout=10)
    ev = recorder(ep)
    ep.service()
    clk.t = 5
    ep.service()
    T.eq(ev, [])
    clk.t = 20
    ep.service()
    ep.service()
    T.eq(ev[0], "error truncated")
    T.eq(ev[-1], "status 6")


@T.test("send(): whole frame or nothing; a sink without room info; retry; retry timeout; raw bytes")
def _():
    class Room:
        def __init__(self, room: int) -> None:
            self.room, self.out = room, bytearray()

        def writable(self) -> int:
            return self.room

        def write(self, b) -> int:
            self.out += b
            self.room -= len(b)
            return len(b)

    f = frame(command(9))
    sink = Room(64)
    ep = Link.endpoint(None, sink, clock=Clock())
    T.eq(ep.send(command(9)), len(f))
    T.eq(bytes(sink.out), f)
    sink.room = 4
    T.eq(code_of(lambda: ep.send(command(9))), Err.BUFFER_FULL)
    T.eq(len(sink.out), len(f), "nothing written")
    T.eq(code_of(lambda: Link.endpoint().send(command(1))), Err.BUFFER_FULL)

    class Slow:
        def __init__(self) -> None:
            self.out = bytearray()

        def write(self, b) -> int:
            self.out += b[:3]
            return min(3, len(b))

    slow, clk = Slow(), Clock()
    ep2 = Link.endpoint(None, slow, clock=clk, idle=clk.idle)
    T.eq(code_of(lambda: ep2.send(status(3))), Err.BUFFER_FULL)
    T.eq(len(slow.out), 3)
    slow.out = bytearray()
    ep2.set_send_policy("retry")
    T.eq(ep2.send(status(3)), len(frame(status(3))))
    T.eq(bytes(slow.out), frame(status(3)))
    T.eq(clk.idles, (len(frame(status(3))) + 2) // 3 - 1)

    class Stuck:
        def __init__(self) -> None:
            self.n = 0

        def write(self, b) -> int:
            if self.n >= 4:
                return 0
            k = min(2, len(b))
            self.n += k
            return k

    clk = Clock()
    ep3 = Link.endpoint(None, Stuck(), clock=clk, idle=clk.idle, send_policy="retry", send_timeout=50)
    T.eq(code_of(lambda: ep3.send(command(1))), Err.TIMEOUT)
    T.eq(clk.t, 50)
    T.eq(code_of(lambda: ep3.set_send_policy("later")), Err.INVALID_VALUE)
    out = io.BytesIO()
    ep4 = Link.endpoint(None, out)
    T.eq(ep4.send_raw(b"\xfd"), 1)
    T.eq(out.getvalue(), b"\xfd")
    # a callable sink
    got = []
    T.eq(Link.endpoint(None, got.append).send(command(2)), len(frame(command(2))))
    T.eq(got, [frame(command(2))])


@T.test("wait_for(): other messages to the listeners, the awaited one returned; match; timeout")
def _():
    src = Chunks(frame(status(1)) + frame(log_msg(4)) + frame(command(42)) + frame(status(2)), chunk=7)
    clk = Clock()
    ep = Link.endpoint(src, clock=clk, idle=clk.idle)
    ev = recorder(ep)
    c = ep.wait_for(tel.Command, 100)
    T.eq(type(c), tel.Command)
    T.eq(c.opcode, 42)
    T.eq(ev, ["status 1", "log 4"])
    T.ok(len(src.data) > 0, "reading stopped soon after the command")
    s = ep.wait_for("Status", 100, match=lambda v: v.voltage == 2)
    T.eq(s.voltage, 2)
    start = clk.t
    T.eq(code_of(lambda: ep.wait_for(tel.Status, 30)), Err.TIMEOUT)
    T.eq(clk.t - start, 30)
    # timeout 0 still takes a message that is readable now
    T.eq(Link.endpoint(io.BytesIO(frame(log_msg(9))), clock=Clock()).wait_for(tel.Log, 0).level, 9)
    # match with the header; wait_for_reply
    h = tel.BinaryLinkHeader(msg_id=tel.MsgId.status)
    ep2 = Link.endpoint(io.BytesIO(frame(status(6)) + frame(status(7), h)), clock=Clock())
    seen = []
    r = ep2.wait_for_reply(tel.Status, 10, match=lambda v, hv: seen.append(hv.msg_id) or v.voltage == 7)
    T.eq(r.message.voltage, 7)
    T.eq(r.header.msg_id, tel.MsgId.status)
    T.eq(r.header.len, len(frame(status(7))) - 2 - 3 - 2)
    T.eq(len(seen), 2)


@T.test("transact(): through a simulated device, over a ByteRing and over a socket pair")
def _():
    rx = gmsg.ByteRing(256)
    rx.push_bytes(frame(status(999)))  # an old reply

    def device(b: bytes) -> int:
        rx.push_bytes(frame(status(Link.decode(b).opcode)))
        return len(b)

    clk = Clock()
    ep = Link.endpoint(rx, device, clock=clk, idle=clk.idle)
    ev = recorder(ep)
    T.eq(ep.transact(command(17), tel.Status, 20).voltage, 17)
    T.eq(ev, ["status 999"])
    T.eq(ep.transact(command(23), tel.Status, 20, header=tel.BinaryLinkHeader()).voltage, 23)
    silent = Link.endpoint(None, lambda b: len(b), clock=clk, idle=clk.idle)
    T.eq(code_of(lambda: silent.transact(command(1), tel.Status, 5)), Err.TIMEOUT)
    before = clk.t
    T.eq(code_of(lambda: Link.endpoint(clock=clk, idle=clk.idle).transact(command(1), tel.Status, 5)), Err.BUFFER_FULL)
    T.eq(clk.t, before, "not waited for")
    # a real socket pair with a device thread (non-blocking client socket: the source and the sink)
    a, b = socket.socketpair()
    a.setblocking(False)

    def serve() -> None:
        p = Link.stream()
        p.on(tel.Command, lambda v, h: b.sendall(frame(status(v.opcode + 1000))))
        while True:
            data = b.recv(256)
            if not data:
                return
            p.feed(data)

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    ep2 = Link.endpoint(a, idle_timeout=0.5)
    T.eq(ep2.transact(command(5), tel.Status, 5.0).voltage, 1005)
    T.eq(code_of(lambda: ep2.wait_for(tel.Command, 0.02)), Err.TIMEOUT)
    a.close()
    t.join(2)
    b.close()


@T.test("overruns: a source with take_overrun(); a ByteRing that overflows; push_gap")
def _():
    class Lossy(Chunks):
        def __init__(self) -> None:
            super().__init__()
            self.lost = False

        def take_overrun(self) -> bool:
            lost, self.lost = self.lost, False
            return lost

    src = Lossy()
    ep = Link.endpoint(src, clock=Clock())
    ev = recorder(ep)
    f = frame(status(1))
    src.add(f[:8])
    ep.service()
    src.lost = True
    src.add(frame(status(2)))
    ep.service()
    T.eq(ev, ["error overrun", "status 2"])
    ring = gmsg.ByteRing(32)
    ep2 = Link.endpoint(ring, clock=Clock())
    ev2 = recorder(ep2)
    ring.push_bytes(frame(status(1)) + frame(status(2)))  # 42 bytes into 32
    ep2.service()
    ring.push_bytes(frame(status(3)))
    ep2.service()
    T.eq(ev2, ["status 1", "error overrun", "status 3"])
    T.eq(ring.room, 32)
    # the parser itself
    p = Link.stream()
    seen = []
    p.on("frame", lambda v, h: seen.append(v.voltage))
    p.on("error", lambda e: seen.append(gmsg.error_text(e.code)))
    T.eq(p.space, p.capacity)
    p.push(frame(status(1)) + frame(status(2))[:4])
    T.eq(p.push_gap(), True)
    T.eq(p.push_gap(), True)
    p.push(frame(status(2)))
    T.eq(p.push_gap(), False)
    while p.next():
        pass
    T.eq(seen, [1, "overrun", 2])


@T.test("text protocol endpoint (NMEA)")
def _():
    gga = b"$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"
    txt = b"$GPTXT,01,01,02,ANTSTATUS=OK*3B\r\n"
    out = io.BytesIO()
    ep = nm.Nmea.endpoint(Chunks(gga + b"noise" + txt, chunk=9), out, clock=Clock())
    ggas = []
    ep.on(nm.GGA, lambda v, h: ggas.append(v))
    m = ep.wait_for(nm.TXT, 10)
    T.eq(len(ggas), 1)
    ep.send(m)
    T.eq(out.getvalue(), txt)


@T.test("request(): returns at once; service() completes it; timeout; several; chained from on_done; during wait_for")
def _():
    rx = gmsg.ByteRing(256)
    sent = []
    clk = Clock()
    ep = Link.endpoint(rx, lambda b: sent.append(b) or len(b), clock=clk, idle=clk.idle)
    ev = recorder(ep)
    rx.push_bytes(frame(status(999)))
    done = []
    p = ep.request(command(17), tel.Status, 50, on_done=lambda q: done.append(q.result().voltage))
    T.eq(len(sent), 1)
    T.eq(p.state, gmsg.RequestState.WAITING)
    T.eq(ev, ["status 999"])
    ep.service()
    T.eq(p.waiting, True)
    rx.push_bytes(frame(log_msg(1)) + frame(status(17)) + frame(status(18)))
    ep.service()
    T.eq(p.done, True)
    T.eq(p.result().voltage, 17)
    T.eq(done, [17])
    T.eq(ev, ["status 999", "log 1", "status 18"])
    T.eq(ep.pending, 0)
    # timeout and cancel
    t = ep.request(command(1), tel.Status, 10)
    clk.t += 9
    ep.service()
    T.eq(t.waiting, True)
    clk.t += 1
    ep.service()
    T.eq(t.error, Err.TIMEOUT)
    T.eq(code_of(t.result), Err.TIMEOUT)
    c = ep.expect(tel.Status, 10, on_done=lambda q: done.append("cancelled?"))
    c.cancel()
    clk.t += 100
    ep.service()
    T.eq(c.state, gmsg.RequestState.IDLE)
    T.eq(done, [17])
    T.eq(code_of(lambda: Link.endpoint(clock=clk).request(command(1), tel.Status, 1)), Err.BUFFER_FULL)
    # several at once, oldest first; match; expect without sending
    seven = ep.request(command(2), tel.Status, 100, match=lambda v: v.voltage == 7)
    first = ep.request(command(3), tel.Status, 100)
    second = ep.request(command(4), tel.Status, 100)
    cmd = ep.expect(tel.Command, 100)
    T.eq(ep.pending, 4)
    rx.push_bytes(frame(status(5)) + frame(status(7)) + frame(status(6)) + frame(command(8)) + frame(status(9)))
    ep.service()
    T.eq([seven.result().voltage, first.result().voltage, second.result().voltage, cmd.result().opcode], [7, 5, 6, 8])
    T.eq(ev[-1], "status 9")
    # a callback that starts the next request (no drain from inside a callback)
    replies = []

    def chain(q):
        replies.append(q.result().voltage)
        if len(replies) < 3:
            ep.request(command(q.result().voltage + 1), tel.Status, 100, on_done=chain)

    ep.request(command(1), tel.Status, 100, on_done=chain)
    rx.push_bytes(frame(status(1)) + frame(status(2)) + frame(status(3)))
    ep.service()
    T.eq(replies, [1, 2, 3])
    # requests are served while a blocking wait runs
    lg = ep.expect(tel.Log, 1000)
    st = ep.expect(tel.Status, 5)
    rx.push_bytes(frame(log_msg(3)) + frame(command(4)))
    T.eq(ep.wait_for(tel.Command, 100).opcode, 4)
    T.eq(lg.result().level, 3)
    T.eq(code_of(lambda: ep.wait_for(tel.Command, 10)), Err.TIMEOUT)
    T.eq(st.error, Err.TIMEOUT)


# ---- asyncio endpoint -------------------------------------------------------------------------------------------

class FakeWriter:
    def __init__(self) -> None:
        self.out = bytearray()

    def write(self, b) -> None:
        self.out += b


def run(coro):
    return asyncio.run(coro)


@T.test("AsyncEndpoint: wait_for / wait_for_reply / transact over an asyncio StreamReader")
def _():
    async def main():
        reader = asyncio.StreamReader()
        ep = Link.async_endpoint(reader, FakeWriter())
        ev = recorder(ep)
        w = asyncio.ensure_future(ep.wait_for(tel.Command, 1.0))
        await asyncio.sleep(0)
        reader.feed_data(frame(status(1)) + frame(log_msg(4)) + frame(command(42)))
        c = await w
        T.eq(c.opcode, 42)
        T.eq(ev, ["status 1", "log 4"])
        r = asyncio.ensure_future(ep.wait_for_reply(tel.Status, 1.0, match=lambda v, h: v.voltage == 7))
        await asyncio.sleep(0)
        reader.feed_data(frame(status(6)) + frame(status(7)))
        got = await r
        T.eq(got.message.voltage, 7)
        T.eq(got.header.msg_id, tel.MsgId.status)
        T.eq(ev[-1], "status 6")
        t0 = asyncio.get_running_loop().time()
        try:
            await ep.wait_for(tel.Status, 0.02)
            T.ok(False, "expected a timeout")
        except gmsg.GmpError as e:
            T.eq(e.code, Err.TIMEOUT)
        T.ok(asyncio.get_running_loop().time() - t0 >= 0.015)
        T.eq(len(ep._waiters), 0)
        # two waits for one type: first come, first served
        a = asyncio.ensure_future(ep.wait_for(tel.Log))
        b = asyncio.ensure_future(ep.wait_for(tel.Log))
        await asyncio.sleep(0)
        ep.receive(frame(log_msg(1)) + frame(log_msg(2)))
        T.eq((await a).level, 1)
        T.eq((await b).level, 2)
        await ep.close()
        try:
            await ep.wait_for(tel.Status)
            T.ok(False, "expected closed")
        except gmsg.GmpError as e:
            T.eq(e.code, Err.INVALID_VALUE)

    run(main())


@T.test("AsyncEndpoint: transact with a device; send policies; idle timeout")
def _():
    async def main():
        ep = None

        def device(b: bytes) -> int:  # answers while the request is being written
            ep.receive(frame(status(Link.decode(b).opcode)))
            return len(b)

        ep = Link.async_endpoint(None, device)
        ev = recorder(ep)
        ep.receive(frame(status(999)))
        T.eq((await ep.transact(command(17), tel.Status, 0.5)).voltage, 17)
        T.eq(ev, ["status 999"])
        silent = Link.async_endpoint(None, lambda b: None)
        try:
            await silent.transact(command(1), tel.Status, 0.01)
            T.ok(False, "expected a timeout")
        except gmsg.GmpError as e:
            T.eq(e.code, Err.TIMEOUT)
        none = Link.async_endpoint()
        try:
            await none.transact(command(1), tel.Status, 0.01)
        except gmsg.GmpError as e:
            T.eq(e.code, Err.BUFFER_FULL)
        T.eq(len(none._waiters), 0)

        class Slow:
            def __init__(self) -> None:
                self.out = bytearray()

            async def write(self, b) -> int:
                self.out += b[:3]
                return min(3, len(b))

        slow = Slow()
        ep2 = Link.async_endpoint(None, slow)
        try:
            await ep2.send(status(3))
        except gmsg.GmpError as e:
            T.eq(e.code, Err.BUFFER_FULL)
        slow.out = bytearray()
        ep2.set_send_policy("retry", 1.0)
        T.eq(await ep2.send(status(3)), len(frame(status(3))))
        T.eq(bytes(slow.out), frame(status(3)))
        # idle timeout: a stuck frame is given up without new input
        ep3 = Link.async_endpoint(idle_timeout=0.01)
        ev3 = recorder(ep3)
        bad = bytearray(frame(status(5))[:10])
        bad[3], bad[4] = 0, 200
        ep3.receive(bytes(bad) + frame(status(6)))
        T.eq(ev3, [])
        await asyncio.sleep(0.05)
        T.eq(ev3[0], "error truncated")
        T.eq(ev3[-1], "status 6")
        await ep3.close()

    run(main())


@T.test("AsyncEndpoint over a real connection (asyncio streams)")
def _():
    async def main():
        async def handle(reader, writer):
            p = Link.stream()
            p.on(tel.Command, lambda v, h: writer.write(frame(status(v.opcode + 1000))))
            while data := await reader.read(256):
                p.feed(data)
            writer.close()

        server = await asyncio.start_server(handle, "127.0.0.1", 0)
        port = server.sockets[0].getsockname()[1]
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        async with Link.async_endpoint(reader, writer, send_policy="retry", send_timeout=1.0) as ep:
            T.eq((await ep.transact(command(5), tel.Status, 2.0)).voltage, 1005)
            T.eq((await ep.transact(command(6), tel.Status, 2.0)).voltage, 1006)
        writer.close()
        server.close()
        await server.wait_closed()

    run(main())


sys.exit(T.run())
