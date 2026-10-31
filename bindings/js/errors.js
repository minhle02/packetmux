// Every error the SDK throws, and the ones a consumer throws to steer it.
import { Reason } from './constants.js';

const reasonName = (code) => Object.keys(Reason).find((name) => Reason[name] === code) ?? String(code);

export class PacketMuxError extends Error {
  constructor(message, options) {
    super(message, options);
    this.name = new.target.name;
  }
}

// A Transport has closed. Only code that implements a Transport sees this.
export class TransportClosed extends PacketMuxError {
  constructor(message = 'transport closed', options) {
    super(message, options);
  }
}

export class StreamReset extends PacketMuxError {
  constructor(code) {
    super(`stream reset: ${reasonName(code)}`);
    this.code = code;
  }
}

// Writing after sendEof, or after a clean close.
export class StreamClosed extends PacketMuxError {}

// No connection within connectTimeoutMs, or a server session is disconnected.
export class NoConnection extends PacketMuxError {
  constructor(message = 'no connection', options) {
    super(message, options);
  }
}

export class SessionClosed extends PacketMuxError {
  constructor(code = Reason.OK) {
    super(`session closed: ${reasonName(code)}`);
    this.code = code;
  }
}

// The server no longer has this session. Connect again for a new one.
export class SessionNotFound extends SessionClosed {
  constructor() {
    super(Reason.SESSION_NOT_FOUND);
  }
}

// maxStreams is reached, or stream IDs are exhausted.
export class LimitExceeded extends PacketMuxError {}

// Typed params do not match their declared fields.
export class InvalidParams extends PacketMuxError {}

// Throw from a dial function to stop reconnecting. The client closes with this error.
export class GiveUp extends PacketMuxError {}

// Throw from a handler to reject the stream.
export class Reject extends PacketMuxError {
  constructor(code, message = new Uint8Array(0)) {
    super(`rejected: ${reasonName(code)}`);
    this.code = code;
    this.rejectMessage = message;
  }
}

// Throw from authorize to refuse the connection.
export class Refuse extends PacketMuxError {
  constructor(code = Reason.POLICY_DENIED) {
    super(`refused: ${reasonName(code)}`);
    this.code = code;
  }
}
