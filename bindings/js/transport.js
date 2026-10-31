// The one thing a consumer implements: a connection that carries whole messages.
//
// A Transport has maxMessageSize, bufferedAmount, send(message), recv(), waitBufferBelow(limit)
// and close(). See index.d.ts. After it closes, for any reason, send, recv and waitBufferBelow
// reject with TransportClosed. A message that arrives before recv is called must not be lost.
import { TransportClosed } from './errors.js';

const BUFFER_POLL_MS = 2;

/**
 * For sources that push: WebSocket, data channel. It keeps the queue behind recv, so a message
 * that arrives early waits there. Subclasses implement sendMessage, closeSource and
 * bufferedAmount, and call the handle methods. A WebSocket has no event for a drained buffer, so
 * waiting polls bufferedAmount. A source that has one can also call handleBufferLow.
 */
export class PushTransport {
  maxMessageSize = 1 << 20;
  #bufferPoll = null;
  #inbox = [];
  #receivers = [];
  #bufferWaiters = [];
  #closed = false;
  #closeError = null;

  /** Closed from this side: reading fails at once. Closed by the peer: the queue is read first. */
  async send(message) {
    this.#throwIfClosed();
    if (message.length > this.maxMessageSize) {
      throw new RangeError(`message of ${message.length} bytes is over the limit`);
    }
    await this.sendMessage(message);
  }

  recv() {
    if (this.#inbox.length > 0) {
      return Promise.resolve(this.#inbox.shift());
    }
    if (this.#closed) {
      return Promise.reject(this.#closedError());
    }
    return new Promise((resolve, reject) => this.#receivers.push({ resolve, reject }));
  }

  waitBufferBelow(limit) {
    if (this.#closed) {
      return Promise.reject(this.#closedError());
    }
    if (this.bufferedAmount < limit) {
      return Promise.resolve();
    }
    return new Promise((resolve, reject) => {
      this.#bufferWaiters.push({ limit, resolve, reject });
      this.#bufferPoll ??= setInterval(() => this.handleBufferLow(), BUFFER_POLL_MS);
    });
  }

  async close() {
    if (!this.#closed) {
      this.#inbox = [];
      this.#shut();
      this.closeSource();
    }
  }

  /** Call with each whole message from the source. */
  handleMessage(message) {
    const receiver = this.#receivers.shift();
    if (receiver === undefined) {
      this.#inbox.push(message);
    } else {
      receiver.resolve(message);
    }
  }

  /** Call when the source closes. Give the error for an abnormal close. */
  handleClose(error) {
    if (!this.#closed) {
      this.#closeError = error ?? null;
      this.#shut();
    }
  }

  /** Call when the source's send buffer drains. */
  handleBufferLow() {
    const waiters = this.#bufferWaiters;
    this.#bufferWaiters = [];
    for (const waiter of waiters) {
      if (this.bufferedAmount < waiter.limit) {
        waiter.resolve();
      } else {
        this.#bufferWaiters.push(waiter);
      }
    }
    if (this.#bufferWaiters.length === 0) {
      clearInterval(this.#bufferPoll);
      this.#bufferPoll = null;
    }
  }

  #closedError() {
    return new TransportClosed('transport closed', { cause: this.#closeError ?? undefined });
  }

  #throwIfClosed() {
    if (this.#closed) {
      throw this.#closedError();
    }
  }

  #shut() {
    this.#closed = true;
    for (const waiter of [...this.#receivers, ...this.#bufferWaiters]) {
      waiter.reject(this.#closedError());
    }
    this.#receivers = [];
    this.#bufferWaiters = [];
    clearInterval(this.#bufferPoll);
    this.#bufferPoll = null;
  }
}
