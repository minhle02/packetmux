"""Python server, Node client, a real WebSocket: the same protocol across two languages."""

import asyncio
import hashlib
import json
import time
from pathlib import Path

from websockets.asyncio.server import ServerConnection, serve

from packetmux.core import EventType, FrameType, Reason, Role, Session, SessionState, decode_frame

JS_BINDING = Path(__file__).resolve().parents[3] / "js"
CLIENT = JS_BINDING / "test" / "interopClient.js"
REQUEST_SIZE = 3 * 1024 * 1024
REPLY_SIZE = 2 * 1024 * 1024
TIMEOUT_S = 30


def now_ms() -> int:
    return int(time.monotonic() * 1000)


def pattern_of(size: int, seed: int) -> bytes:
    return bytes((index * seed + 7) % 251 for index in range(size))


class Server:
    """One host: the loop from Session.h, on top of a websockets connection."""

    def __init__(self) -> None:
        self.received = bytearray()
        self.events: list[EventType] = []
        self.kind = ""
        self.peer_session_id = ""
        self.close_code: Reason | None = None
        self.stream_close_code: Reason | None = None
        self.final_state: SessionState | None = None
        self.first_frame_type: FrameType | None = None
        self._reply = b""
        self._reply_size = 0
        self._offset = 0
        self._stream_id = 0
        self._eof_sent = False

    def _push_reply(self, session: Session) -> None:
        while self._stream_id and self._offset < len(self._reply):
            accepted = session.write(self._stream_id, self._reply[self._offset :], now_ms())
            if accepted == 0:
                return
            self._offset += accepted
        if self._reply and self._offset == len(self._reply) and not self._eof_sent:
            session.send_eof(self._stream_id)
            self._eof_sent = True

    def _on_event(self, session: Session, event) -> None:
        self.events.append(event.type)
        if event.type == EventType.CONNECTED:
            self.peer_session_id = event.peer_session_id
        elif event.type == EventType.STREAM_OPENED:
            self.kind = event.kind
            self._stream_id = event.stream_id
            session.accept_stream(event.stream_id)
            self._reply_size = int(event.params)
        elif event.type == EventType.DATA:
            self.received += event.data
            session.consume(event.stream_id, len(event.data))
        elif event.type == EventType.EOF:
            self._reply = pattern_of(self._reply_size, 17)
        elif event.type == EventType.STREAM_CLOSED:
            self.stream_close_code = event.code
        elif event.type == EventType.SESSION_CLOSED:
            self.close_code = event.code

    async def _pump(self, session: Session, socket: ServerConnection) -> None:
        progress = True
        while progress:
            progress = False
            self._push_reply(session)
            while (event := session.poll_event()) is not None:
                self._on_event(session, event)
                progress = True
            while (message := session.poll_transmit()) is not None:
                await socket.send(message)
                progress = True

    async def handle(self, socket: ServerConnection) -> None:
        # A server learns which session this is from the first message (M2 section 2.2).
        first = await socket.recv(decode=False)
        hello = decode_frame(first)
        self.first_frame_type = hello.type
        with Session(Role.SERVER, hello.session_id) as session:
            session.attach_connection(now_ms())
            session.on_message(first, now_ms())
            await self._pump(session, socket)
            while session.state != SessionState.CLOSED:
                session.on_message(await socket.recv(decode=False), now_ms())
                await self._pump(session, socket)
            self.final_state = session.state


async def run_interop() -> tuple[Server, dict]:
    server = Server()
    async with serve(server.handle, "127.0.0.1", 0) as listener:
        port = listener.sockets[0].getsockname()[1]
        client = await asyncio.create_subprocess_exec(
            "node",
            str(CLIENT),
            f"ws://127.0.0.1:{port}",
            str(REQUEST_SIZE),
            str(REPLY_SIZE),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await asyncio.wait_for(client.communicate(), TIMEOUT_S)
    assert client.returncode == 0, stderr.decode()
    return server, json.loads(stdout)


def test_python_and_js_talk_over_a_real_websocket() -> None:
    assert (JS_BINDING / "dist" / "packetmux.mjs").exists(), (
        "build the WASM module first: emcmake cmake -S . -B build/wasm "
        "-DPACKETMUX_BUILD_TESTS=OFF && cmake --build build/wasm"
    )
    server, client = asyncio.run(run_interop())

    # Handshake, in both directions.
    assert server.first_frame_type == FrameType.HELLO
    assert server.peer_session_id == "interop-1"
    assert client["peerSessionId"] == "interop-1"

    # Byte for byte, in both directions.
    assert len(server.received) == REQUEST_SIZE
    assert hashlib.sha256(server.received).hexdigest() == client["requestSha256"]
    assert server.received == pattern_of(REQUEST_SIZE, 31)
    assert client["replyBytes"] == REPLY_SIZE
    assert client["replySha256"] == hashlib.sha256(pattern_of(REPLY_SIZE, 17)).hexdigest()

    # A clean close: the stream ended with both EOFs, and then the session.
    assert server.kind == "interop"
    assert server.stream_close_code == Reason.OK
    assert client["streamCloseCode"] == Reason.OK
    assert server.close_code == Reason.OK
    assert client["sessionCloseCode"] == Reason.OK
    assert client["state"] == "CLOSED"
    assert server.final_state == SessionState.CLOSED
    assert "WRITABLE" in client["events"], "the transfer should have hit backpressure"
