// Runs the language-neutral SDK layers in conformance/: params, async and transport.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import * as packetmux from '../index.js';
import { FrameType, loadPacketMux } from '../core.js';
import { decodeParams, encodeParams, typeOf } from '../params.js';
import { sleep, withTimeout, TIMED_OUT } from '../signal.js';
import {
  MemoryNetwork, TRANSPORT_CHECKS, memoryTransportPair, runTransportCheck,
} from '../testing.js';

const {
  ClientState, InvalidParams, Reason, Refuse, Reject, Server, TransportClosed, connect, defineStream,
} = packetmux;

const conformance = new URL('../../../conformance/', import.meta.url);
const readJson = (path) => JSON.parse(readFileSync(new URL(path, conformance), 'utf8'));
const manifest = readJson('manifest.json');
const TIMEOUT = 5000;

const fromHex = (value) => Uint8Array.from(Buffer.from(value, 'hex'));
function dataOf(value) {
  if (typeof value === 'string') {
    return fromHex(value);
  }
  const unit = fromHex(value.repeat);
  const data = new Uint8Array(unit.length * value.count);
  for (let index = 0; index < data.length; index += unit.length) {
    data.set(unit, index);
  }
  return data;
}

async function timed(promise, ms = TIMEOUT) {
  const result = await withTimeout(promise, ms);
  assert.notEqual(result, TIMED_OUT, 'timed out');
  return result;
}

// --- params ------------------------------------------------------------------------------------

for (const path of manifest.params) {
  const suite = readJson(path);
  for (const testCase of suite.cases) {
    test(`params: ${testCase.name}`, () => {
      const type = defineStream('kind', suite.fields);
      const data = fromHex(testCase.input);
      if (testCase.rejected) {
        assert.throws(() => decodeParams(type, data), InvalidParams);
      } else {
        assert.deepEqual(decodeParams(type, data), testCase.result);
      }
    });
  }
  for (const testCase of suite.encode_rejected) {
    test(`params, outgoing: ${testCase.name}`, () => {
      const type = defineStream('kind', suite.fields);
      assert.throws(() => encodeParams(type(testCase.value)), InvalidParams);
    });
  }
}

// --- transport ---------------------------------------------------------------------------------

const checks = manifest.transport.flatMap((path) => readJson(path).checks);

test('every transport check exists', () => {
  assert.deepEqual([...checks].sort(), Object.keys(TRANSPORT_CHECKS).sort());
});

for (const name of checks) {
  test(`transport: ${name}`, () => runTransportCheck(name, memoryTransportPair));
}

// --- async -------------------------------------------------------------------------------------

class Harness {
  handlerSaw = [];
  serverCloseSeen = [];
  sessionsClosed = [];
  states = [];
  smallTransport = 0;
  clients = new Map();
  dialIds = [];
  streams = new Map();
  tasks = new Map();
  dialsSeen = 0;
  raw = null;

  constructor(scenario) {
    const { options } = scenario;
    this.handlers = Object.entries(scenario.handlers).map(([kind, spec]) => this.handler(kind, spec));
    this.clientOptions = {
      connectTimeoutMs: options.connect_timeout_ms,
      reconnectBackoffMs: options.reconnect_backoff_ms,
      maxReadBuffer: options.max_read_buffer ?? 16 << 20,
    };
    const session = {};
    if (options.session?.hello_timeout_ms !== undefined) {
      session.helloTimeoutMs = options.session.hello_timeout_ms;
    }
    this.server = new Server({
      handlers: this.handlers,
      authorize: this.authorizeWith(scenario),
      onSessionClosed: (closed, code) => {
        assert.ok(!this.server.sessions().includes(closed));
        this.sessionsClosed.push(code);
        if (scenario.session_hook === 'raise') {
          throw new Error('hook failed');
        }
      },
      gracePeriodMs: options.grace_period_ms ?? 120_000,
      maxSessions: options.max_sessions ?? 1024,
      maxReadBuffer: options.max_read_buffer ?? 16 << 20,
      session,
    });
    this.network = new MemoryNetwork(this.server);
    this.dial = async (sessionId) => {
      this.dialIds.push(sessionId);
      const transport = await this.network.dial(sessionId);
      if (this.smallTransport > 0) {
        transport.maxMessageSize = this.smallTransport;
      }
      return transport;
    };
  }

  authorizeWith(scenario) {
    const rule = scenario.authorize;
    if (rule === undefined) {
      return undefined;
    }
    return async (request) => {
      if (rule.startsWith('refuse:')) {
        throw new Refuse(Reason[rule.split(':')[1]]);
      }
      if (request.session !== null && request.session.data !== request.context) {
        throw new Refuse(Reason.AUTH_REVOKED);
      }
      return request.context;
    };
  }

  handler(kind, spec) {
    const name = typeof spec === 'string' ? spec : spec.behaviour;
    const target = typeof spec === 'string' ? kind : defineStream(kind, spec.fields);
    const [action, argument] = name.split(':');
    const handle = async (request) => {
      switch (action) {
        case 'echo': {
          const stream = request.accept();
          for await (const chunk of stream) {
            await stream.write(chunk);
          }
          await stream.sendEof();
          break;
        }
        case 'reject':
          throw new Reject(Reason[argument]);
        case 'fail':
          throw new Error('handler failed');
        case 'accept-then-fail':
          request.accept();
          throw new Error('handler failed');
        case 'reply':
          await request.accept().write(dataOf(argument));
          break;
        case 'reply-then-reset': {
          const stream = request.accept();
          await stream.write(dataOf(argument));
          await sleep(20);
          stream.reset(Reason.KILLED);
          break;
        }
        case 'flood':
          await request.accept().write(new Uint8Array(Number(argument)));
          await new Promise(() => {});
          break;
        case 'record-close':
          this.handlerSaw.push(await request.accept().waitClosed());
          break;
        case 'reply-params':
          await request.accept().write(
            new TextEncoder().encode(`${request.params.host}:${request.params.port}`),
          );
          break;
        case 'reply-raw-params':
          await request.accept().write(request.params);
          break;
        case 'return':
          break;
        default:
          throw new Error(`unknown behaviour ${name}`);
      }
    };
    return [target, handle];
  }

  client(step) {
    return this.clients.get(step.client ?? 'client');
  }

  session() {
    return this.server.sessions()[0];
  }

  link(step) {
    return this.network.links.at(step.link === 'previous' ? -2 : -1);
  }

  transportOf(step) {
    return this.link(step)[step.side === 'client' ? 0 : 1];
  }

  async until(condition, what) {
    const deadline = performance.now() + TIMEOUT;
    while (!condition()) {
      assert.ok(performance.now() < deadline, `timed out waiting for ${JSON.stringify(what)}`);
      await sleep(2);
    }
  }

  checkError(step, error) {
    assert.ok(error instanceof packetmux[step.expect_error], String(error));
    if (step.expect_code !== undefined) {
      assert.equal(error.code, Reason[step.expect_code]);
    }
  }

  background(step, promise) {
    if (step.background === undefined) {
      return false;
    }
    promise.catch(() => {});
    this.tasks.set(step.background, promise);
    return true;
  }
}

async function openStream(h, s, target, params) {
  const on = s.on ?? 'client';
  const owner = on === 'server' ? h.session() : h.clients.get(on);
  try {
    h.streams.set(s.as, await owner.openStream(target, params));
    assert.equal(s.expect_error, undefined);
  } catch (error) {
    if (s.expect_error === undefined) {
      throw error;
    }
    h.checkError(s, error);
  }
}

function checkCode(error, step) {
  if (step.code !== undefined) {
    assert.equal(error.code, Reason[step.code]);
  }
  return true;
}

async function rawConnect(h, s) {
  const [near, far] = memoryTransportPair();
  h.raw = near;
  if (s.wire !== undefined) {
    await near.send(fromHex(s.wire));
  }
  h.tasks.set('raw', h.server.serve(far, h.network.context));
}

const asyncSteps = {
  async await(h, s) {
    if (s.error === undefined) {
      await timed(h.tasks.get(s.task));
    } else {
      await assert.rejects(timed(h.tasks.get(s.task)), (error) => (
        error instanceof packetmux[s.error] && checkCode(error, s)));
    }
  },
  async break(h, s) { await h.link(s)[0].breakConnection(); },
  async cancel_connect(h) {
    const controller = new AbortController();
    const connecting = connect(h.dial, { ...h.clientOptions, handlers: h.handlers, signal: controller.signal });
    await sleep(5);
    controller.abort();
    await assert.rejects(timed(connecting));
  },
  async cancel_read(h, s) {
    const controller = new AbortController();
    const reading = h.streams.get(s.stream).read({ signal: controller.signal });
    await sleep(5);
    controller.abort();
    await assert.rejects(reading);
  },
  async close(h, s) {
    const closing = h.client(s).close(Reason[s.code ?? 'OK']);
    if (!h.background(s, closing)) {
      await timed(closing);
    }
  },
  async close_session(h, s) { await timed(h.session().close(Reason[s.code])); },
  async connect(h, s) {
    try {
      const options = { ...h.clientOptions, handlers: h.handlers };
      h.clients.set(s.as ?? 'client', await connect(h.dial, options));
      assert.equal(s.expect_error, undefined);
    } catch (error) {
      if (s.expect_error === undefined) {
        throw error;
      }
      h.checkError(s, error);
    }
  },
  async dial_count(h) { h.dialsSeen = h.network.dials; },
  async dial_fails(h, s) { h.network.failNext(s.count); },
  async dial_gives_up(h) { h.network.giveUp(); },
  async dial_hangs(h) { h.network.hang(); },
  async dial_works(h) { h.network.hang(false); },
  async drop(h, s) { h.transportOf({ side: 'client', ...s }).failLocally(); },
  async inject(h, s) { h.transportOf(s).handleMessage(fromHex(s.wire)); },
  async eof(h, s) { await h.streams.get(s.stream).sendEof(); },
  async expect_closed(h, s) {
    const client = h.client(s);
    await timed(client.waitClosed());
    assert.equal(client.state, ClientState.CLOSED);
    if (s.error === undefined) {
      assert.equal(client.closeError, null);
      return;
    }
    assert.ok(client.closeError instanceof packetmux[s.error], String(client.closeError));
    checkCode(client.closeError, s);
  },
  async expect_connected(h, s) { assert.equal(h.client(s).state, ClientState.CONNECTED); },
  async expect_dial_ids_match(h, s) { assert.ok(h.dialIds.includes(h.clients.get(s.client).sessionId)); },
  async expect_dials(h, s) { await h.until(() => h.network.dials >= s.min, s); },
  async expect_eof(h, s) { assert.equal(await timed(h.streams.get(s.stream).read()), null); },
  async expect_error(h, s) {
    const stream = h.streams.get(s.stream);
    const calls = {
      read: () => stream.read(),
      wait_accepted: () => stream.waitAccepted(),
      write: () => stream.write(new Uint8Array(1)),
    };
    await assert.rejects(timed(calls[s.call]()), (error) => (
      error instanceof packetmux[s.error] && checkCode(error, s)));
  },
  async expect_handler_saw(h, s) { await h.until(() => h.handlerSaw.includes(Reason[s.code]), s); },
  async expect_high_water(h, s) { assert.equal(h.streams.get(s.stream).readHighWater, s.value); },
  async expect_no_new_dials(h) { assert.equal(h.network.dials, h.dialsSeen); },
  async expect_no_session_closed(h) { assert.deepEqual(h.sessionsClosed, []); },
  async expect_pending(h, s) {
    assert.equal(await withTimeout(h.tasks.get(s.task), 1), TIMED_OUT);
  },
  async expect_raw_closed(h) {
    const untilClosed = (async () => {
      for (;;) {
        await h.raw.recv();
      }
    })();
    await assert.rejects(timed(untilClosed), TransportClosed);
  },
  async expect_read(h, s) {
    const expected = dataOf(s.data);
    const stream = h.streams.get(s.stream);
    const chunks = [];
    let size = 0;
    while (size < expected.length) {
      const chunk = await timed(stream.read());
      assert.notEqual(chunk, null, 'end of stream too early');
      chunks.push(chunk);
      size += chunk.length;
    }
    assert.ok(Buffer.concat(chunks).equals(Buffer.from(expected)), 'wrong bytes');
  },
  async expect_server_close(h, s) { assert.deepEqual(h.serverCloseSeen, [Reason[s.code]]); },
  async expect_session_closed(h, s) {
    await h.until(() => h.sessionsClosed.length > 0, s);
    assert.deepEqual(h.sessionsClosed, [Reason[s.code]]);
  },
  async expect_session_disconnected(h, s) { await h.until(() => !h.session().connected, s); },
  async expect_session_ids_differ(h, s) {
    const a = h.clients.get(s.a).sessionId;
    assert.notEqual(a, h.clients.get(s.b).sessionId);
    assert.match(a, /^[a-zA-Z0-9-]{16,}$/);
  },
  async expect_session_transport(h, s) {
    const expected = { current: h.network.links.at(-1)[1], none: null }[s.is];
    await h.until(() => h.session().transport === expected, s);
  },
  async expect_sessions(h, s) { await h.until(() => h.server.sessions().length === s.count, s); },
  async expect_state(h, s) { await h.until(() => h.client(s).state === s.state, s); },
  async expect_states(h, s) { assert.deepEqual(h.states, s.states); },
  async expect_stream_closed(h, s) {
    assert.equal(await timed(h.streams.get(s.stream).waitClosed()), Reason[s.code]);
  },
  async open(h, s) {
    const opening = openStream(h, s, s.kind, fromHex(s.params ?? ''));
    if (!h.background(s, opening)) {
      await opening;
    }
  },
  async open_typed(h, s) {
    await openStream(h, s, defineStream(s.kind, s.fields)(s.value), undefined);
  },
  raw_connect: rawConnect,
  async raw_hello(h, s) {
    const { encodeFrame } = await loadPacketMux();
    const hello = encodeFrame({
      type: FrameType.HELLO,
      sessionId: h.clients.get(s.session_of).sessionId,
      resuming: s.resuming,
    });
    await rawConnect(h, { wire: Buffer.from(hello).toString('hex') });
  },
  async record_server_close(h) {
    const session = h.session();
    const onClosed = session.onClosed.bind(session);
    session.onClosed = (code, connection) => {
      h.serverCloseSeen.push(code);
      onClosed(code, connection);
    };
  },
  async record_states(h, s) { h.client(s).onStateChange((state) => h.states.push(state)); },
  async release(h, s) { h.transportOf(s).release(); },
  async reset(h, s) { h.streams.get(s.stream).reset(Reason[s.code]); },
  async set_context(h, s) { h.network.context = s.value; },
  async settle(h, s) { await sleep(s.ms ?? 20); },
  async small_transport(h, s) { h.smallTransport = s.max_message_size; },
  async stall(h, s) { h.transportOf(s).stall(); },
  async wait_accepted(h, s) { await timed(h.streams.get(s.stream).waitAccepted()); },
  async write(h, s) {
    const writing = h.streams.get(s.stream).write(dataOf(s.data));
    if (!h.background(s, writing)) {
      await writing;
    }
  },
  async write_concurrently(h, s) {
    await Promise.all(s.writes.map((data) => h.streams.get(s.stream).write(dataOf(data))));
  },
};

const scenarios = manifest.async.flatMap((path) => readJson(path).scenarios);

for (const scenario of scenarios) {
  test(`async: ${scenario.name}`, { timeout: 20_000 }, async () => {
    const harness = new Harness(scenario);
    try {
      for (const step of scenario.steps) {
        assert.ok(asyncSteps[step.op], `unknown step ${step.op}`);
        await asyncSteps[step.op](harness, step);
      }
    } finally {
      harness.network.hang(false);
      await Promise.all([...harness.clients.values()].map((client) => timed(client.close())));
      await timed(harness.server.close());
    }
  });
}

test('typed params carry their kind', () => {
  const type = defineStream('tcp-forward', { host: 'string' });
  assert.equal(typeOf(type({ host: 'a' })), type);
  assert.equal(type.kind, 'tcp-forward');
});
