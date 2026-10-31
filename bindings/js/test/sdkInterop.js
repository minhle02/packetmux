// The Node side of the SDK interop test (bindings/python/packetmux/tests/test_sdk_interop.py).
//     node sdkInterop.js client <url>    runs the script against a server, prints JSON
//     node sdkInterop.js server          runs a server, prints its port
import { createHash } from 'node:crypto';

import { WebSocketServer } from 'ws';

import { Reason, Server, connect, defineStream } from '../index.js';
import { WsTransport } from './wsTransport.js';

const Forward = defineStream('forward', { host: 'string', port: 'int' });
const decoder = new TextDecoder();

const pattern = (size) => Uint8Array.from({ length: size }, (_, i) => (i * 31 + 7) % 251);
const sha256 = (chunks) => {
  const hash = createHash('sha256');
  chunks.forEach((chunk) => hash.update(chunk));
  return hash.digest('hex');
};

async function exchange(client, size) {
  const stream = await client.openStream('echo');
  const data = pattern(size);
  const reading = (async () => {
    const chunks = [];
    for await (const chunk of stream) {
      chunks.push(chunk);
    }
    return chunks;
  })();
  await stream.write(data);
  await stream.sendEof();
  return { sent: sha256([data]), received: sha256(await reading) };
}

async function runClient(url) {
  let transport = null;
  const dial = async (sessionId) => {
    transport = await WsTransport.open(`${url}?session=${sessionId}`);
    return transport;
  };
  const client = await connect(dial, { reconnectBackoffMs: [10] });
  const result = { sessionId: client.sessionId };

  result.first = await exchange(client, 3 << 20);

  const typed = await client.openStream(Forward({ host: 'h', port: 7 }));
  result.typed = decoder.decode(await typed.read());

  const lost = await client.openStream('echo');
  await transport.close(); // the connection dies
  result.lostCode = await lost.waitClosed();
  while (client.state !== 'connected') {
    await new Promise((resolve) => { setTimeout(resolve, 5); });
  }
  result.afterReconnect = await exchange(client, 1 << 20);

  await client.close(Reason.AUTH_REVOKED);
  result.state = client.state;
  console.log(JSON.stringify(result));
}

async function echo(request) {
  const stream = request.accept();
  for await (const chunk of stream) {
    await stream.write(chunk);
  }
}

async function forward(request) {
  const { host, port } = request.params;
  await request.accept().write(new TextEncoder().encode(`${host}:${port}`));
}

function runServer() {
  const server = new Server({ handlers: [['echo', echo], [Forward, forward]] });
  const sockets = new WebSocketServer({ host: '127.0.0.1', port: 0, maxPayload: 1 << 20 });
  sockets.on('connection', (socket) => {
    void server.serve(new WsTransport(socket));
  });
  sockets.on('listening', () => console.log(sockets.address().port));
}

const [mode, url] = process.argv.slice(2);
if (mode === 'client') {
  await runClient(url);
} else {
  runServer();
}
