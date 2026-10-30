// The Node side of the Python <-> JS test (bindings/python/packetmux/tests/test_interop.py).
// Connects over a real WebSocket, sends a request, checks the reply, and closes the
// session. Prints what it saw as JSON.
import { createHash } from 'node:crypto';

import { EventType, Reason, Role, SessionState, loadPacketMux } from '../index.js';

const [url, requestSize, replySize] = process.argv.slice(2);

function patternOf(size, seed) {
  const bytes = new Uint8Array(size);
  for (let index = 0; index < size; ++index) {
    bytes[index] = (index * seed + 7) % 251;
  }
  return bytes;
}

const sha256 = (chunks) => {
  const hash = createHash('sha256');
  chunks.forEach((chunk) => hash.update(chunk));
  return hash.digest('hex');
};

const { Session } = await loadPacketMux();
const session = new Session({ role: Role.CLIENT, sessionId: 'interop-1' });
const request = patternOf(Number(requestSize), 31);
const received = [];
const result = { events: [] };
let streamId = 0;
let offset = 0;
let eofSent = false;

const socket = new WebSocket(url);
socket.binaryType = 'arraybuffer';
setTimeout(() => {
  console.error('timed out');
  process.exit(2);
}, 30000).unref();

function pushRequest() {
  while (streamId !== 0 && offset < request.length) {
    const accepted = session.write(streamId, request.subarray(offset), Date.now());
    if (accepted === 0) {
      return;
    }
    offset += accepted;
  }
  if (streamId !== 0 && !eofSent) {
    session.sendEof(streamId);
    eofSent = true;
  }
}

function onEvent(event) {
  result.events.push(Object.keys(EventType)[event.type]);
  switch (event.type) {
    case EventType.CONNECTED:
      result.peerSessionId = event.peerSessionId;
      break;
    case EventType.DATA:
      received.push(event.data);
      break;
    case EventType.STREAM_CLOSED:
      result.streamCloseCode = event.code;
      session.close(Reason.OK, Date.now());
      break;
    case EventType.SESSION_CLOSED:
      result.sessionCloseCode = event.code;
      result.state = Object.keys(SessionState)[session.state];
      result.replySha256 = sha256(received);
      result.requestSha256 = sha256([request]);
      result.replyBytes = received.reduce((total, chunk) => total + chunk.length, 0);
      console.log(JSON.stringify(result));
      socket.close();
      process.exit(0);
      break;
    default:
      break;
  }
}

// The host loop from Session.h: take events and frames until neither has any.
function pump() {
  let progress = true;
  while (progress) {
    progress = false;
    pushRequest();
    for (let event = session.pollEvent(); event !== null; event = session.pollEvent()) {
      onEvent(event);
      progress = true;
    }
    for (let message = session.pollTransmit(); message !== null; message = session.pollTransmit()) {
      socket.send(message);
      progress = true;
    }
  }
}

socket.onopen = () => {
  session.attachConnection(Date.now());
  streamId = session.openStream('interop', new TextEncoder().encode(String(replySize)), Date.now());
  pump();
};

socket.onmessage = (message) => {
  session.onMessage(new Uint8Array(message.data), Date.now());
  pump();
};

socket.onerror = () => {
  console.error('socket error');
  process.exit(3);
};
