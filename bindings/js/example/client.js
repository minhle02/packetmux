// A PacketMux client over WebSocket. It uses the global WebSocket, so it runs in Node or a browser.
//
//     node client.js [port]
//
// Start server.js first.
import { PushTransport, connect, defineStream } from 'packetmux';

class WebSocketTransport extends PushTransport {
  #socket;

  constructor(socket) {
    super();
    this.#socket = socket;
    socket.binaryType = 'arraybuffer';
    socket.onmessage = (event) => this.handleMessage(new Uint8Array(event.data));
    socket.onclose = () => this.handleClose();
    socket.onerror = () => {};
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

const Greet = defineStream('greet', { name: 'string', shout: { type: 'bool', default: false } });

const port = Number(process.argv[2] ?? 8765);
const decoder = new TextDecoder();

// Called to connect, and again after every lost connection.
function dial() {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(`ws://127.0.0.1:${port}?token=token-1`);
    socket.onopen = () => resolve(new WebSocketTransport(socket));
    socket.onerror = () => reject(new Error('cannot connect'));
  });
}

const client = await connect(dial, { connectTimeoutMs: 5000 });
client.onStateChange((state) => console.log(`state: ${state}`));
console.log(`session ${client.sessionId}`);

let stream = await client.openStream('echo');
await stream.write(new TextEncoder().encode('hello'));
await stream.sendEof(); // we are done writing, so the server's loop ends
for await (const chunk of stream) {
  console.log(`echo: ${decoder.decode(chunk)}`);
}

stream = await client.openStream(Greet({ name: 'alice', shout: true }));
console.log(`greet: ${decoder.decode(await stream.read())}`);
await stream.sendEof();

await client.close();
