"""Acceptance test: two sessions driven through the whole lifecycle from Python."""

import pytest

from packetmux import (
    EventType,
    Reason,
    Role,
    Session,
    SessionError,
    SessionState,
    Status,
    StreamState,
)

NOW_MS = 1000


class Peer:
    def __init__(self, role: Role, session_id: str) -> None:
        self.session = Session(role, session_id)
        self.events = []

    def drain(self) -> None:
        while (event := self.session.poll_event()) is not None:
            self.events.append(event)

    def take(self) -> list:
        self.drain()
        events, self.events = self.events, []
        return events


def transfer(sender: Peer, receiver: Peer, now_ms: int = NOW_MS) -> None:
    while (message := sender.session.poll_transmit()) is not None:
        receiver.session.on_message(message, now_ms)
        receiver.drain()
    sender.drain()


def pump(client: Peer, server: Peer) -> None:
    transfer(client, server)
    transfer(server, client)
    transfer(client, server)


@pytest.fixture
def pair() -> tuple[Peer, Peer]:
    client = Peer(Role.CLIENT, "client-1")
    server = Peer(Role.SERVER, "server-1")
    client.session.attach_connection(NOW_MS)
    server.session.attach_connection(NOW_MS)
    pump(client, server)
    assert [e.peer_session_id for e in client.take()] == ["server-1"]
    assert [e.peer_session_id for e in server.take()] == ["client-1"]
    assert client.session.state == SessionState.ESTABLISHED
    return client, server


def types(events: list) -> list[EventType]:
    return [event.type for event in events]


def test_request_and_reply_with_clean_close(pair: tuple[Peer, Peer]) -> None:
    client, server = pair
    stream_id = client.session.open_stream("tcp-forward", b"{}", NOW_MS)
    assert client.session.write(stream_id, b"request", NOW_MS) == 7
    client.session.send_eof(stream_id)
    transfer(client, server)

    opened, data, eof = server.take()
    assert (opened.type, opened.kind, opened.params) == (EventType.STREAM_OPENED, "tcp-forward", b"{}")
    assert (data.type, data.data) == (EventType.DATA, b"request")
    assert eof.type == EventType.EOF

    server.session.accept_stream(stream_id)
    server.session.write(stream_id, b"the reply", NOW_MS)
    server.session.send_eof(stream_id)
    transfer(server, client)

    assert types(server.take()) == [EventType.STREAM_CLOSED]
    accepted, reply, eof, closed = client.take()
    assert accepted.type == EventType.STREAM_ACCEPTED
    assert reply.data == b"the reply"
    assert eof.type == EventType.EOF
    assert (closed.type, closed.code) == (EventType.STREAM_CLOSED, Reason.OK)
    assert client.session.list_streams() == []


def test_events_own_their_bytes_after_the_next_message(pair: tuple[Peer, Peer]) -> None:
    client, server = pair
    first = client.session.open_stream("kind", b"", NOW_MS)
    transfer(client, server)
    server.take()
    server.session.accept_stream(first)
    transfer(server, client)
    client.take()

    client.session.write(first, b"one", NOW_MS)
    transfer(client, server)
    kept = server.take()[0]
    client.session.write(first, b"two", NOW_MS)
    transfer(client, server)
    assert kept.data == b"one"


def test_reject_and_reset(pair: tuple[Peer, Peer]) -> None:
    client, server = pair
    rejected = client.session.open_stream("kind", b"", NOW_MS)
    transfer(client, server)
    server.session.reject_stream(rejected, Reason.POLICY_DENIED, b"no")
    transfer(server, client)
    assert [(e.type, e.code) for e in client.take()] == [
        (EventType.STREAM_CLOSED, Reason.POLICY_DENIED)
    ]
    server.take()

    reset = client.session.open_stream("kind", b"", NOW_MS)
    client.session.write(reset, b"abc", NOW_MS)
    info = client.session.list_streams()[0]
    assert (info.stream_id, info.kind, info.opener) == (reset, "kind", Role.CLIENT)
    assert (info.state, info.bytes_sent) == (StreamState.SENDER_OPENING, 3)

    client.session.reset_stream(reset, Reason.TIMEOUT)
    pump(client, server)
    assert client.take()[-1].code == Reason.TIMEOUT
    assert server.take()[-1].code == Reason.TIMEOUT


def test_errors_become_exceptions(pair: tuple[Peer, Peer]) -> None:
    client, _ = pair
    with pytest.raises(SessionError) as error:
        client.session.send_eof(99)
    assert error.value.status == Status.UNKNOWN_STREAM
    with pytest.raises(SessionError) as error:
        client.session.open_stream("not_valid", b"", NOW_MS)
    assert error.value.status == Status.INVALID_ARGUMENT


def test_keepalive_measures_rtt(pair: tuple[Peer, Peer]) -> None:
    client, server = pair
    assert client.session.rtt_ms is None
    deadline = client.session.next_deadline_ms()
    client.session.on_tick(deadline)
    server.session.on_message(client.session.poll_transmit(), deadline)
    client.session.on_message(server.session.poll_transmit(), deadline + 25)
    assert client.session.rtt_ms == 25


def test_reconnect_keeps_the_session(pair: tuple[Peer, Peer]) -> None:
    client, server = pair
    before = client.session.open_stream("kind", b"", NOW_MS)
    client.session.on_connection_lost()
    server.session.on_connection_lost()
    assert types(client.take()) == [EventType.STREAM_CLOSED, EventType.DISCONNECTED]
    server.take()
    assert client.session.state == SessionState.DISCONNECTED

    client.session.attach_connection(NOW_MS)
    server.session.attach_connection(NOW_MS)
    pump(client, server)
    assert client.session.state == SessionState.ESTABLISHED
    assert client.session.open_stream("kind", b"", NOW_MS) > before


def test_connection_lost_after_the_end_does_nothing(pair: tuple[Peer, Peer]) -> None:
    client, _ = pair
    client.session.on_connection_lost()
    assert types(client.take()) == [EventType.DISCONNECTED]
    client.session.on_connection_lost()
    assert client.take() == []

    client.session.close(Reason.OK, NOW_MS)
    assert types(client.take()) == [EventType.SESSION_CLOSED]
    client.session.on_connection_lost()
    assert client.take() == []


def test_close_ends_both_sessions(pair: tuple[Peer, Peer]) -> None:
    client, server = pair
    client.session.close(Reason.AUTH_REVOKED, NOW_MS)
    assert client.session.state == SessionState.CLOSING
    transfer(client, server)
    assert client.session.state == SessionState.CLOSED
    assert server.session.state == SessionState.CLOSED
    assert [(e.type, e.code) for e in client.take()] == [
        (EventType.SESSION_CLOSED, Reason.AUTH_REVOKED)
    ]
    assert client.session.next_deadline_ms() is None


def test_context_manager_frees_the_session() -> None:
    with Session(Role.CLIENT, "s1") as session:
        session.attach_connection(NOW_MS)
    session.destroy()  # safe to repeat
