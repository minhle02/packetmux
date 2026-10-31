"""M10: what a bulk transfer costs the stream beside it, and what the SDK costs a bulk transfer.

Separate processes over a loopback WebSocket, because that is the shape of a real deployment. Each
records the queues a delay could sit in: the transport buffer, the kernel socket buffers, and how
late its event loop runs.

Stacks: "sdk" moves bytes straight between handlers. "tunnel" adds a TCP socket pump at each end, as
a port forward has, with a target process and a driver process around it. "tcp" is the driver talking
to the target with no tunnel. "thin" is a minimal host over the sync core, as a baseline for the SDK.

    PYTHONPATH=. PACKETMUX_LIBRARY=<release dylib> uv run python bench/latency.py matrix out.jsonl

Socket queue sizes use SO_NWRITE and SO_NREAD, so this runs on macOS.
"""

import argparse
import asyncio
import json
import os
import resource
import signal
import socket
import subprocess
import sys
import time
from typing import Any

from packetmux import (
    ClientOptions,
    IncomingStream,
    Server,
    ServerOptions,
    StreamReset,
    Transport,
    TransportClosed,
    connect,
)
from packetmux import core
from packetmux.core import EventType, Role
from websockets.asyncio.client import connect as ws_connect
from websockets.asyncio.server import serve as ws_serve
from websockets.exceptions import ConnectionClosed

CHUNK = bytes(64 * 1024)
SO_NREAD = 0x1020
SO_NWRITE = 0x1024
MB = 1_000_000


class BenchTransport(Transport):
    def __init__(self, socket_: Any, wait: str) -> None:
        self._socket = socket_
        self._wait = wait
        self.waits = 0

    async def send(self, message: bytes) -> None:
        try:
            await self._socket.send(message)
        except ConnectionClosed as error:
            raise TransportClosed() from error

    async def recv(self) -> bytes:
        try:
            return await self._socket.recv()
        except ConnectionClosed as error:
            raise TransportClosed() from error

    @property
    def buffered_amount(self) -> int:
        return self._socket.transport.get_write_buffer_size()

    async def wait_buffer_below(self, limit: int) -> None:
        if self._socket.close_code is not None:
            raise TransportClosed()
        while self.buffered_amount >= limit:
            self.waits += 1
            if self._wait == "poll":
                await asyncio.sleep(0.002)  # the default in Transport
            else:
                await self._socket.drain()

    async def close(self) -> None:
        await self._socket.close()


class Probe:
    """Samples, once per ms, how late the loop runs and how much is queued."""

    def __init__(self) -> None:
        self.reset()
        self.transport: BenchTransport | None = None
        self.raw_socket: socket.socket | None = None

    def reset(self) -> None:
        self.lag: list[float] = []
        self.kernel_write: list[int] = []
        self.kernel_read: list[int] = []
        self.buffered: list[int] = []
        self.waits_at_start = self.transport.waits if getattr(self, "transport", None) else 0
        self.cpu_at_start = cpu_seconds()

    async def run(self) -> None:
        while True:
            started = time.perf_counter()
            await asyncio.sleep(0.001)
            self.lag.append((time.perf_counter() - started - 0.001) * 1000)
            if self.raw_socket is not None and self.raw_socket.fileno() != -1:
                self.kernel_write.append(self.raw_socket.getsockopt(socket.SOL_SOCKET, SO_NWRITE))
                self.kernel_read.append(self.raw_socket.getsockopt(socket.SOL_SOCKET, SO_NREAD))
            if self.transport is not None:
                self.buffered.append(self.transport.buffered_amount)

    def report(self, prefix: str) -> dict[str, float]:
        waits = self.transport.waits - self.waits_at_start if self.transport else 0
        return {
            f"{prefix}_lag_p50_ms": percentile(self.lag, 50),
            f"{prefix}_lag_p99_ms": percentile(self.lag, 99),
            f"{prefix}_kernel_write_p50_kb": percentile(self.kernel_write, 50) / 1000,
            f"{prefix}_kernel_read_p50_kb": percentile(self.kernel_read, 50) / 1000,
            f"{prefix}_buffered_p50_kb": percentile(self.buffered, 50) / 1000,
            f"{prefix}_waits": waits,
            f"{prefix}_cpu_s": cpu_seconds() - self.cpu_at_start,
        }


def cpu_seconds() -> float:
    usage = resource.getrusage(resource.RUSAGE_SELF)
    return usage.ru_utime + usage.ru_stime


def percentile(values: list[float], percent: int) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, len(ordered) * percent // 100)]


def session_config(window: int) -> dict[str, int]:
    return {"receive_window": window} if window else {}


def bench_socket(socket_buffer: int) -> socket.socket:
    """Options go on before connect, so they apply from the first window."""
    sock = socket.socket()
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if socket_buffer:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, socket_buffer)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, socket_buffer)
    return sock


async def pace(sent: int, rate: float, started: float) -> None:
    if rate:
        ahead = sent / rate - (time.perf_counter() - started)
        if ahead > 0:
            await asyncio.sleep(ahead)
    else:
        await asyncio.sleep(0)  # as a socket pump would, between reads


async def write_bulk(stream: Any, size: int, rate: float) -> None:
    started = time.perf_counter()
    sent = 0
    while sent < size:
        await stream.write(CHUNK)
        sent += len(CHUNK)
        await pace(sent, rate, started)


# --- SDK ---------------------------------------------------------------------------------------


def sdk_handlers(probe: Probe, sinks: list[Any]) -> list[Any]:
    async def source(request: IncomingStream) -> None:
        settings = json.loads(request.params)
        await write_bulk(request.accept(), settings["size"], settings["rate"])

    async def sink(request: IncomingStream) -> None:
        stream = request.accept()
        sinks.append(stream)
        async for _ in stream:
            pass
        await stream.write(b"done")

    async def echo(request: IncomingStream) -> None:
        stream = request.accept()
        async for chunk in stream:
            await stream.write(chunk)

    async def stats(request: IncomingStream) -> None:
        stream = request.accept()
        async for command in stream:
            if command == b"reset":
                probe.reset()
                sinks.clear()
                await stream.write(b"{}")
            else:
                report = probe.report("server")
                report["server_read_high_water_kb"] = sinks[-1].read_high_water / 1000 if sinks else 0
                await stream.write(json.dumps(report).encode())

    async def forward(request: IncomingStream) -> None:
        stream = request.accept()
        reader, writer = await asyncio.open_connection("127.0.0.1", int(request.params))
        sinks.append(stream)
        await pump(stream, reader, writer)

    return [
        ("source", source), ("sink", sink), ("echo", echo), ("stats", stats), ("forward", forward),
    ]


async def sdk_serve(port: int, wait: str, socket_buffer: int, inflight: int, quiet: bool, window: int) -> None:
    probe = Probe()
    sinks: list[Any] = []
    server = Server(
        handlers=sdk_handlers(probe, sinks),
        options=ServerOptions(inflight_limit=inflight, session=session_config(window)),
    )

    async def on_socket(connection: Any) -> None:
        transport = BenchTransport(connection, wait)
        probe.transport = transport
        probe.raw_socket = connection.transport.get_extra_info("socket")
        await server.serve(transport)

    sock = bench_socket(socket_buffer)
    sock.bind(("127.0.0.1", port))
    sock.listen(16)
    sock.setblocking(False)
    if not quiet:
        asyncio.create_task(probe.run())
    async with ws_serve(on_socket, sock=sock, max_size=None, compression=None, ping_interval=None):
        await asyncio.Event().wait()


async def round_trips(client: Any, samples: list[float], stop: asyncio.Event) -> None:
    stream = await client.open_stream("echo")
    message = bytes(64)
    while not stop.is_set():
        started = time.perf_counter()
        await stream.write(message)
        received = 0
        while received < len(message):
            received += len(await stream.read())
        samples.append((time.perf_counter() - started) * 1000)
        await asyncio.sleep(0.001)
    await stream.send_eof()


async def sdk_run(args: argparse.Namespace) -> dict[str, Any]:
    transports: list[BenchTransport] = []

    async def dial(_: str) -> BenchTransport:
        sock = bench_socket(args.socket_buffer)
        await asyncio.get_running_loop().sock_connect(sock, ("127.0.0.1", args.port))
        connection = await ws_connect(
            f"ws://127.0.0.1:{args.port}", sock=sock, max_size=None, compression=None, ping_interval=None
        )
        transports.append(BenchTransport(connection, args.wait))
        return transports[-1]

    options = ClientOptions(inflight_limit=args.inflight, session=session_config(args.window))
    client = await connect(dial, options=options)
    probe = Probe()
    probe.transport = transports[0]
    probe.raw_socket = transports[0]._socket.transport.get_extra_info("socket")
    quiet = args.stack == "quiet"  # no probes or pings, to compare with the thin host
    sampler = asyncio.create_task(probe.run() if not quiet else asyncio.sleep(0))
    stats = await client.open_stream("stats")

    async def ask(command: bytes) -> dict[str, Any]:
        await stats.write(command)
        return json.loads(await stats.read())

    await ask(b"reset")
    probe.reset()
    samples: list[float] = []
    stop = asyncio.Event()
    pinger = asyncio.create_task(round_trips(client, samples, stop) if not quiet else asyncio.sleep(0))
    bulk_high_water = 0
    started = time.perf_counter()
    if args.direction == "idle":
        await asyncio.sleep(args.idle_seconds)
    elif args.direction == "download":
        stream = await client.open_stream("source", json.dumps({"size": args.size, "rate": args.rate}).encode())
        async for _ in stream:
            pass
        bulk_high_water = stream.read_high_water
    else:
        stream = await client.open_stream("sink")
        await write_bulk(stream, args.size, args.rate)
        await stream.send_eof()
        assert await stream.read() == b"done"
    seconds = time.perf_counter() - started
    stop.set()
    await pinger
    server_report = await ask(b"report")
    sampler.cancel()
    result = {
        "seconds": seconds,
        "mb_per_s": 0.0 if args.direction == "idle" else args.size / seconds / MB,
        "samples": len(samples),
        "rtt_p50_ms": percentile(samples, 50),
        "rtt_p99_ms": percentile(samples, 99),
        "client_read_high_water_kb": bulk_high_water / 1000,
        **probe.report("client"),
        **server_report,
    }
    await client.close()
    return result


# --- Tunnel: the same session, with a TCP socket pump at each end --------------------------------


async def pump(stream: Any, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    async def socket_to_stream() -> None:
        while chunk := await reader.read(65536):
            await stream.write(chunk)
        await stream.send_eof()

    async def stream_to_socket() -> None:
        async for chunk in stream:
            writer.write(chunk)
            await writer.drain()
        writer.write_eof()

    try:
        await asyncio.gather(socket_to_stream(), stream_to_socket())
    except (ConnectionError, StreamReset):
        stream.reset()
    finally:
        writer.close()


async def target(port: int) -> None:
    """Where tunnelled connections end up. The first line says what this connection is for."""

    async def on_connection(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        command, *values = (await reader.readline()).decode().split()
        try:
            if command == "source":
                started, size, rate, sent = time.perf_counter(), int(values[0]), float(values[1]), 0
                while sent < size:
                    writer.write(CHUNK)
                    await writer.drain()
                    sent += len(CHUNK)
                    await pace(sent, rate, started)
            elif command == "sink":
                while await reader.read(65536):
                    pass
                writer.write(b"done")
            else:
                while chunk := await reader.read(65536):
                    writer.write(chunk)
                    await writer.drain()
            await writer.drain()
        except ConnectionError:
            pass
        finally:
            writer.close()

    server = await asyncio.start_server(on_connection, "127.0.0.1", port)
    await server.serve_forever()


async def tunnel_client(args: argparse.Namespace) -> None:
    """A local listener whose connections go through the session, and a control port for the driver."""
    transports: list[BenchTransport] = []
    streams: list[Any] = []

    async def dial(_: str) -> BenchTransport:
        sock = bench_socket(args.socket_buffer)
        sock.setblocking(False)
        await asyncio.get_running_loop().sock_connect(sock, ("127.0.0.1", args.port))
        connection = await ws_connect(
            f"ws://127.0.0.1:{args.port}", sock=sock, max_size=None, compression=None, ping_interval=None
        )
        transports.append(BenchTransport(connection, args.wait))
        return transports[-1]

    options = ClientOptions(inflight_limit=args.inflight, session=session_config(args.window))
    client = await connect(dial, options=options)
    probe = Probe()
    probe.transport = transports[0]
    probe.raw_socket = transports[0]._socket.transport.get_extra_info("socket")
    probe.reset()
    asyncio.create_task(probe.run())
    stats = await client.open_stream("stats")

    async def on_local(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        stream = await client.open_stream("forward", str(args.forward_port or args.port + 1).encode())
        streams.append(stream)
        await pump(stream, reader, writer)

    async def on_control(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        while command := (await reader.readline()).strip():
            await stats.write(command)
            report = json.loads(await stats.read())
            if command == b"reset":
                probe.reset()
                streams.clear()
            else:
                report.update(probe.report("client"))
                high_water = max((stream.read_high_water for stream in streams), default=0)
                report["client_read_high_water_kb"] = high_water / 1000
            writer.write(json.dumps(report).encode() + b"\n")
            await writer.drain()

    await asyncio.start_server(on_local, "127.0.0.1", args.port + 2)
    server = await asyncio.start_server(on_control, "127.0.0.1", args.port + 3)
    await server.serve_forever()


async def driver_round_trips(port: int, samples: list[float], stop: asyncio.Event) -> None:
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    writer.write(b"echo\n")
    message = bytes(64)
    while not stop.is_set():
        started = time.perf_counter()
        writer.write(message)
        await reader.readexactly(len(message))
        samples.append((time.perf_counter() - started) * 1000)
        await asyncio.sleep(0.001)
    writer.close()


async def driver(args: argparse.Namespace) -> dict[str, Any]:
    """Port is the tunnel's local listener, or the target itself for the tcp stack."""
    tunnelled = args.stack == "tunnel"
    port = args.port + 2 if tunnelled else args.port + 1
    control_reader, control_writer = None, None

    async def ask(command: bytes) -> dict[str, Any]:
        if not tunnelled:
            return {}
        control_writer.write(command + b"\n")
        await control_writer.drain()
        return json.loads(await control_reader.readline())

    if tunnelled:
        control_reader, control_writer = await asyncio.open_connection("127.0.0.1", args.port + 3)
    await ask(b"reset")
    samples: list[float] = []
    stop = asyncio.Event()
    pinger = asyncio.create_task(driver_round_trips(port, samples, stop))
    await asyncio.sleep(0.05)
    samples.clear()
    started = time.perf_counter()
    if args.direction == "idle":
        await asyncio.sleep(args.idle_seconds)
    else:
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        if args.direction == "download":
            writer.write(f"source {args.size} {args.rate}\n".encode())
            while await reader.read(1 << 20):
                pass
        else:
            writer.write(b"sink\n")
            await write_bulk_socket(writer, args.size, args.rate)
            writer.write_eof()
            assert await reader.read() == b"done"
        writer.close()
    seconds = time.perf_counter() - started
    stop.set()
    await pinger
    return {
        "seconds": seconds,
        "mb_per_s": 0.0 if args.direction == "idle" else args.size / seconds / MB,
        "samples": len(samples),
        "rtt_p50_ms": percentile(samples, 50),
        "rtt_p99_ms": percentile(samples, 99),
        **(await ask(b"report")),
    }


async def write_bulk_socket(writer: asyncio.StreamWriter, size: int, rate: float) -> None:
    started = time.perf_counter()
    sent = 0
    while sent < size:
        writer.write(CHUNK)
        await writer.drain()
        sent += len(CHUNK)
        await pace(sent, rate, started)


# --- A thin host over the sync core, as the baseline for what the SDK adds ----------------------


class ThinHost:
    def __init__(self, role: Role, session_id: str, connection: Any) -> None:
        self.connection = connection
        self.session = core.Session(role, session_id, b"")
        self.session.attach_connection(now_ms())
        self.work = asyncio.Event()
        self.writable = asyncio.Event()
        self.connected = asyncio.Event()
        self.accepted = asyncio.Event()
        self.opened: asyncio.Queue = asyncio.Queue()
        self.received = 0
        self.eof = asyncio.Event()
        self.done = asyncio.Event()

    def drain(self) -> None:
        while (event := self.session.poll_event()) is not None:
            match event.type:
                case EventType.DATA:
                    self.received += len(event.data)
                    self.session.consume(event.stream_id, len(event.data))
                case EventType.EOF:
                    self.eof.set()
                case EventType.WRITABLE:
                    self.writable.set()
                case EventType.CONNECTED:
                    self.connected.set()
                case EventType.STREAM_ACCEPTED:
                    self.accepted.set()
                case EventType.STREAM_OPENED:
                    self.session.accept_stream(event.stream_id)
                    self.opened.put_nowait((event.stream_id, event.params))
        self.work.set()

    async def reader(self) -> None:
        try:
            while True:
                self.session.on_message(await self.connection.recv(), now_ms())
                self.drain()
        except ConnectionClosed:
            self.done.set()

    async def sender(self) -> None:
        while True:
            message = self.session.poll_transmit()
            self.drain()
            if message is None:
                self.work.clear()
                await self.work.wait()
                continue
            await self.connection.send(message)

    async def write(self, stream_id: int, data: bytes) -> None:
        view = memoryview(data)
        while view:
            self.writable.clear()
            accepted = self.session.write(stream_id, view, now_ms())
            self.drain()
            view = view[accepted:]
            if view:
                await self.writable.wait()


def now_ms() -> int:
    return time.monotonic_ns() // 1_000_000


async def thin_serve(port: int, socket_buffer: int) -> None:
    async def on_socket(connection: Any) -> None:
        first = await connection.recv()
        hello = core.decode_frame(first)
        host = ThinHost(Role.SERVER, hello.session_id, connection)
        host.session.on_message(first, now_ms())
        host.drain()
        tasks = [asyncio.create_task(host.reader()), asyncio.create_task(host.sender())]
        cpu_at_start = cpu_seconds()
        while not host.done.is_set():
            getter = asyncio.create_task(host.opened.get())
            ended = asyncio.create_task(host.done.wait())
            await asyncio.wait({getter, ended}, return_when=asyncio.FIRST_COMPLETED)
            ended.cancel()
            if not getter.done():
                getter.cancel()
                break
            stream_id, params = getter.result()
            settings = json.loads(params)
            if settings["size"]:  # download
                sent = 0
                while sent < settings["size"]:
                    await host.write(stream_id, CHUNK)
                    sent += len(CHUNK)
                    await asyncio.sleep(0)
                host.session.send_eof(stream_id)
                host.drain()
            else:  # upload
                await host.eof.wait()
                host.eof.clear()
                report = {"server_cpu_s": cpu_seconds() - cpu_at_start}
                await host.write(stream_id, json.dumps(report).encode())
        for task in tasks:
            task.cancel()

    sock = bench_socket(socket_buffer)
    sock.bind(("127.0.0.1", port))
    sock.listen(16)
    sock.setblocking(False)
    async with ws_serve(on_socket, sock=sock, max_size=None, compression=None, ping_interval=None):
        await asyncio.Event().wait()


async def thin_run(args: argparse.Namespace) -> dict[str, Any]:
    sock = bench_socket(args.socket_buffer)
    await asyncio.get_running_loop().sock_connect(sock, ("127.0.0.1", args.port))
    connection = await ws_connect(
        f"ws://127.0.0.1:{args.port}", sock=sock, max_size=None, compression=None, ping_interval=None
    )
    host = ThinHost(Role.CLIENT, os.urandom(16).hex(), connection)
    host.drain()
    tasks = [asyncio.create_task(host.reader()), asyncio.create_task(host.sender())]
    await host.connected.wait()
    size = args.size if args.direction == "download" else 0
    cpu_at_start = cpu_seconds()
    stream_id = host.session.open_stream("bulk", json.dumps({"size": size}).encode(), now_ms())
    host.drain()
    started = time.perf_counter()
    if args.direction == "download":
        while not host.eof.is_set():
            await host.eof.wait()
    else:
        sent = 0
        while sent < args.size:
            await host.write(stream_id, CHUNK)
            sent += len(CHUNK)
            await asyncio.sleep(0)
        host.session.send_eof(stream_id)
        host.drain()
        while host.received == 0:
            await asyncio.sleep(0.001)
    seconds = time.perf_counter() - started
    result = {
        "seconds": seconds,
        "mb_per_s": args.size / seconds / MB,
        "client_cpu_s": cpu_seconds() - cpu_at_start,
    }
    for task in tasks:
        task.cancel()
    await connection.close()
    return result


# --- Driver ------------------------------------------------------------------------------------

BULK = 800 * MB

# name, stack, direction, wait, rate in MB/s, socket buffer, window
SETUPS = [
    ("tunnel idle", "tunnel", "idle", "poll", 0, 0, 0),
    ("tunnel download", "tunnel", "download", "poll", 0, 0, 0),
    ("tunnel upload", "tunnel", "upload", "poll", 0, 0, 0),
    ("tunnel download, window 512 KiB", "tunnel", "download", "poll", 0, 0, 512 * 1024),
    ("tunnel download, window 1024 KiB", "tunnel", "download", "poll", 0, 0, 1024 * 1024),
    ("thin download", "thin", "download", "", 0, 0, 0),
    ("quiet download", "quiet", "download", "poll", 0, 0, 0),
    ("thin upload", "thin", "upload", "", 0, 0, 0),
    ("quiet upload", "quiet", "upload", "poll", 0, 0, 0),
]


def run_setup(script: str, setup: tuple, port: int) -> dict[str, Any]:
    name, stack, direction, wait, rate, socket_buffer, window = setup
    common = [
        "--port", str(port), "--stack", stack, "--wait", wait or "poll",
        "--socket-buffer", str(socket_buffer), "--window", str(window),
    ]
    python = [sys.executable, script]
    processes = {
        "tcp": [[*python, "target", *common]],
        "tunnel": [[*python, "target", *common], [*python, "serve", *common]],
        "sdk": [[*python, "serve", *common]],
        "quiet": [[*python, "serve", *common]],
        "thin": [[*python, "serve", *common]],
    }[stack]
    running = [subprocess.Popen(command) for command in processes]
    try:
        time.sleep(0.4)
        if stack == "tunnel":
            running.append(subprocess.Popen([*python, "tunnel-client", *common]))
            time.sleep(0.4)
        output = subprocess.run(
            [*python, "driver" if stack in ("tcp", "tunnel") else "client", *common, "--direction", direction,
             "--size", str(BULK), "--rate", str(rate * MB)],
            check=True, capture_output=True, text=True, timeout=120,
        ).stdout
    finally:
        for process in running:
            process.send_signal(signal.SIGKILL)
            process.wait()
    return {"setup": name, **json.loads(output.strip().splitlines()[-1])}


def matrix(path: str, rounds: int, only: str | None) -> None:
    script = os.path.abspath(__file__)
    setups = [setup for setup in SETUPS if only is None or any(word in setup[0] for word in only.split(","))]
    with open(path, "a") as out:
        for index in range(rounds):  # whole passes, so versions alternate
            for offset, setup in enumerate(setups):
                result = run_setup(script, setup, 18600 + (index * 40 + offset * 6) % 1200)
                result["round"] = index
                out.write(json.dumps(result) + "\n")
                out.flush()
                print(f"{index} {setup[0]}: {result.get('mb_per_s', 0):.0f} MB/s, "
                      f"rtt p50 {result.get('rtt_p50_ms', 0):.2f} ms", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["serve", "client", "matrix", "target", "tunnel-client", "driver"])
    parser.add_argument("out", nargs="?")
    parser.add_argument("--port", type=int, default=18600)
    parser.add_argument("--stack", default="sdk")
    parser.add_argument("--wait", default="poll")
    parser.add_argument("--socket-buffer", type=int, default=0)
    parser.add_argument("--inflight", type=int, default=64 * 1024)
    parser.add_argument("--direction", default="download")
    parser.add_argument("--size", type=int, default=BULK)
    parser.add_argument("--rate", type=float, default=0)
    parser.add_argument("--idle-seconds", type=float, default=2.0)
    parser.add_argument("--window", type=int, default=0)
    parser.add_argument("--forward-port", type=int, default=0)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--only")
    args = parser.parse_args()
    if args.command == "matrix":
        matrix(args.out, args.rounds, args.only)
    elif args.command == "target":
        asyncio.run(target(args.port + 1))
    elif args.command == "tunnel-client":
        asyncio.run(tunnel_client(args))
    elif args.command == "driver":
        print(json.dumps(asyncio.run(driver(args))))
    elif args.command == "serve":
        if args.stack in ("sdk", "tunnel", "quiet"):
            quiet = args.stack == "quiet"
            asyncio.run(sdk_serve(args.port, args.wait, args.socket_buffer, args.inflight, quiet, args.window))
        else:
            asyncio.run(thin_serve(args.port, args.socket_buffer))
    else:
        run = thin_run if args.stack == "thin" else sdk_run
        print(json.dumps(asyncio.run(run(args))))


if __name__ == "__main__":
    main()
