// The constants must match the C header, or every value crossing the ABI is wrong.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { CodecStatus, EventType, FrameType, Reason, Role, SessionState, Status, StreamState } from '../core.js';

const header = readFileSync(new URL('../../../include/packetmux/PacketMuxC.h', import.meta.url), 'utf8');
const blocks = [...header.matchAll(/enum \{(.*?)\};/gs)].map((match) => [...match[1].matchAll(/PACKETMUX_\w+/g)].map((name) => name[0]));

function headerValues(prefix) {
  const block = blocks.find((names) => names.every((name) => name.startsWith(`${prefix}_`)
    && !name.startsWith(`${prefix}_STATUS_`)));
  assert.ok(block, `${prefix} not found`);
  return Object.fromEntries(block.map((name, value) => [name.slice(prefix.length + 1), value])
    .filter(([name]) => name !== 'MAX'));
}

const ENUMS = {
  PACKETMUX_STATUS: Status,
  PACKETMUX_CODEC_STATUS: CodecStatus,
  PACKETMUX_ROLE: Role,
  PACKETMUX_REASON: Reason,
  PACKETMUX_FRAME_TYPE: FrameType,
  PACKETMUX_EVENT_TYPE: EventType,
  PACKETMUX_SESSION_STATE: SessionState,
  PACKETMUX_STREAM_STATE: StreamState,
};

for (const [prefix, values] of Object.entries(ENUMS)) {
  test(`${prefix} matches the C header`, () => {
    assert.deepEqual({ ...values }, headerValues(prefix));
  });
}
