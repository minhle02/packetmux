// A PacketMux server over WebSocket, for Node.
//
//     node server.js [port]
//
// Then run client.js. Needs the ws package.
import { PushTransport, Reason, Refuse, Server, defineStream } from 'packetmux';
import { WebSocketServer } from 'ws';

const TOKENS = new Map([['token-1', 'alice']]); // your real check goes here

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

async function echo(request) {
  const stream = request.accept();
  for await (const chunk of stream) {
    await stream.write(chunk);
  }
}

async function greet(request) {
  const { name, shout } = request.params;
  const greeting = `hello, ${name}`;
  const text = shout ? greeting.toUpperCase() : greeting;
  await request.accept().write(new TextEncoder().encode(text));
}

// Runs before any existing session is touched. What it returns becomes session.data.
async function authorize(request) {
  const user = TOKENS.get(request.context);
  if (user === undefined) {
    throw new Refuse(Reason.AUTH_REVOKED);
  }
  if (request.session !== null && request.session.data !== user) {
    throw new Refuse(Reason.AUTH_REVOKED); // someone else's session
  }
  return user;
}

const server = new Server({ handlers: [['echo', echo], [Greet, greet]], authorize });

const port = Number(process.argv[2] ?? 8765);
const sockets = new WebSocketServer({ host: '127.0.0.1', port });
sockets.on('connection', (socket, request) => {
  const token = new URL(request.url, 'http://localhost').searchParams.get('token');
  void server.serve(new WebSocketTransport(socket), token);
});
sockets.on('listening', () => console.log(`listening on port ${sockets.address().port}`));
