// Test tools for the SDK and for consumers: a memory transport with fault injection, a network
// of them, and the checks any Transport has to pass (FR-CONF-5).
import assert from 'node:assert/strict';

import { GiveUp, TransportClosed } from './errors.js';
import { Signal, TIMED_OUT, sleep, withTimeout } from './signal.js';
import { PushTransport } from './transport.js';

/** One side of a memory link. Tests can stall it, release it and break it. */
export class MemoryTransport extends PushTransport {
  peer = null;
  #stalled = false;
  #held = [];
  #room = new Signal();
  #isClosed = false;

  constructor(maxMessageSize = 1 << 20) {
    super();
    this.maxMessageSize = maxMessageSize;
    this.#room.set();
  }

  get bufferedAmount() {
    return this.#held.reduce((total, message) => total + message.length, 0);
  }

  sendMessage(message) {
    if (this.#stalled) {
      this.#held.push(message.slice());
    } else {
      this.#deliver(message.slice());
    }
  }

  async waitBufferBelow(limit) {
    while (this.#stalled && !this.#isClosed) {
      this.#room.clear();
      await this.#room.wait();
    }
    if (this.#isClosed) {
      throw new TransportClosed();
    }
    return super.waitBufferBelow(limit);
  }

  closeSource() {
    this.#isClosed = true;
    this.#room.set();
    this.peer?.handleClose();
  }

  // --- Fault injection ---------------------------------------------------------------------

  /** The link is full: the sender waits, and nothing is delivered. */
  stall() {
    this.#stalled = true;
  }

  release() {
    this.#stalled = false;
    this.#held.splice(0).forEach((message) => this.#deliver(message));
    this.handleBufferLow();
    this.#room.set();
  }

  /** The link dies, on both sides. */
  async breakConnection() {
    await this.close();
  }

  /** This side sees the link die. The peer still thinks it is alive. */
  failLocally() {
    this.#isClosed = true;
    this.#room.set();
    this.handleClose();
  }

  #deliver(message) {
    this.peer?.handleMessage(message);
  }
}

export function memoryTransportPair(maxMessageSize = 1 << 20) {
  const a = new MemoryTransport(maxMessageSize);
  const b = new MemoryTransport(maxMessageSize);
  a.peer = b;
  b.peer = a;
  return [a, b];
}

/** Connects clients to a Server over memory transports. network.dial is a dial function. */
export class MemoryNetwork {
  links = [];
  dials = 0;
  #server;
  #failures = 0;
  #giveUp = false;
  #flowing = new Signal();

  constructor(server, context = undefined) {
    this.#server = server;
    this.context = context;
    this.#flowing.set();
    this.dial = this.dial.bind(this);
  }

  failNext(count) {
    this.#failures = count;
  }

  giveUp() {
    this.#giveUp = true;
  }

  /** Dials wait until this is called with false. */
  hang(value = true) {
    if (value) {
      this.#flowing.clear();
    } else {
      this.#flowing.set();
    }
  }

  async dial() {
    this.dials += 1;
    if (this.#giveUp) {
      throw new GiveUp('refused for good');
    }
    if (this.#failures > 0) {
      this.#failures -= 1;
      throw new Error('no route to host');
    }
    await this.#flowing.wait();
    const [client, server] = memoryTransportPair();
    this.links.push([client, server]);
    void this.#server.serve(server, this.context).catch(() => {});
    return client;
  }
}

async function recv(transport, ms = 10_000) {
  const message = await withTimeout(transport.recv(), ms);
  assert.notEqual(message, TIMED_OUT, 'recv timed out');
  return message;
}

function fill(value, size) {
  return new Uint8Array(size).fill(value);
}

async function expectClosed(call) {
  await assert.rejects(call(), TransportClosed);
}

export const TRANSPORT_CHECKS = {
  async order(a, b) {
    const largest = Math.min(a.maxMessageSize, 60_000);
    const messages = Array.from({ length: 80 }, (_, i) => fill(i % 256, 1 + (i * 37) % largest));
    for (const message of messages) {
      await a.send(message);
    }
    for (const message of messages) {
      assert.deepEqual(await recv(b), message);
    }
  },
  async 'both-directions'(a, b) {
    await Promise.all([a.send(fill(1, 4)), b.send(fill(2, 4))]);
    assert.deepEqual(await recv(b), fill(1, 4));
    assert.deepEqual(await recv(a), fill(2, 4));
  },
  async oversize(a) {
    await assert.rejects(a.send(new Uint8Array(a.maxMessageSize + 1)));
  },
  async bulk(a, b) {
    const chunk = Uint8Array.from({ length: 16_000 }, (_, i) => i % 251);
    const count = 320; // about 5 MB
    const sending = (async () => {
      for (let i = 0; i < count; i += 1) {
        await a.waitBufferBelow(1 << 20);
        await a.send(chunk);
      }
    })();
    for (let i = 0; i < count; i += 1) {
      assert.deepEqual(await recv(b, 30_000), chunk);
    }
    await sending;
    await a.waitBufferBelow(1);
    assert.equal(a.bufferedAmount, 0);
  },
  async 'buffer-wait'(a, b) {
    await a.send(fill(7, 1000));
    await a.waitBufferBelow(1 << 20); // well above what is buffered
    assert.deepEqual(await recv(b), fill(7, 1000));
    await a.waitBufferBelow(1);
  },
  async 'early-messages'(a, b) {
    await a.send(fill(3, 5));
    await sleep(50); // nobody is in recv while it arrives
    assert.deepEqual(await recv(b), fill(3, 5));
  },
  async 'close-fails-operations'(a) {
    await a.close();
    await expectClosed(() => a.send(fill(1, 1)));
    await expectClosed(() => a.recv());
  },
  async 'peer-close'(a, b) {
    await a.close();
    await expectClosed(() => b.recv());
  },
  async 'close-is-idempotent'(a) {
    await a.close();
    await a.close();
  },
};

export async function runTransportCheck(name, factory) {
  const [a, b] = await factory();
  try {
    await TRANSPORT_CHECKS[name](a, b);
  } finally {
    await a.close();
    await b.close();
  }
}

/** Runs every check on your Transport. factory returns a connected pair of it. */
export async function checkTransport(factory) {
  for (const name of Object.keys(TRANSPORT_CHECKS)) {
    await runTransportCheck(name, factory);
  }
}
