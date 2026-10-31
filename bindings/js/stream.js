// An async byte stream, and the request a handler gets for an inbound one.
import { Reason } from './constants.js';
import { StreamClosed, StreamReset } from './errors.js';
import { Signal } from './signal.js';

export class Stream {
  resetCode = null;
  readHighWater = 0;
  #host;
  #closed = false;
  #closeCode = Reason.OK;
  #remoteEof = false;
  #eofSent = false;
  #readBuffer = [];
  #readBytes = 0;
  #readable = new Signal();
  #writable = new Signal();
  #accepted = new Signal();
  #finished = new Signal();
  #writeTail = Promise.resolve(); // FIFO: concurrent writes do not interleave

  constructor(host, id, kind, accepted) {
    this.#host = host;
    this.id = id;
    this.kind = kind;
    if (accepted) {
      this.#accepted.set();
    }
  }

  /** The Client or ServerSession this stream belongs to. */
  get session() {
    return this.#host.owner;
  }

  get isClosed() {
    return this.#closed;
  }

  // --- Consumer API ----------------------------------------------------------------------

  /** The next chunk. null means the peer half-closed and everything has been read. */
  async read({ signal } = {}) {
    for (;;) {
      if (this.resetCode !== null) { // a reset wins over buffered data
        throw new StreamReset(this.resetCode);
      }
      if (this.#readBuffer.length > 0) {
        return this.#take();
      }
      if (this.#remoteEof || this.#closed) {
        return null;
      }
      this.#readable.clear();
      await this.#readable.wait(signal);
    }
  }

  /** Resolves once every byte is queued. Aborting leaves an unknown amount sent. */
  write(data, { signal } = {}) {
    return this.#inOrder(async () => {
      this.#checkWritable();
      let rest = data;
      while (rest.length > 0) {
        this.#writable.clear(); // before the call, so its WRITABLE is not lost
        const piece = rest.subarray(0, this.#host.maxWriteCall);
        const accepted = this.#host.write(this.id, piece);
        rest = rest.subarray(accepted);
        if (accepted < piece.length) {
          await this.#writable.wait(signal);
          this.#checkWritable();
        }
      }
    });
  }

  sendEof() {
    return this.#inOrder(async () => { // EOF goes after any write still in progress
      if (this.resetCode !== null) {
        throw new StreamReset(this.resetCode);
      }
      if (!this.#eofSent && !this.#closed) {
        this.#eofSent = true;
        this.#host.sendEof(this.id);
      }
    });
  }

  reset(code = Reason.KILLED) {
    if (!this.#closed) {
      this.#host.reset(this.id, code);
    }
  }

  async waitAccepted({ signal } = {}) {
    await this.#accepted.wait(signal);
    if (this.resetCode !== null) {
      throw new StreamReset(this.resetCode);
    }
  }

  /** Resolves with the reason. Reason.OK means a clean close. */
  async waitClosed() {
    await this.#finished.wait();
    return this.#closeCode;
  }

  /** Details from the core, or null once the stream has closed. */
  info() {
    return this.#host.streamInfo(this);
  }

  async* [Symbol.asyncIterator]() {
    for (let chunk = await this.read(); chunk !== null; chunk = await this.read()) {
      yield chunk;
    }
  }

  // --- From the host. These run inside the drain, so they cannot wait ---------------------

  onData(chunk) {
    if (this.#closed) {
      return;
    }
    this.#readBuffer.push(chunk);
    this.#readBytes += chunk.length;
    this.readHighWater = Math.max(this.readHighWater, this.#readBytes);
    if (this.#readBytes > this.#host.maxReadBuffer) {
      this.#host.resetInDrain(this.id, Reason.LIMIT_EXCEEDED); // STREAM_CLOSED follows
      return;
    }
    this.#readable.set();
  }

  onEof() {
    this.#remoteEof = true;
    this.#readable.set();
  }

  onAccepted() {
    this.#accepted.set();
  }

  onWritable() {
    this.#writable.set();
  }

  onClosed(code) {
    this.#closed = true;
    this.#closeCode = code;
    if (code !== Reason.OK) {
      this.resetCode = code;
    }
    this.#readable.set();
    this.#writable.set();
    this.#accepted.set();
    this.#finished.set();
  }

  // --- Internals -------------------------------------------------------------------------

  // The one path bytes leave by, which is where credit will be returned (FR-AST-7).
  #take() {
    const chunk = this.#readBuffer.shift();
    this.#readBytes -= chunk.length;
    return chunk;
  }

  #checkWritable() {
    if (this.resetCode !== null) {
      throw new StreamReset(this.resetCode);
    }
    if (this.#closed || this.#eofSent) {
      throw new StreamClosed(`stream ${this.id} is closed for writing`);
    }
  }

  #inOrder(task) {
    const run = this.#writeTail.then(task);
    this.#writeTail = run.catch(() => {});
    return run;
  }
}

/** A stream the peer opened. The handler accepts it or rejects it. */
export class IncomingStream {
  #stream;
  #host;
  #settled = false;

  constructor(host, stream, kind, params) {
    this.#host = host;
    this.#stream = stream;
    this.kind = kind;
    this.params = params;
  }

  get session() {
    return this.#stream.session;
  }

  accept() {
    if (!this.#settled) {
      this.#settled = true;
      this.#host.accept(this.#stream);
    }
    return this.#stream;
  }

  reject(code, message = new Uint8Array(0)) {
    if (!this.#settled) {
      this.#settled = true;
      this.#host.reject(this.#stream, code, message);
    }
  }

  // --- What happens when the handler finishes ---------------------------------------------

  onHandlerRejected(code, message) {
    if (this.#settled) {
      this.#stream.reset(code);
    } else {
      this.reject(code, message);
    }
  }

  onHandlerFailed() {
    this.onHandlerRejected(this.#settled ? Reason.KILLED : Reason.CONNECT_FAILED);
  }

  async onHandlerReturned() {
    if (!this.#settled) {
      this.reject(Reason.POLICY_DENIED);
      return;
    }
    try { // the stream stays until the peer's EOF, so it can still read the reply
      await this.#stream.sendEof();
    } catch (error) {
      if (!(error instanceof StreamReset)) {
        throw error;
      }
    }
  }
}
