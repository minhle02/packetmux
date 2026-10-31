// The server: hand it each accepted Transport, and it finds or creates the session.
import { CodecError, FrameType, Reason, Role, loadPacketMux } from './core.js';
import { Refuse, SessionClosed, TransportClosed } from './errors.js';
import { SessionHost, handlerTable } from './host.js';
import { TIMED_OUT, withTimeout } from './signal.js';

const DEFAULTS = {
  gracePeriodMs: 120_000,
  maxSessions: 1024,
  inflightLimit: 64 * 1024,
  maxReadBuffer: 16 * 1024 * 1024,
  session: {},
};

export class ServerSession {
  #server;
  #grace = null;

  constructor(server, mux, sessionId, data) {
    const { options, capabilities, handlers } = server._config;
    this.sessionId = sessionId;
    this.data = data; // what authorize returned
    this.#server = server;
    this._host = new SessionHost(mux, this, {
      role: Role.SERVER,
      sessionId,
      capabilities,
      handlers,
      inflightLimit: options.inflightLimit,
      maxReadBuffer: options.maxReadBuffer,
      session: options.session,
    });
  }

  get connected() {
    return this._host.connected;
  }

  get rttMs() {
    return this._host.rttMs;
  }

  /** Rejects with NoConnection while disconnected: the client may never come back. */
  async openStream(target, params = new Uint8Array(0)) {
    return this._host.open(target, params);
  }

  streams() {
    return this._host.listStreams();
  }

  async close(code = Reason.OK) {
    this._host.close(code);
    await this._host.closed.wait();
  }

  // --- From the host -----------------------------------------------------------------------

  onConnected() {}

  onLost(replaced) {
    if (!replaced) { // a replacing connection is already here
      this._cancelGrace();
      this.#grace = setTimeout(() => this.#expire(), this.#server._config.options.gracePeriodMs);
    }
  }

  onClosed() {
    this._cancelGrace();
    this.#server._sessions.delete(this.sessionId);
  }

  // --- Internals -------------------------------------------------------------------------

  _cancelGrace() {
    clearTimeout(this.#grace);
    this.#grace = null;
  }

  #expire() {
    this.#grace = null;
    void this.close(Reason.TIMEOUT);
  }
}

export class Server {
  _sessions = new Map();
  #authorize;

  constructor({ handlers, authorize, capabilities = new Uint8Array(0), ...overrides } = {}) {
    this.#authorize = authorize;
    this._config = {
      handlers: handlerTable(handlers),
      capabilities,
      options: { ...DEFAULTS, ...overrides },
    };
  }

  sessions() {
    return [...this._sessions.values()];
  }

  /** Runs one connection. Resolves when it ends. */
  async serve(transport, context = undefined) {
    const mux = await loadPacketMux();
    const settings = { ...mux.configDefaults(), ...this._config.options.session };
    const first = await this.#firstMessage(transport, mux, settings);
    if (first === null) {
      return;
    }
    const { message, hello } = first;

    let data;
    try {
      data = await this.#authorize?.({
        sessionId: hello.sessionId,
        capabilities: hello.capabilities,
        resuming: hello.resuming,
        context,
        session: this._sessions.get(hello.sessionId) ?? null,
      });
    } catch (error) {
      if (error instanceof Refuse) {
        await this.#refuse(transport, hello, error.code, mux, settings);
        return;
      }
      await transport.close();
      throw error;
    }

    let session = this._sessions.get(hello.sessionId); // authorize may have waited
    if (session === undefined) {
      if (hello.resuming) {
        await this.#refuse(transport, hello, Reason.SESSION_NOT_FOUND, mux, settings);
        return;
      }
      if (this._sessions.size >= this._config.options.maxSessions) {
        await this.#refuse(transport, hello, Reason.LIMIT_EXCEEDED, mux, settings);
        return;
      }
      session = new ServerSession(this, mux, hello.sessionId, data);
      this._sessions.set(hello.sessionId, session);
    }

    try {
      session._host.checkMessageSize(transport);
    } catch (error) {
      await transport.close();
      throw error;
    }
    session._cancelGrace();
    let connection;
    try {
      connection = session._host.attach(transport, message);
    } catch (error) {
      if (!(error instanceof SessionClosed)) {
        throw error;
      }
      await this.#refuse(transport, hello, Reason.SESSION_NOT_FOUND, mux, settings); // it just ended
      return;
    }
    await connection.closed.wait();
  }

  async close(code = Reason.OK) {
    await Promise.all(this.sessions().map((session) => session.close(code)));
  }

  // Null, with the transport closed, unless the first message is a valid HELLO in time.
  async #firstMessage(transport, mux, settings) {
    try {
      const message = await withTimeout(transport.recv(), settings.helloTimeoutMs);
      if (message !== TIMED_OUT) {
        const hello = mux.decodeFrame(message);
        if (hello.type === FrameType.HELLO) {
          return { message, hello };
        }
      }
    } catch (error) {
      if (!(error instanceof TransportClosed) && !(error instanceof CodecError)) {
        throw error;
      }
    }
    await transport.close(); // no session has been touched
    return null;
  }

  // HELLO first: anything else before it is a protocol error.
  async #refuse(transport, hello, code, mux, settings) {
    try {
      await transport.send(mux.encodeFrame({
        type: FrameType.HELLO,
        sessionId: hello.sessionId,
        capabilities: this._config.capabilities,
      }));
      await transport.send(mux.encodeFrame({ type: FrameType.GOAWAY, code }));
      await withTimeout(transport.waitBufferBelow(1), settings.drainTimeoutMs);
    } catch (error) {
      if (!(error instanceof TransportClosed)) {
        throw error;
      }
    }
    await transport.close();
  }
}
