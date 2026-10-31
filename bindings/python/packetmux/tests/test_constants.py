"""The IntEnums must match the C header, or every value crossing the ABI is wrong."""

import re
from pathlib import Path

import pytest

from packetmux import core

HEADER = Path(__file__).resolve().parents[4] / "include/packetmux/PacketMuxC.h"

ENUMS = {
    "PACKETMUX_STATUS": core.Status,
    "PACKETMUX_CODEC_STATUS": core.CodecStatus,
    "PACKETMUX_ROLE": core.Role,
    "PACKETMUX_REASON": core.Reason,
    "PACKETMUX_FRAME_TYPE": core.FrameType,
    "PACKETMUX_EVENT_TYPE": core.EventType,
    "PACKETMUX_SESSION_STATE": core.SessionState,
    "PACKETMUX_STREAM_STATE": core.StreamState,
}


def header_values(prefix: str) -> dict[str, int]:
    blocks = re.findall(r"enum \{(.*?)\};", HEADER.read_text(), re.S)
    for block in blocks:
        names = re.findall(r"(PACKETMUX_\w+)", block)
        if names[0].rsplit("_", 1)[0].startswith(prefix) and all(
            name.startswith(prefix + "_") and not name.startswith(prefix + "_STATUS_")
            for name in names
        ):
            members = [name[len(prefix) + 1 :] for name in names]
            return {name: value for value, name in enumerate(members) if name != "MAX"}
    raise AssertionError(f"{prefix} not found in the header")


@pytest.mark.parametrize("prefix", ENUMS)
def test_enum_matches_the_c_header(prefix: str) -> None:
    assert {member.name: member.value for member in ENUMS[prefix]} == header_values(prefix)
