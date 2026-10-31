// PacketMux: many byte streams over one connection, and a session that survives reconnects.
//
//     import { connect } from 'packetmux'
//
//     const client = await connect(dial)
//     const stream = await client.openStream('echo')
//     await stream.write(new TextEncoder().encode('hello'))
//
// You implement a Transport, and a dial function that opens one. The sync binding is packetmux/core.
export { Client, ClientState, connect } from './client.js';
export { Reason, loadPacketMux } from './core.js';
export {
  GiveUp, InvalidParams, LimitExceeded, NoConnection, PacketMuxError, Refuse, Reject, SessionClosed,
  SessionNotFound, StreamClosed, StreamReset, TransportClosed,
} from './errors.js';
export { defineStream } from './params.js';
export { Server, ServerSession } from './server.js';
export { IncomingStream, Stream } from './stream.js';
export { PushTransport } from './transport.js';
