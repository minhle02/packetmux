"""Runs the language-neutral SDK layers in conformance/: params, async and transport."""

import asyncio
import dataclasses
import json
from pathlib import Path
from typing import Any

import packetmux
import pytest
from packetmux import ClientOptions, Reason, Reject, Refuse, Server, ServerOptions
from packetmux.params import decode_params, define_stream, encode_params
from packetmux.testing import (
    MemoryNetwork,
    MemoryTransport,
    TRANSPORT_CHECKS,
    memory_transport_pair,
    run_transport_check,
)

CONFORMANCE = Path(__file__).resolve().parents[4] / "conformance"
MANIFEST = json.loads((CONFORMANCE / "manifest.json").read_text())
TIMEOUT = 5.0


def data_of(value: str | dict) -> bytes:
    if isinstance(value, dict):
        return bytes.fromhex(value["repeat"]) * value["count"]
    return bytes.fromhex(value)


# --- params ----------------------------------------------------------------------------------

PARAMS = [json.loads((CONFORMANCE / path).read_text()) for path in MANIFEST["params"]]


def params_cases() -> list[tuple[dict, dict]]:
    return [(suite["fields"], case) for suite in PARAMS for case in suite["cases"]]


@pytest.mark.parametrize("fields,case", params_cases(), ids=[c["name"] for _, c in params_cases()])
def test_params(fields: dict, case: dict) -> None:
    stream = define_stream("kind", fields)
    data = bytes.fromhex(case["input"])
    if case.get("rejected"):
        with pytest.raises(packetmux.InvalidParams):
            decode_params(stream, data)
        return
    assert dataclasses.asdict(decode_params(stream, data)) == case["result"]


@pytest.mark.parametrize(
    "fields,case",
    [(suite["fields"], case) for suite in PARAMS for case in suite["encode_rejected"]],
    ids=lambda value: value["name"] if "name" in value else "",
)
def test_outgoing_params(fields: dict, case: dict) -> None:
    stream = define_stream("kind", fields)
    with pytest.raises(packetmux.InvalidParams):
        encode_params(stream(**case["value"]))


# --- transport -------------------------------------------------------------------------------

CHECKS = [
    name
    for path in MANIFEST["transport"]
    for name in json.loads((CONFORMANCE / path).read_text())["checks"]
]


def test_every_transport_check_exists() -> None:
    assert sorted(CHECKS) == sorted(TRANSPORT_CHECKS)


@pytest.mark.parametrize("name", CHECKS)
def test_memory_transport(name: str) -> None:
    asyncio.run(asyncio.wait_for(run_transport_check(name, memory_transport_pair), 30))


# --- async -----------------------------------------------------------------------------------


class Harness:
    """A server and its clients, over a memory network. The scenario drives them."""

    def __init__(self, scenario: dict) -> None:
        options = scenario["options"]
        self.handler_saw: list[Reason] = []
        self.server_close_seen: list[Reason] = []
        self.states: list[str] = []
        self.client_options = ClientOptions(
            connect_timeout=options["connect_timeout_ms"] / 1000,
            reconnect_backoff=[ms / 1000 for ms in options["reconnect_backoff_ms"]],
            max_read_buffer=options.get("max_read_buffer", 16 << 20),
        )
        server_options = ServerOptions(
            grace_period=options.get("grace_period_ms", 120_000) / 1000,
            max_sessions=options.get("max_sessions", 1024),
            max_read_buffer=options.get("max_read_buffer", 16 << 20),
            session=options.get("session", {}),
        )
        self.handlers = [self._handler(kind, spec) for kind, spec in scenario["handlers"].items()]
        self.server = Server(self.handlers, self._authorize_with(scenario), options=server_options)
        self.network = MemoryNetwork(self.server)
        self.small_transport = 0
        self.clients: dict[str, Any] = {}
        self.dial_ids: list[str] = []
        self.streams: dict[str, Any] = {}
        self.tasks: dict[str, asyncio.Task] = {}
        self.dials_seen = 0
        self.raw: MemoryTransport | None = None

    async def dial(self, session_id: str) -> Any:
        self.dial_ids.append(session_id)
        transport = await self.network.dial(session_id)
        if self.small_transport:
            transport.max_message_size = self.small_transport
        return transport

    def _authorize_with(self, scenario: dict) -> Any:
        rule = scenario.get("authorize")
        if rule is None:
            return None

        async def authorize(request: packetmux.ConnectRequest) -> Any:
            if rule.startswith("refuse:"):
                raise Refuse(Reason[rule.split(":")[1]])
            if request.session is not None and request.session.data != request.context:
                raise Refuse(Reason.AUTH_REVOKED)
            return request.context

        return authorize

    def _handler(self, kind: str, spec: str | dict) -> tuple[Any, Any]:
        name = spec if isinstance(spec, str) else spec["behaviour"]
        target = kind if isinstance(spec, str) else define_stream(kind, spec["fields"])
        action, _, argument = name.partition(":")

        async def handle(request: packetmux.IncomingStream) -> None:
            if action == "echo":
                stream = request.accept()
                async for chunk in stream:
                    await stream.write(chunk)
                await stream.send_eof()
            elif action == "reject":
                raise Reject(Reason[argument])
            elif action == "fail":
                raise RuntimeError("handler failed")
            elif action == "accept-then-fail":
                request.accept()
                raise RuntimeError("handler failed")
            elif action == "reply":
                await request.accept().write(data_of(argument))
            elif action == "reply-then-reset":
                stream = request.accept()
                await stream.write(data_of(argument))
                await asyncio.sleep(0.02)
                stream.reset(Reason.KILLED)
            elif action == "flood":
                await request.accept().write(b"x" * int(argument))
                await asyncio.Event().wait()
            elif action == "record-close":
                self.handler_saw.append(await request.accept().wait_closed())
            elif action == "reply-params":
                value = f"{request.params.host}:{request.params.port}"
                await request.accept().write(value.encode())
            elif action == "reply-raw-params":
                await request.accept().write(request.params)
            elif action != "return":
                raise AssertionError(f"unknown behaviour {name}")

        return target, handle

    def client(self, step: dict) -> Any:
        return self.clients[step.get("client", "client")]

    def session(self) -> Any:
        return self.server.sessions()[0]

    def link(self, step: dict) -> Any:
        index = -2 if step.get("link") == "previous" else -1
        return self.network.links[index]

    def transport_of(self, step: dict) -> MemoryTransport:
        return self.link(step)[0 if step["side"] == "client" else 1]

    async def until(self, condition: Any, what: Any) -> None:
        for _ in range(int(TIMEOUT / 0.002)):
            if condition():
                return
            await asyncio.sleep(0.002)
        raise AssertionError(f"timed out waiting for {what}")

    def error_of(self, step: dict, error: Exception) -> None:
        assert type(error).__name__ == step["expect_error"] or isinstance(
            error, getattr(packetmux, step["expect_error"])
        ), repr(error)
        if "expect_code" in step:
            assert error.code == Reason[step["expect_code"]]

    def background(self, step: dict, coroutine: Any) -> bool:
        if "background" in step:
            self.tasks[step["background"]] = asyncio.get_running_loop().create_task(coroutine)
            return True
        return False


async def do_connect(h: Harness, s: dict) -> None:
    try:
        h.clients[s.get("as", "client")] = await packetmux.connect(
            h.dial, handlers=h.handlers, options=h.client_options
        )
    except packetmux.PacketMuxError as error:
        h.error_of(s, error)
    else:
        assert "expect_error" not in s


async def open_stream(h: Harness, s: dict, target: Any, params: bytes) -> None:
    on = s.get("on", "client")
    owner = h.session() if on == "server" else h.clients[on]
    try:
        h.streams[s["as"]] = await owner.open_stream(target, params)
    except packetmux.PacketMuxError as error:
        h.error_of(s, error)
    else:
        assert "expect_error" not in s


async def do_open(h: Harness, s: dict) -> None:
    run = open_stream(h, s, s["kind"], bytes.fromhex(s.get("params", "")))
    if not h.background(s, run):
        await run


async def do_open_typed(h: Harness, s: dict) -> None:
    await open_stream(h, s, define_stream(s["kind"], s["fields"])(**s["value"]), b"")


async def do_write(h: Harness, s: dict) -> None:
    run = h.streams[s["stream"]].write(data_of(s["data"]))
    if not h.background(s, run):
        await run


async def do_write_concurrently(h: Harness, s: dict) -> None:
    await asyncio.gather(*(h.streams[s["stream"]].write(data_of(w)) for w in s["writes"]))


async def do_expect_read(h: Harness, s: dict) -> None:
    expected = data_of(s["data"])
    received = bytearray()
    while len(received) < len(expected):
        chunk = await asyncio.wait_for(h.streams[s["stream"]].read(), TIMEOUT)
        assert chunk, "end of stream too early"
        received += chunk
    assert bytes(received) == expected


async def do_expect_error(h: Harness, s: dict) -> None:
    stream = h.streams[s["stream"]]
    call = {"read": stream.read, "wait_accepted": stream.wait_accepted}.get(s["call"])
    with pytest.raises(getattr(packetmux, s["error"])) as caught:
        await asyncio.wait_for(call() if call else stream.write(b"x"), TIMEOUT)
    if "code" in s:
        assert caught.value.code == Reason[s["code"]]


async def do_await(h: Harness, s: dict) -> None:
    task = h.tasks[s["task"]]
    if "error" in s:
        with pytest.raises(getattr(packetmux, s["error"])) as caught:
            await asyncio.wait_for(task, TIMEOUT)
        assert caught.value.code == Reason[s["code"]]
    else:
        await asyncio.wait_for(task, TIMEOUT)


async def do_close(h: Harness, s: dict) -> None:
    run = h.client(s).close(Reason[s.get("code", "OK")])
    if not h.background(s, run):
        await asyncio.wait_for(run, TIMEOUT)


async def do_expect_state(h: Harness, s: dict) -> None:
    await h.until(lambda: h.client(s).state.value == s["state"], s)


async def do_expect_closed(h: Harness, s: dict) -> None:
    client = h.client(s)
    await asyncio.wait_for(client.wait_closed(), TIMEOUT)
    assert client.state.value == "closed"
    if "error" not in s:
        assert client.close_error is None
        return
    assert isinstance(client.close_error, getattr(packetmux, s["error"])), repr(client.close_error)
    if "code" in s:
        assert client.close_error.code == Reason[s["code"]]


async def do_expect_stream_closed(h: Harness, s: dict) -> None:
    code = await asyncio.wait_for(h.streams[s["stream"]].wait_closed(), TIMEOUT)
    assert code == Reason[s["code"]]


async def do_expect_eof(h: Harness, s: dict) -> None:
    assert await asyncio.wait_for(h.streams[s["stream"]].read(), TIMEOUT) == b""


async def do_cancel_read(h: Harness, s: dict) -> None:
    task = asyncio.get_running_loop().create_task(h.streams[s["stream"]].read())
    await asyncio.sleep(0.005)
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)


async def do_record_states(h: Harness, s: dict) -> None:
    h.client(s).on_state_change(lambda state: h.states.append(state.value))


async def do_raw_connect(h: Harness, s: dict) -> None:
    h.raw, far = memory_transport_pair()
    if "wire" in s:
        await h.raw.send(bytes.fromhex(s["wire"]))
    h.tasks["raw"] = asyncio.get_running_loop().create_task(h.server.serve(far, h.network.context))


async def do_raw_hello(h: Harness, s: dict) -> None:
    hello = packetmux.core.Frame(
        type=packetmux.core.FrameType.HELLO,
        session_id=h.clients[s["session_of"]].session_id,
        resuming=s["resuming"],
    )
    await do_raw_connect(h, {"wire": packetmux.core.encode_frame(hello).hex()})


async def do_expect_raw_closed(h: Harness, s: dict) -> None:
    async def read_until_closed() -> None:
        while True:
            await h.raw.recv()

    with pytest.raises(packetmux.TransportClosed):
        await asyncio.wait_for(read_until_closed(), TIMEOUT)


async def do_settle(h: Harness, s: dict) -> None:
    await asyncio.sleep(s.get("ms", 20) / 1000)


async def do_set_context(h: Harness, s: dict) -> None:
    h.network.context = s["value"]


async def do_dial_count(h: Harness, s: dict) -> None:
    h.dials_seen = h.network.dials


async def do_close_session(h: Harness, s: dict) -> None:
    await asyncio.wait_for(h.session().close(Reason[s["code"]]), TIMEOUT)


async def do_expect_sessions(h: Harness, s: dict) -> None:
    await h.until(lambda: len(h.server.sessions()) == s["count"], s)


async def do_expect_dials(h: Harness, s: dict) -> None:
    await h.until(lambda: h.network.dials >= s["min"], s)


async def do_expect_handler_saw(h: Harness, s: dict) -> None:
    await h.until(lambda: Reason[s["code"]] in h.handler_saw, s)


async def do_expect_server_close(h: Harness, s: dict) -> None:
    assert h.server_close_seen == [Reason[s["code"]]]


async def do_record_server_close(h: Harness, s: dict) -> None:
    host = h.session()._host
    on_closed = host.owner.on_closed

    def record(code: Reason, connection: Any) -> None:
        h.server_close_seen.append(code)
        on_closed(code, connection)

    host.owner.on_closed = record


async def do_expect_high_water(h: Harness, s: dict) -> None:
    assert h.streams[s["stream"]].read_high_water == s["value"]


async def do_expect_ids_differ(h: Harness, s: dict) -> None:
    a, b = h.clients[s["a"]].session_id, h.clients[s["b"]].session_id
    assert a != b and len(a) >= 16
    assert all(c.isalnum() or c == "-" for c in a)


async def do_expect_dial_ids(h: Harness, s: dict) -> None:
    assert h.clients[s["client"]].session_id in h.dial_ids


async def do_stall(h: Harness, s: dict) -> None:
    h.transport_of(s).stall()


async def do_release(h: Harness, s: dict) -> None:
    h.transport_of(s).release()


async def do_break(h: Harness, s: dict) -> None:
    await h.link(s)[0].break_connection()


async def do_drop(h: Harness, s: dict) -> None:
    h.transport_of({"side": "client", **s}).fail_locally()


async def do_inject(h: Harness, s: dict) -> None:
    h.transport_of(s).inject(bytes.fromhex(s["wire"]))


async def do_eof(h: Harness, s: dict) -> None:
    await h.streams[s["stream"]].send_eof()


async def do_reset(h: Harness, s: dict) -> None:
    h.streams[s["stream"]].reset(Reason[s["code"]])


async def do_wait_accepted(h: Harness, s: dict) -> None:
    await asyncio.wait_for(h.streams[s["stream"]].wait_accepted(), TIMEOUT)


async def do_dial_fails(h: Harness, s: dict) -> None:
    h.network.fail_next(s["count"])


async def do_dial_hangs(h: Harness, s: dict) -> None:
    h.network.hang()


async def do_dial_works(h: Harness, s: dict) -> None:
    h.network.hang(False)


async def do_dial_gives_up(h: Harness, s: dict) -> None:
    h.network.give_up()


async def do_expect_pending(h: Harness, s: dict) -> None:
    assert not h.tasks[s["task"]].done()


async def do_expect_states(h: Harness, s: dict) -> None:
    assert h.states == s["states"]


async def do_expect_no_new_dials(h: Harness, s: dict) -> None:
    assert h.network.dials == h.dials_seen


async def do_small_transport(h: Harness, s: dict) -> None:
    h.small_transport = s["max_message_size"]


async def do_expect_connected(h: Harness, s: dict) -> None:
    assert h.client(s).state.value == "connected"


async def do_expect_session_disconnected(h: Harness, s: dict) -> None:
    await h.until(lambda: not h.session().connected, s)


ASYNC_STEPS = {
    "await": do_await,
    "break": do_break,
    "cancel_read": do_cancel_read,
    "close": do_close,
    "close_session": do_close_session,
    "connect": do_connect,
    "dial_count": do_dial_count,
    "dial_fails": do_dial_fails,
    "dial_gives_up": do_dial_gives_up,
    "dial_hangs": do_dial_hangs,
    "dial_works": do_dial_works,
    "drop": do_drop,
    "eof": do_eof,
    "inject": do_inject,
    "expect_closed": do_expect_closed,
    "expect_connected": do_expect_connected,
    "expect_dial_ids_match": do_expect_dial_ids,
    "expect_dials": do_expect_dials,
    "expect_eof": do_expect_eof,
    "expect_error": do_expect_error,
    "expect_handler_saw": do_expect_handler_saw,
    "expect_high_water": do_expect_high_water,
    "expect_no_new_dials": do_expect_no_new_dials,
    "expect_pending": do_expect_pending,
    "expect_raw_closed": do_expect_raw_closed,
    "expect_read": do_expect_read,
    "expect_server_close": do_expect_server_close,
    "expect_session_disconnected": do_expect_session_disconnected,
    "expect_session_ids_differ": do_expect_ids_differ,
    "expect_sessions": do_expect_sessions,
    "expect_state": do_expect_state,
    "expect_states": do_expect_states,
    "expect_stream_closed": do_expect_stream_closed,
    "open": do_open,
    "open_typed": do_open_typed,
    "raw_connect": do_raw_connect,
    "raw_hello": do_raw_hello,
    "record_server_close": do_record_server_close,
    "record_states": do_record_states,
    "release": do_release,
    "reset": do_reset,
    "set_context": do_set_context,
    "settle": do_settle,
    "small_transport": do_small_transport,
    "stall": do_stall,
    "wait_accepted": do_wait_accepted,
    "write": do_write,
    "write_concurrently": do_write_concurrently,
}

ASYNC_SCENARIOS = [
    scenario
    for path in MANIFEST["async"]
    for scenario in json.loads((CONFORMANCE / path).read_text())["scenarios"]
]


async def run_scenario(scenario: dict) -> None:
    harness = Harness(scenario)
    try:
        for step in scenario["steps"]:
            await ASYNC_STEPS[step["op"]](harness, step)  # an unknown op raises KeyError
            await asyncio.sleep(0)  # a background step has started, as it would in JS
    finally:
        harness.network.hang(False)
        for task in harness.tasks.values():
            task.cancel()
        for client in harness.clients.values():
            await asyncio.wait_for(client.close(), TIMEOUT)
        await asyncio.wait_for(harness.server.close(), TIMEOUT)


@pytest.mark.parametrize("scenario", ASYNC_SCENARIOS, ids=[s["name"] for s in ASYNC_SCENARIOS])
def test_async_scenario(scenario: dict) -> None:
    asyncio.run(asyncio.wait_for(run_scenario(scenario), 20))
