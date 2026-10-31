// Runs the language-neutral sync scenarios in conformance/ through the binding.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import {
  CodecStatus, EventType, FrameType, Reason, Role, SessionError, SessionState, Status, StreamState, loadPacketMux,
} from '../index.js';

const { Session, decodeFrame, encodeFrame } = await loadPacketMux();

const conformance = new URL('../../../conformance/', import.meta.url);
const readJson = (path) => JSON.parse(readFileSync(new URL(path, conformance), 'utf8'));
const manifest = readJson('manifest.json');
const setups = readJson('sync/setups.json');

const fromHex = (value) => Uint8Array.from(Buffer.from(value, 'hex'));
const dataOf = (value) => {
  if (typeof value === 'string') {
    return fromHex(value);
  }
  const unit = fromHex(value.repeat);
  const data = new Uint8Array(unit.length * value.count);
  for (let index = 0; index < data.length; index += unit.length) {
    data.set(unit, index);
  }
  return data;
};
const camelOf = (name) => name.replace(/_(.)/g, (_, letter) => letter.toUpperCase());

class Harness {
  constructor(scenario) {
    const options = { role: Role[scenario.role.toUpperCase()], sessionId: 'session-1' };
    for (const [key, value] of Object.entries(scenario.config ?? {})) {
      options[camelOf(key)] = key === 'capabilities' ? fromHex(value) : value;
    }
    this.session = new Session(options);
    this.nowMs = 1000;
    this.lastPing = 0n;
  }

  call(step, method, ...args) {
    const expected = Status[step.expect_status ?? 'OK'];
    try {
      const result = this.session[method](...args);
      assert.equal(expected, Status.OK);
      return result;
    } catch (error) {
      if (!(error instanceof SessionError)) {
        throw error;
      }
      assert.equal(error.status, expected);
      return undefined;
    }
  }

  frames() {
    const frames = [];
    for (let message = this.session.pollTransmit(); message !== null; message = this.session.pollTransmit()) {
      const frame = decodeFrame(message);
      frames.push(frame);
      if (frame.type === FrameType.PING) {
        this.lastPing = frame.nonce;
      }
    }
    return frames;
  }

  events() {
    const events = [];
    for (let event = this.session.pollEvent(); event !== null; event = this.session.pollEvent()) {
      events.push(event);
    }
    return events;
  }

  encode(fields) {
    return encodeFrame({
      type: FrameType[fields.type],
      streamId: fields.stream_id ?? 0,
      resuming: fields.resuming ?? false,
      sessionId: fields.session_id ?? '',
      capabilities: fromHex(fields.capabilities ?? ''),
      kind: fields.kind ?? '',
      params: fromHex(fields.params ?? ''),
      code: Reason[fields.code ?? 'OK'],
      message: fromHex(fields.message ?? ''),
      data: dataOf(fields.data ?? ''),
      nonce: fields.nonce === 'last' ? this.lastPing : BigInt(fields.nonce ?? 0),
    });
  }
}

function expectFields(actual, expected, names) {
  assert.equal(actual.type, names[expected.type]);
  assert.equal(actual.streamId, expected.stream_id ?? 0);
  for (const [key, value] of Object.entries(expected)) {
    if (key === 'code') {
      assert.equal(actual.code, Reason[value]);
    } else if (key === 'codec_status') {
      assert.equal(actual.codecStatus, CodecStatus[value]);
    } else if (['capabilities', 'params', 'message', 'data'].includes(key)) {
      assert.deepEqual(actual[key], dataOf(value));
    } else if (key === 'nonce') {
      assert.equal(actual.nonce, BigInt(value));
    } else if (key !== 'type' && key !== 'stream_id') {
      assert.equal(actual[camelOf(key)], value);
    }
  }
}

function expectList(actual, expected, names) {
  const nameOf = (type) => Object.keys(names).find((name) => names[name] === type);
  assert.deepEqual(actual.map((item) => nameOf(item.type)), expected.map((item) => item.type));
  actual.forEach((item, index) => expectFields(item, expected[index], names));
}

const steps = {
  attach: (h, s) => h.call(s, 'attachConnection', h.nowMs),
  rx: (h, s) => h.call(s, 'onMessage', s.wire === undefined ? h.encode(s.frame) : fromHex(s.wire), h.nowMs),
  lost: (h, s) => h.call(s, 'onConnectionLost'),
  advance: (h, s) => { h.nowMs += s.ms; },
  tick: (h, s) => h.call(s, 'onTick', h.nowMs),
  open: (h, s) => {
    const streamId = h.call(s, 'openStream', s.kind, fromHex(s.params ?? ''), h.nowMs);
    if (s.expect_id !== undefined) {
      assert.equal(streamId, s.expect_id);
    }
  },
  accept: (h, s) => h.call(s, 'acceptStream', s.stream),
  reject: (h, s) => h.call(s, 'rejectStream', s.stream, Reason[s.code], fromHex(s.message ?? '')),
  write: (h, s) => {
    const accepted = h.call(s, 'write', s.stream, dataOf(s.data), h.nowMs);
    if (s.expect_accepted !== undefined) {
      assert.equal(accepted, s.expect_accepted);
    }
  },
  eof: (h, s) => h.call(s, 'sendEof', s.stream),
  reset: (h, s) => h.call(s, 'resetStream', s.stream, Reason[s.code]),
  close: (h, s) => h.call(s, 'close', Reason[s.code], h.nowMs),
  drain: (h) => { h.frames(); h.events(); },
  drain_frames: (h) => { h.frames(); },
  drain_events: (h) => { h.events(); },
  expect_frames: (h, s) => expectList(h.frames(), s.frames, FrameType),
  expect_events: (h, s) => expectList(h.events(), s.events, EventType),
  expect_state: (h, s) => assert.equal(h.session.state, SessionState[s.state]),
  expect_deadline: (h, s) => {
    const deadline = h.session.nextDeadlineMs();
    assert.equal(deadline === null ? null : deadline - h.nowMs, s.ms);
  },
  expect_rtt: (h, s) => assert.equal(h.session.rttMs, s.ms),
  expect_streams: (h, s) => {
    const actual = h.session.listStreams();
    assert.deepEqual(actual.map((info) => info.streamId), s.streams.map((info) => info.stream_id));
    actual.forEach((info, index) => {
      for (const [key, value] of Object.entries(s.streams[index])) {
        assert.equal(info[camelOf(key)], key === 'state' ? StreamState[value] : value);
      }
    });
  },
};

const scenarios = manifest.sync.flatMap((path) => readJson(path).scenarios);

test('scenario names are unique', () => {
  assert.equal(new Set(scenarios.map((scenario) => scenario.name)).size, scenarios.length);
});

for (const scenario of scenarios) {
  test(scenario.name, () => {
    const harness = new Harness(scenario);
    try {
      for (const step of [...setups[scenario.setup ?? 'disconnected'], ...scenario.steps]) {
        assert.ok(steps[step.op], `unknown step ${step.op}`);
        steps[step.op](harness, step);
      }
      assert.deepEqual(harness.frames(), []);
      assert.deepEqual(harness.events(), []);
    } finally {
      harness.session.destroy();
    }
  });
}
