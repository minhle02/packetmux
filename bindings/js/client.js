// The client: connect through a dial function, and reconnect through it too.
//
// The client is a state machine with one class per state, like the core (D12). The dialing
// states start the dial loop on entry, so nothing else here knows about retries.
import { Reason, Role, loadPacketMux } from './core.js';
import { GiveUp, NoConnection, PacketMuxError, SessionClosed, SessionNotFound } from './errors.js';
import { SessionHost, handlerTable } from './host.js';
import { Signal, TIMED_OUT, raceAbort, sleep, withTimeout } from './signal.js';

export const ClientState = Object.freeze({
  CONNECTING: 'connecting',
  CONNECTED: 'connected',
  RECONNECTING: 'reconnecting',
  CLOSED: 'closed',
});

const DEFAULTS = {
  connectTimeoutMs: 10_000,
  reconnectBackoffMs: [500, 1_000, 2_000, 4_000, 8_000], // the last value repeats
  inflightLimit: 64 * 1024,
  session: {},
};

class State {
  id = ClientState.CONNECTING;

  constructor(client) {
    this.client = client;
  }

  onEnter() {}

  onExit() {}

  onConnected() {}

  onLost() {}

  async openStream(target, params, deadline, signal) {
    await this.client._waitReady(deadline, signal);
    return this.client._state.openStream(target, params, deadline, signal);
  }
}

class DialingState extends State {
  #stop = new Signal();

  onEnter() {
    void this.#dialLoop();
  }

  onExit() {
    this.#stop.set();
  }

  onConnected() {
    this.client._enter(ConnectedState);
  }

  async #dialLoop() {
    const { client } = this;
    const backoff = client._options.reconnectBackoffMs;
    for (let attempt = 0; !this.#stop.isSet; attempt += 1) {
      try {
        const transport = await this.#dial();
        if (transport !== null) {
          client._host.checkMessageSize(transport);
          const connection = client._host.attach(transport);
          await Promise.race([connection.settled.wait(), this.#stop.wait()]);
        }
      } catch (error) {
        if (error instanceof GiveUp) {
          client._fail(error);
          return;
        }
        client._lastDialError = error;
      }
      if (this.#stop.isSet) {
        return;
      }
      await Promise.race([sleep(backoff[Math.min(attempt, backoff.length - 1)]), this.#stop.wait()]);
    }
  }

  // Returns null if the state ended or the dial timed out. A late transport is closed.
  async #dial() {
    const { client } = this;
    const dialing = Promise.resolve().then(() => client._dial(client.sessionId));
    const stopped = this.#stop.wait().then(() => TIMED_OUT);
    const result = await withTimeout(Promise.race([dialing, stopped]), client._options.connectTimeoutMs);
    if (result === TIMED_OUT) {
      dialing.then((transport) => transport.close(), () => {});
      return null;
    }
    return result;
  }
}

class ConnectingState extends DialingState {
  id = ClientState.CONNECTING;
}

class ReconnectingState extends DialingState {
  id = ClientState.RECONNECTING;
}

class ConnectedState extends State {
  id = ClientState.CONNECTED;

  onEnter() {
    this.client._ready.set();
    this.client._settleFirstOutcome();
  }

  onExit() {
    this.client._ready.clear();
  }

  onLost() {
    this.client._enter(ReconnectingState);
  }

  async openStream(target, params) {
    return this.client._host.open(target, params);
  }
}

class ClosedState extends State {
  id = ClientState.CLOSED;

  onEnter() {
    this.client._ready.set(); // waiters wake, and find this state
    this.client._settleFirstOutcome();
  }

  async openStream() {
    throw this.client.closeError ?? new SessionClosed();
  }
}

/** Create one with connect(). */
export class Client {
  closeError = null;
  _ready = new Signal();
  _lastDialError = null;
  #listeners = [];
  #closingLocally = false;
  #lastConnection = null;
  #settleOutcome;

  constructor(mux, dial, sessionId, handlers, capabilities, options) {
    this.sessionId = sessionId;
    this._dial = dial;
    this._options = options;
    this._firstOutcome = new Promise((resolve) => {
      this.#settleOutcome = resolve;
    });
    this._host = new SessionHost(mux, this, {
      role: Role.CLIENT,
      sessionId,
      capabilities,
      handlers: handlerTable(handlers),
      inflightLimit: options.inflightLimit,
      session: options.session,
    });
    this._state = new State(this);
  }

  get state() {
    return this._state.id;
  }

  get transport() {
    return this._host.activeTransport;
  }

  get rttMs() {
    return this._host.rttMs;
  }

  /** Opens a stream, or waits for a connection first. It does not wait for OPEN_OK. */
  openStream(target, params = new Uint8Array(0), { signal } = {}) {
    const deadline = performance.now() + this._options.connectTimeoutMs;
    return this._state.openStream(target, params, deadline, signal);
  }

  streams() {
    return this._host.listStreams();
  }

  /** Returns a function that removes the listener. */
  onStateChange(listener) {
    this.#listeners.push(listener);
    return () => {
      this.#listeners = this.#listeners.filter((item) => item !== listener);
    };
  }

  async close(code = Reason.OK) {
    this.#closingLocally = true;
    this._host.close(code);
    await this.waitClosed();
  }

  async waitClosed() {
    await this._host.closed.wait();
    await this.#lastConnection?.closed.wait();
  }

  async [Symbol.asyncDispose]() {
    await this.close();
  }

  // --- From the host -----------------------------------------------------------------------

  onConnected() {
    this._state.onConnected();
  }

  onLost() {
    this._state.onLost();
  }

  onClosed(code, connection) {
    this.#lastConnection = connection;
    if (!this.#closingLocally && this.closeError === null) {
      this.closeError = code === Reason.SESSION_NOT_FOUND ? new SessionNotFound() : new SessionClosed(code);
    }
    this._enter(ClosedState);
  }

  // --- Internals -------------------------------------------------------------------------

  _enter(StateClass) {
    this._state.onExit();
    this._state = new StateClass(this);
    this._state.onEnter();
    for (const listener of [...this.#listeners]) {
      try {
        listener(this._state.id);
      } catch (error) {
        console.error('packetmux: state listener failed', error);
      }
    }
  }

  async _waitReady(deadline, signal) {
    const left = Math.max(0, deadline - performance.now());
    if (await withTimeout(raceAbort(this._ready.wait(), signal), left) === TIMED_OUT) {
      throw new NoConnection();
    }
  }

  _fail(error) {
    this.closeError = error;
    this._host.close(Reason.OK);
  }

  _settleFirstOutcome() {
    this.#settleOutcome();
  }
}

function newSessionId() {
  // Not randomUUID: it exists only in secure contexts, and a renderer may load over plain http.
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
}

/** Resolves once the handshake has finished. Reconnecting after that is automatic. */
export async function connect(dial, { handlers, capabilities = new Uint8Array(0), signal, ...overrides } = {}) {
  const options = { ...DEFAULTS, ...overrides };
  const mux = await loadPacketMux();
  const client = new Client(mux, dial, newSessionId(), handlers, capabilities, options);
  client._enter(ConnectingState);
  let outcome;
  try {
    outcome = await withTimeout(raceAbort(client._firstOutcome, signal), options.connectTimeoutMs);
  } catch (error) {
    client._fail(error instanceof PacketMuxError ? error : new NoConnection('connect was aborted', { cause: error }));
    throw error;
  }
  if (outcome === TIMED_OUT) {
    const reason = client._lastDialError;
    client._fail(new NoConnection(reason?.message ?? 'no connection', { cause: reason ?? undefined }));
  }
  if (client.closeError !== null) { // refused: GOAWAY came right behind HELLO
    throw client.closeError;
  }
  return client;
}
