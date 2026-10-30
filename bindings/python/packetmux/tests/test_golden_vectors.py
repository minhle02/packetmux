"""The language-neutral vectors from tests/vectors/FramesV2.txt, through the binding."""

from pathlib import Path

import pytest

from packetmux import CodecError, CodecStatus, Frame, FrameType, Reason, decode_frame, encode_frame

VECTORS = Path(__file__).resolve().parents[4] / "tests/vectors/FramesV2.txt"
DEFAULT_LIMITS = {"max_data_payload": 16000, "max_control_payload": 32 * 1024}


def load_vectors() -> list[tuple[str, dict[str, str]]]:
    vectors: list[tuple[str, dict[str, str]]] = []
    for line in VECTORS.read_text().splitlines():
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        if text.startswith("["):
            vectors.append((text[1:-1], {}))
            continue
        key, _, value = text.partition("=")
        vectors[-1][1][key.strip()] = value.strip()
    return vectors


def hex_field(fields: dict[str, str], key: str) -> bytes:
    return bytes.fromhex(fields.get(key, "").replace(" ", ""))


def limits_of(fields: dict[str, str]) -> dict[str, int]:
    return {key: int(fields.get(key, value)) for key, value in DEFAULT_LIMITS.items()}


VECTOR_LIST = load_vectors()


def test_all_vectors_were_found() -> None:
    assert len(VECTOR_LIST) >= 50


@pytest.mark.parametrize("fields", [v[1] for v in VECTOR_LIST], ids=[v[0] for v in VECTOR_LIST])
def test_vector(fields: dict[str, str]) -> None:
    wire = hex_field(fields, "wire")
    limits = limits_of(fields)
    status = fields.get("status", "OK")

    if status != "OK":
        with pytest.raises(CodecError) as error:
            decode_frame(wire, **limits)
        assert error.value.status == CodecStatus[status]
        return

    expected = Frame(
        type=FrameType[fields["type"]],
        stream_id=int(fields["stream_id"]),
        version=int(fields.get("version", "2")),
        session_id=hex_field(fields, "session_id").decode("ascii"),
        capabilities=hex_field(fields, "capabilities"),
        kind=hex_field(fields, "kind").decode("ascii"),
        params=hex_field(fields, "params"),
        code=Reason[fields.get("code", "OK")],
        message=hex_field(fields, "message"),
        data=hex_field(fields, "data"),
        nonce=int(fields.get("nonce", "0")),
    )
    assert decode_frame(wire, **limits) == expected
    if fields.get("roundtrip", "true") == "true":
        assert encode_frame(expected, **limits) == wire
