// Acceptance test: two sessions driven through the whole lifecycle from JS.
import assert from 'node:assert/strict';
import { beforeEach, test } from 'node:test';

import { EventType, Reason, Role, SessionError, SessionState, Status, StreamState, loadPacketMux } from '../core.js';

const NOW_MS = 1000;
const bytes = (value) => new TextEncoder().encode(value);
const text = (value) => new TextDecoder().decode(value);

const { Session } = await loadPacketMux();

class Peer {
  constructor(role, sessionId) {
    this.session = new Session({ role, sessionId });
    this.events = [];
  }

  drain() {
    for (let event = this.session.pollEvent(); event !== null; event = this.session.pollEvent()) {
      this.events.push(event);
      if (event.type === EventType.DATA) {
        this.session.consume(event.streamId, event.data.length);
      }
    }
  }

  take() {
    this.drain();
    const events = this.events;
    this.events = [];
    return events;
  }
}

function transfer(sender, receiver, nowMs = NOW_MS) {
  for (let message = sender.session.pollTransmit(); message !== null; message = sender.session.pollTransmit()) {
    receiver.session.onMessage(message, nowMs);
    receiver.drain();
  }
  sender.drain();
}

function pump(client, server) {
  transfer(client, server);
  transfer(server, client);
  transfer(client, server);
}

const types = (events) => events.map((event) => event.type);

let client;
let server;

beforeEach(() => {
  client = new Peer(Role.CLIENT, 'client-1');
  server = new Peer(Role.SERVER, 'server-1');
  client.session.attachConnection(NOW_MS);
  server.session.attachConnection(NOW_MS);
  pump(client, server);
  assert.equal(client.take()[0].peerSessionId, 'server-1');
  assert.equal(server.take()[0].peerSessionId, 'client-1');
  assert.equal(client.session.state, SessionState.ESTABLISHED);
});

test('request and reply with a clean close', () => {
  const streamId = client.session.openStream('tcp-forward', bytes('{}'), NOW_MS);
  assert.equal(client.session.write(streamId, bytes('request'), NOW_MS), 7);
  client.session.sendEof(streamId);
  transfer(client, server);

  const [opened, data, eof] = server.take();
  assert.equal(opened.type, EventType.STREAM_OPENED);
  assert.equal(opened.kind, 'tcp-forward');
  assert.equal(text(opened.params), '{}');
  assert.equal(text(data.data), 'request');
  assert.equal(eof.type, EventType.EOF);

  server.session.acceptStream(streamId);
  server.session.write(streamId, bytes('the reply'), NOW_MS);
  server.session.sendEof(streamId);
  transfer(server, client);

  assert.deepEqual(types(server.take()), [EventType.STREAM_CLOSED]);
  const [accepted, reply, replyEof, closed] = client.take();
  assert.equal(accepted.type, EventType.STREAM_ACCEPTED);
  assert.equal(text(reply.data), 'the reply');
  assert.equal(replyEof.type, EventType.EOF);
  assert.equal(closed.type, EventType.STREAM_CLOSED);
  assert.equal(closed.code, Reason.OK);
  assert.deepEqual(client.session.listStreams(), []);
});

test('events own their bytes, even when WASM memory grows', () => {
  const streamId = client.session.openStream('kind', null, NOW_MS);
  transfer(client, server);
  server.take();
  server.session.acceptStream(streamId);
  transfer(server, client);
  client.take();

  client.session.write(streamId, bytes('one'), NOW_MS);
  transfer(client, server);
  const kept = server.take()[0];

  // Far more than the module starts with, moved in pieces the queues accept.
  const big = new Uint8Array(32 * 1024 * 1024).fill(7);
  let offset = 0;
  while (offset < big.length) {
    offset += client.session.write(streamId, big.subarray(offset), NOW_MS);
    transfer(client, server);
    server.drain();
    transfer(server, client);
  }
  pump(client, server); // what was still waiting for credit
  const received = server.take().reduce((total, event) => total + event.data.length, 0);
  assert.equal(received, big.length);
  assert.equal(text(kept.data), 'one');
});

test('reject and reset', () => {
  const rejected = client.session.openStream('kind', null, NOW_MS);
  transfer(client, server);
  server.session.rejectStream(rejected, Reason.POLICY_DENIED, bytes('no'));
  transfer(server, client);
  const [refused] = client.take();
  assert.equal(refused.type, EventType.STREAM_CLOSED);
  assert.equal(refused.code, Reason.POLICY_DENIED);
  server.take();

  const reset = client.session.openStream('kind', null, NOW_MS);
  client.session.write(reset, bytes('abc'), NOW_MS);
  const [info] = client.session.listStreams();
  assert.equal(info.streamId, reset);
  assert.equal(info.kind, 'kind');
  assert.equal(info.opener, Role.CLIENT);
  assert.equal(info.state, StreamState.SENDER_OPENING);
  assert.equal(info.bytesSent, 3);
  assert.equal(info.createdAtMs, NOW_MS);

  client.session.resetStream(reset, Reason.TIMEOUT);
  pump(client, server);
  assert.equal(client.take().at(-1).code, Reason.TIMEOUT);
  assert.equal(server.take().at(-1).code, Reason.TIMEOUT);
});

test('errors become exceptions', () => {
  assert.throws(() => client.session.sendEof(99), (error) => {
    assert.ok(error instanceof SessionError);
    assert.equal(error.status, Status.UNKNOWN_STREAM);
    return true;
  });
  assert.throws(() => client.session.openStream('not_valid', null, NOW_MS),
    (error) => error.status === Status.INVALID_ARGUMENT);
});

test('a refused message does not free the one events still borrow', () => {
  const streamId = client.session.openStream('kind', null, NOW_MS);
  client.session.write(streamId, bytes('payload'), NOW_MS);
  const open = client.session.pollTransmit();
  const data = client.session.pollTransmit();

  server.session.onMessage(open, NOW_MS);
  assert.throws(() => server.session.onMessage(data, NOW_MS),
    (error) => error.status === Status.EVENTS_PENDING);
  const [opened] = server.take();
  assert.equal(opened.kind, 'kind');
});

test('keepalive measures RTT', () => {
  assert.equal(client.session.rttMs, null);
  const deadline = client.session.nextDeadlineMs();
  client.session.onTick(deadline);
  server.session.onMessage(client.session.pollTransmit(), deadline);
  client.session.onMessage(server.session.pollTransmit(), deadline + 25);
  assert.equal(client.session.rttMs, 25);
});

test('reconnect keeps the session', () => {
  const before = client.session.openStream('kind', null, NOW_MS);
  client.session.onConnectionLost();
  server.session.onConnectionLost();
  assert.deepEqual(types(client.take()), [EventType.STREAM_CLOSED, EventType.DISCONNECTED]);
  server.take();
  assert.equal(client.session.state, SessionState.DISCONNECTED);

  client.session.attachConnection(NOW_MS);
  server.session.attachConnection(NOW_MS);
  pump(client, server);
  assert.equal(client.session.state, SessionState.ESTABLISHED);
  assert.ok(client.session.openStream('kind', null, NOW_MS) > before);
});

test('connection lost after the end does nothing', () => {
  client.session.onConnectionLost();
  assert.deepEqual(types(client.take()), [EventType.DISCONNECTED]);
  client.session.onConnectionLost();
  assert.deepEqual(client.take(), []);

  client.session.close(Reason.OK, NOW_MS);
  assert.deepEqual(types(client.take()), [EventType.SESSION_CLOSED]);
  client.session.onConnectionLost();
  assert.deepEqual(client.take(), []);
});

test('close ends both sessions', () => {
  client.session.close(Reason.AUTH_REVOKED, NOW_MS);
  assert.equal(client.session.state, SessionState.CLOSING);
  transfer(client, server);
  assert.equal(client.session.state, SessionState.CLOSED);
  assert.equal(server.session.state, SessionState.CLOSED);
  const [closed] = client.take();
  assert.equal(closed.type, EventType.SESSION_CLOSED);
  assert.equal(closed.code, Reason.AUTH_REVOKED);
  assert.equal(client.session.nextDeadlineMs(), null);
});

test('destroy can be repeated', () => {
  client.session.destroy();
  client.session.destroy();
});
