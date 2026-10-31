// The small async tools the SDK uses in place of asyncio's.

/** An event that tasks wait on. Like asyncio.Event. */
export class Signal {
  #set = false;
  #waiters = [];

  get isSet() {
    return this.#set;
  }

  set() {
    this.#set = true;
    const waiters = this.#waiters;
    this.#waiters = [];
    waiters.forEach((resolve) => resolve());
  }

  clear() {
    this.#set = false;
  }

  /** Rejects with the signal's reason if abort fires first. */
  wait(abort) {
    abort?.throwIfAborted();
    if (this.#set) {
      return Promise.resolve();
    }
    return raceAbort(new Promise((resolve) => this.#waiters.push(resolve)), abort);
  }
}

export function raceAbort(promise, abort) {
  if (abort === undefined) {
    return promise;
  }
  return new Promise((resolve, reject) => {
    const onAbort = () => reject(abort.reason);
    abort.addEventListener('abort', onAbort, { once: true });
    promise.then(resolve, reject).finally(() => abort.removeEventListener('abort', onAbort));
  });
}

export const TIMED_OUT = Symbol('timed out');

/** Resolves with the promise's result, or with TIMED_OUT after ms. */
export function withTimeout(promise, ms) {
  let timer;
  const timeout = new Promise((resolve) => {
    timer = setTimeout(resolve, ms, TIMED_OUT);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

export const sleep = (ms) => new Promise((resolve) => {
  setTimeout(resolve, ms);
});
