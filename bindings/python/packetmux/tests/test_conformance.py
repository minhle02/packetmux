"""Runs the language-neutral sync scenarios in conformance/ through the binding."""

import json
from pathlib import Path

import pytest
from packetmux.core import (
    CodecStatus,
    EventType,
    Frame,
    FrameType,
    Reason,
    Role,
    Session,
    SessionError,
    SessionState,
    Status,
    StreamState,
    decode_frame,
    encode_frame,
)

CONFORMANCE = Path(__file__).resolve().parents[4] / "conformance"
MANIFEST = json.loads((CONFORMANCE / "manifest.json").read_text())
SETUPS = json.loads((CONFORMANCE / "sync/setups.json").read_text())


def data_of(value: str | dict) -> bytes:
    if isinstance(value, dict):
        return bytes.fromhex(value["repeat"]) * value["count"]
    return bytes.fromhex(value)


class Harness:
    def __init__(self, scenario: dict) -> None:
        config = dict(scenario.get("config", {}))
        capabilities = bytes.fromhex(config.pop("capabilities", ""))
        self.session = Session(
            Role[scenario["role"].upper()], "session-1", capabilities, **config
        )
        self.now_ms = 1000
        self.last_ping = 0

    def call(self, step: dict, method: str, *args: object) -> object:
        expected = Status[step.get("expect_status", "OK")]
        try:
            result = getattr(self.session, method)(*args)
        except SessionError as error:
            assert error.status == expected
            return None
        assert expected == Status.OK
        return result

    def frames(self) -> list[Frame]:
        frames = []
        while (message := self.session.poll_transmit()) is not None:
            frames.append(decode_frame(message))
            if frames[-1].type == FrameType.PING:
                self.last_ping = frames[-1].nonce
        return frames

    def events(self) -> list:
        events = []
        while (event := self.session.poll_event()) is not None:
            events.append(event)
        return events

    def encode(self, fields: dict) -> bytes:
        nonce = fields.get("nonce", 0)
        return encode_frame(
            Frame(
                type=FrameType[fields["type"]],
                stream_id=fields.get("stream_id", 0),
                resuming=fields.get("resuming", False),
                session_id=fields.get("session_id", ""),
                capabilities=bytes.fromhex(fields.get("capabilities", "")),
                kind=fields.get("kind", ""),
                params=bytes.fromhex(fields.get("params", "")),
                code=Reason[fields.get("code", "OK")],
                message=bytes.fromhex(fields.get("message", "")),
                data=data_of(fields.get("data", "")),
                nonce=self.last_ping if nonce == "last" else nonce,
            )
        )


def expect_fields(actual: object, expected: dict, name_of: type) -> None:
    assert actual.type == name_of[expected["type"]], expected
    assert actual.stream_id == expected.get("stream_id", 0), expected
    for key, value in expected.items():
        if key == "code":
            assert actual.code == Reason[value], expected
        elif key == "codec_status":
            assert actual.codec_status == CodecStatus[value], expected
        elif key in ("capabilities", "params", "message", "data"):
            assert getattr(actual, key) == data_of(value), expected
        elif key not in ("type", "stream_id"):
            assert getattr(actual, key) == value, expected


def expect_list(actual: list, expected: list, name_of: type) -> None:
    assert [a.type.name for a in actual] == [e["type"] for e in expected]
    for item, fields in zip(actual, expected):
        expect_fields(item, fields, name_of)


def do_open(h: Harness, s: dict) -> None:
    stream_id = h.call(s, "open_stream", s["kind"], bytes.fromhex(s.get("params", "")), h.now_ms)
    assert "expect_id" not in s or stream_id == s["expect_id"]


def do_write(h: Harness, s: dict) -> None:
    accepted = h.call(s, "write", s["stream"], data_of(s["data"]), h.now_ms)
    assert "expect_accepted" not in s or accepted == s["expect_accepted"]


def do_advance(h: Harness, s: dict) -> None:
    h.now_ms += s["ms"]


def do_rx(h: Harness, s: dict) -> None:
    wire = bytes.fromhex(s["wire"]) if "wire" in s else h.encode(s["frame"])
    h.call(s, "on_message", wire, h.now_ms)


def do_expect_deadline(h: Harness, s: dict) -> None:
    deadline = h.session.next_deadline_ms()
    assert (None if deadline is None else deadline - h.now_ms) == s["ms"]


def do_expect_streams(h: Harness, s: dict) -> None:
    actual = h.session.list_streams()
    assert [a.stream_id for a in actual] == [e["stream_id"] for e in s["streams"]]
    for info, expected in zip(actual, s["streams"]):
        for key, value in expected.items():
            value = StreamState[value] if key == "state" else value
            assert getattr(info, key) == value, expected


STEPS = {
    "attach": lambda h, s: h.call(s, "attach_connection", h.now_ms),
    "rx": do_rx,
    "lost": lambda h, s: h.call(s, "on_connection_lost"),
    "advance": do_advance,
    "tick": lambda h, s: h.call(s, "on_tick", h.now_ms),
    "open": do_open,
    "accept": lambda h, s: h.call(s, "accept_stream", s["stream"]),
    "reject": lambda h, s: h.call(
        s, "reject_stream", s["stream"], Reason[s["code"]], bytes.fromhex(s.get("message", ""))
    ),
    "write": do_write,
    "eof": lambda h, s: h.call(s, "send_eof", s["stream"]),
    "reset": lambda h, s: h.call(s, "reset_stream", s["stream"], Reason[s["code"]]),
    "close": lambda h, s: h.call(s, "close", Reason[s["code"]], h.now_ms),
    "drain": lambda h, s: (h.frames(), h.events()),
    "drain_frames": lambda h, s: h.frames(),
    "drain_events": lambda h, s: h.events(),
    "expect_frames": lambda h, s: expect_list(h.frames(), s["frames"], FrameType),
    "expect_events": lambda h, s: expect_list(h.events(), s["events"], EventType),
    "expect_state": lambda h, s: h.session.state == SessionState[s["state"]] or pytest.fail(s),
    "expect_deadline": do_expect_deadline,
    "expect_rtt": lambda h, s: h.session.rtt_ms == s["ms"] or pytest.fail(s),
    "expect_streams": do_expect_streams,
}


def load_scenarios() -> list[dict]:
    scenarios = []
    for path in MANIFEST["sync"]:
        scenarios += json.loads((CONFORMANCE / path).read_text())["scenarios"]
    return scenarios


SCENARIOS = load_scenarios()


def test_scenario_names_are_unique() -> None:
    names = [scenario["name"] for scenario in SCENARIOS]
    assert len(names) == len(set(names))


@pytest.mark.parametrize("scenario", SCENARIOS, ids=[s["name"] for s in SCENARIOS])
def test_scenario(scenario: dict) -> None:
    harness = Harness(scenario)
    with harness.session:
        for step in SETUPS[scenario.get("setup", "disconnected")] + scenario["steps"]:
            STEPS[step["op"]](harness, step)  # an unknown op raises KeyError, and fails
        assert harness.frames() == []
        assert harness.events() == []
