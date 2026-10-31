// A Transport over a real WebSocket, for the tests. It is also the shape a consumer writes.
import { PushTransport } from '../index.js';

export class WsTransport extends PushTransport {
  #socket;

  constructor(socket) {
    super();
    this.#socket = socket;
    socket.binaryType = 'arraybuffer';
    socket.onmessage = (event) => this.handleMessage(new Uint8Array(event.data));
    socket.onclose = () => this.handleClose();
    socket.onerror = () => {};
  }

  /** Resolves with a transport once the socket is open. */
  static open(url) {
    return new Promise((resolve, reject) => {
      const socket = new WebSocket(url);
      socket.onopen = () => resolve(new WsTransport(socket));
      socket.onerror = () => reject(new Error(`cannot reach ${url}`));
    });
  }

  get bufferedAmount() {
    return this.#socket.bufferedAmount;
  }

  sendMessage(message) {
    this.#socket.send(message);
  }

  closeSource() {
    this.#socket.close();
  }
}
