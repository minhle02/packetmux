// Runs one core session on Promises: every host rule in one place (FR-SDK-4).
//
// SessionHost is one per session and owns the core. Connection is one per Transport, and runs
// the reader and sender loops. Every core call goes through #call, so no path can skip the drain.
import { EventType, Reason, SessionError, Status } from './core.js';
import {
  GiveUp, InvalidParams, LimitExceeded, NoConnection, Reject, SessionClosed, TransportClosed,
} from './errors.js';
import { decodeParams, encodeParams, isStreamType, typeOf } from './params.js';
import { Signal, withTimeout } from './signal.js';
import { IncomingStream, Stream } from './stream.js';

// Whole milliseconds: a fraction throws at the ABI boundary.
export const nowMs = () => Math.floor(performance.now());

export function handlerTable(handlers = []) {
  const table = new Map();
  for (const [target, handler] of handlers) {
    if (isStreamType(target)) {
      table.set(target.kind, { handler, type: target });
    } else {
      table.set(target, { handler, type: null });
    }
  }
  return table;
}

/** One Transport, plus the loops that move messages between it and the host. */
export class Connection {
  settled = new Signal(); // the handshake finished, or the connection ended
  closed = new Signal();
  #host;
  #finished = false;
  #work = new Signal();
  #sender = null;

  constructor(host, transport) {
    this.#host = host;
    this.transport = transport;
  }

  start() {
    void this.#reader();
    this.#sender = this.#sendLoop();
  }

  wake() {
    this.#work.set();
  }

  /** The session is done with this connection. A GOAWAY may still be in the sender's hands. */
  finish() {
    if (this.#finished) {
      return;
    }
    this.#finished = true;
    this.settled.set();
    this.#work.set();
    void this.#shutdown();
  }

  async #reader() {
    try {
      while (!this.#finished) {
        this.#host.feed(this, await this.transport.recv());
      }
    } catch (error) {
      this.#report(error, 'reader');
    }
  }

  async #sendLoop() {
    const limit = this.#host.inflightLimit;
    try {
      while (!this.#finished) {
        await this.transport.waitBufferBelow(limit);
        if (this.#finished) {
          break; // replaced while waiting: what is queued now is the new connection's
        }
        const message = this.#host.pollTransmit();
        if (message === null) {
          if (!this.#finished) {
            this.#work.clear();
            await this.#work.wait();
          }
          continue;
        }
        await this.transport.send(message); // a GOAWAY just taken still goes out
      }
    } catch (error) {
      this.#report(error, 'sender');
    }
  }

  #report(error, loop) {
    if (!(error instanceof TransportClosed) && !this.#finished) {
      console.error(`packetmux: ${loop} failed`, error);
    }
    this.#host.transportLost(this);
  }

  async #shutdown() {
    const drain = this.#host.drainTimeoutMs;
    await withTimeout(this.#sender, drain);
    await withTimeout(this.transport.waitBufferBelow(1).catch(() => {}), drain);
    try {
      await this.transport.close(); // recv and any wait reject, which ends both loops
    } catch (error) {
      console.error('packetmux: transport close failed', error);
    } finally {
      this.closed.set();
    }
  }
}

/** Owns one core session. The owner is the Client or ServerSession around it. */
export class SessionHost {
  connection = null;
  connected = false;
  closed = new Signal();
  #core;
  #handlers;
  #streams = new Map();
  #replacing = false;
  #coreEnded = false;
  #timer = null;
  #timerAt = 0;

  constructor(mux, owner, { role, sessionId, capabilities, handlers, inflightLimit, maxReadBuffer, session }) {
    this.owner = owner;
    this.inflightLimit = inflightLimit;
    this.maxReadBuffer = maxReadBuffer;
    const settings = { ...mux.configDefaults(), ...session };
    this.maxWriteCall = settings.maxQueuedBytesPerStream;
    this.drainTimeoutMs = settings.drainTimeoutMs;
    this.largestFrame = 5 + Math.max(settings.maxDataPayload, settings.maxControlPayload);
    this.#handlers = handlers;
    this.#core = new mux.Session({ role, sessionId, capabilities, ...session });
  }

  get rttMs() {
    return this.#core === null ? null : this.#core.rttMs;
  }

  // --- The one path into the core ----------------------------------------------------------

  #afterCall() {
    while (this.#core !== null) {
      const event = this.#core.pollEvent();
      if (event === null) {
        break;
      }
      this.#onEvent(event);
    }
    if (this.#coreEnded) {
      this.#releaseCore();
      return;
    }
    this.#armTimer();
    this.connection?.wake();
  }

  #call(method, ...args) {
    const result = this.#core[method](...args);
    this.#afterCall();
    return result;
  }

  // WASM memory is not garbage-collected.
  #releaseCore() {
    clearTimeout(this.#timer);
    this.#timer = null;
    this.#core?.destroy();
    this.#core = null;
  }

  #armTimer() {
    const deadline = this.#core.nextDeadlineMs();
    if (deadline === null) {
      clearTimeout(this.#timer);
      this.#timer = null;
    } else if (this.#timer === null || deadline < this.#timerAt) {
      // A later deadline can wait: a tick before it is harmless and re-arms for the rest.
      clearTimeout(this.#timer);
      this.#timerAt = deadline;
      this.#timer = setTimeout(() => this.#onTimer(), Math.max(0, deadline - nowMs()));
    }
  }

  #onTimer() {
    this.#timer = null;
    if (this.#core !== null) {
      this.#call('onTick', nowMs());
    }
  }

  // --- Connections -------------------------------------------------------------------------

  checkMessageSize(transport) {
    if (transport.maxMessageSize < this.largestFrame) {
      throw new GiveUp(`transport carries ${transport.maxMessageSize} bytes, `
        + `and frames can reach ${this.largestFrame}`);
    }
  }

  attach(transport, firstMessage = null) {
    if (this.#core === null) {
      throw new SessionClosed();
    }
    if (this.connection !== null) { // the client re-dialed before the old one died
      this.#replacing = true;
      this.#call('onConnectionLost');
      this.#replacing = false;
      if (this.#core === null) { // it was closing, and that was its connection
        throw new SessionClosed();
      }
    }
    this.#core.attachConnection(nowMs());
    const connection = new Connection(this, transport);
    this.connection = connection;
    if (firstMessage !== null) { // a server has already read the HELLO
      this.#core.onMessage(firstMessage, nowMs());
    }
    connection.start();
    this.#afterCall();
    return connection;
  }

  feed(connection, message) {
    if (connection === this.connection) { // a replaced connection may report late
      this.#call('onMessage', message, nowMs());
    }
  }

  transportLost(connection) {
    if (connection === this.connection) {
      this.#call('onConnectionLost');
    }
  }

  pollTransmit() {
    return this.#core === null ? null : this.#call('pollTransmit');
  }

  close(code) {
    if (this.#core === null) {
      return;
    }
    try {
      this.#call('close', code, nowMs());
    } catch (error) {
      if (!(error instanceof SessionError) || error.status !== Status.CLOSED) { // already closing
        throw error;
      }
    }
  }

  // --- Events. Synchronous: they only buffer and wake ---------------------------------------

  #onEvent(event) {
    const stream = this.#streams.get(event.streamId);
    switch (event.type) {
      case EventType.DATA:
        stream?.onData(event.data);
        break;
      case EventType.EOF:
        stream?.onEof();
        break;
      case EventType.WRITABLE:
        stream?.onWritable();
        break;
      case EventType.STREAM_ACCEPTED:
        stream?.onAccepted();
        break;
      case EventType.STREAM_CLOSED:
        this.#streams.delete(event.streamId);
        stream?.onClosed(event.code);
        break;
      case EventType.STREAM_OPENED:
        this.#onStreamOpened(event);
        break;
      case EventType.CONNECTED:
        this.connected = true;
        this.connection?.settled.set();
        this.owner.onConnected();
        break;
      case EventType.DISCONNECTED: {
        this.connected = false;
        const connection = this.connection;
        this.connection = null;
        connection?.finish();
        this.owner.onLost(this.#replacing);
        break;
      }
      case EventType.SESSION_CLOSED: {
        this.connected = false;
        const connection = this.connection;
        this.connection = null;
        connection?.finish();
        this.#coreEnded = true;
        this.closed.set();
        this.owner.onClosed(event.code, connection);
        break;
      }
      default:
        break;
    }
  }

  #onStreamOpened(event) {
    const entry = this.#handlers.get(event.kind);
    if (entry === undefined) {
      this.#core.rejectStream(event.streamId, Reason.UNSUPPORTED_KIND);
      return;
    }
    let { params } = event;
    if (entry.type !== null) {
      try {
        params = decodeParams(entry.type, params);
      } catch (error) {
        if (!(error instanceof InvalidParams)) {
          throw error;
        }
        const message = new TextEncoder().encode(error.message);
        this.#core.rejectStream(event.streamId, Reason.PROTOCOL_ERROR, message);
        return;
      }
    }
    const stream = new Stream(this, event.streamId, event.kind, false);
    this.#streams.set(event.streamId, stream);
    void this.#runHandler(entry.handler, new IncomingStream(this, stream, event.kind, params));
  }

  async #runHandler(handler, request) {
    try {
      await handler(request);
      await request.onHandlerReturned();
    } catch (error) {
      if (error instanceof Reject) {
        request.onHandlerRejected(error.code, error.rejectMessage);
      } else if (!this.closed.isSet) { // a session that ended fails its handlers' streams
        console.error(`packetmux: handler for ${request.kind} failed`, error);
        request.onHandlerFailed();
      }
    }
  }

  // --- Streams -----------------------------------------------------------------------------

  open(target, params) {
    let kind = target;
    let bytes = params;
    const type = typeOf(target);
    if (type !== undefined) {
      kind = type.kind;
      bytes = encodeParams(target);
    }
    if (this.#core === null) {
      throw new SessionClosed();
    }
    let streamId;
    try {
      streamId = this.#core.openStream(kind, bytes, nowMs());
    } catch (error) {
      throw openError(error);
    }
    const stream = new Stream(this, streamId, kind, false);
    this.#streams.set(streamId, stream);
    this.#afterCall();
    return stream;
  }

  write(streamId, data) {
    return this.#call('write', streamId, data, nowMs());
  }

  sendEof(streamId) {
    this.#call('sendEof', streamId);
  }

  reset(streamId, code) {
    this.#call('resetStream', streamId, code);
  }

  resetInDrain(streamId, code) {
    this.#core.resetStream(streamId, code); // the drain in progress picks up its events
  }

  accept(stream) {
    if (!stream.isClosed) {
      this.#call('acceptStream', stream.id);
      stream.onAccepted();
    }
  }

  reject(stream, code, message) {
    if (!stream.isClosed) {
      this.#call('rejectStream', stream.id, code, message);
    }
  }

  listStreams() {
    const infos = this.#core === null ? [] : this.#core.listStreams();
    return infos.map((info) => ({
      ...info,
      readHighWater: this.#streams.get(info.streamId)?.readHighWater ?? 0,
    }));
  }

  streamInfo(stream) {
    return this.listStreams().find((info) => info.streamId === stream.id) ?? null;
  }
}

function openError(error) {
  if (!(error instanceof SessionError)) {
    return error;
  }
  if (error.status === Status.NOT_CONNECTED) {
    return new NoConnection();
  }
  if (error.status === Status.CLOSED) {
    return new SessionClosed();
  }
  if (error.status === Status.LIMIT_EXCEEDED || error.status === Status.IDS_EXHAUSTED) {
    return new LimitExceeded('stream limit reached');
  }
  return new InvalidParams('invalid stream kind');
}
