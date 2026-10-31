// The Transport checks over a real WebSocket, with the transport a consumer would write.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { WebSocketServer } from 'ws';

import { TRANSPORT_CHECKS, checkTransport, runTransportCheck } from '../testing.js';
import { WsTransport } from './wsTransport.js';

async function socketPair() {
  const sockets = new WebSocketServer({ host: '127.0.0.1', port: 0, maxPayload: 1 << 21 });
  await new Promise((resolve) => { sockets.on('listening', resolve); });
  const accepted = new Promise((resolve) => {
    sockets.on('connection', (socket) => resolve(new WsTransport(socket)));
  });
  const client = await WsTransport.open(`ws://127.0.0.1:${sockets.address().port}`);
  const server = await accepted;
  sockets.close();
  return [client, server];
}

for (const name of Object.keys(TRANSPORT_CHECKS)) {
  test(`websocket: ${name}`, () => runTransportCheck(name, socketPair));
}

test('checkTransport is what a consumer calls', async () => {
  await checkTransport(socketPair);
  assert.ok(true);
});
