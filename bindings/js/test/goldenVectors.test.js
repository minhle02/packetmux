// The language-neutral vectors from conformance/vectors/frames.txt, through the binding.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { CodecError, CodecStatus, FrameType, Reason, loadPacketMux } from '../core.js';

const text = readFileSync(new URL('../../../conformance/vectors/frames.txt', import.meta.url), 'utf8');

function loadVectors() {
  const vectors = [];
  for (const line of text.split('\n')) {
    const entry = line.trim();
    if (entry === '' || entry.startsWith('#')) {
      continue;
    }
    if (entry.startsWith('[')) {
      vectors.push({ name: entry.slice(1, -1), fields: {} });
      continue;
    }
    const separator = entry.indexOf('=');
    vectors.at(-1).fields[entry.slice(0, separator).trim()] = entry.slice(separator + 1).trim();
  }
  return vectors;
}

const hexField = (fields, key) => Uint8Array.from(Buffer.from((fields[key] ?? '').replaceAll(' ', ''), 'hex'));
const limitsOf = (fields) => ({
  ...(fields.max_data_payload && { maxDataPayload: Number(fields.max_data_payload) }),
  ...(fields.max_control_payload && { maxControlPayload: Number(fields.max_control_payload) }),
});

const vectors = loadVectors();
const { decodeFrame, encodeFrame } = await loadPacketMux();

test('all vectors were found', () => {
  assert.ok(vectors.length >= 50);
});

for (const { name, fields } of vectors) {
  test(name, () => {
    const wire = hexField(fields, 'wire');
    const limits = limitsOf(fields);
    const status = fields.status ?? 'OK';

    if (status !== 'OK') {
      assert.throws(() => decodeFrame(wire, limits), (error) => {
        assert.ok(error instanceof CodecError);
        assert.equal(error.status, CodecStatus[status]);
        return true;
      });
      return;
    }

    const expected = {
      type: FrameType[fields.type],
      streamId: Number(fields.stream_id),
      version: Number(fields.version ?? 2),
      resuming: fields.resuming === '1',
      sessionId: Buffer.from(hexField(fields, 'session_id')).toString('ascii'),
      capabilities: hexField(fields, 'capabilities'),
      kind: Buffer.from(hexField(fields, 'kind')).toString('ascii'),
      params: hexField(fields, 'params'),
      code: Reason[fields.code ?? 'OK'],
      message: hexField(fields, 'message'),
      data: hexField(fields, 'data'),
      nonce: BigInt(fields.nonce ?? 0),
    };
    assert.deepEqual(decodeFrame(wire, limits), expected);
    if ((fields.roundtrip ?? 'true') === 'true') {
      assert.deepEqual(encodeFrame(expected, limits), wire);
    }
  });
}
