// Enum values of the C ABI. test/constants.test.js checks them against the header.

const enumOf = (names) => Object.freeze(Object.fromEntries(names.map((name, value) => [name, value])));

export const Status = enumOf(['OK', 'CLOSED', 'NOT_CONNECTED', 'ALREADY_CONNECTED', 'EVENTS_PENDING',
  'UNKNOWN_STREAM', 'INVALID_STATE', 'LIMIT_EXCEEDED', 'IDS_EXHAUSTED', 'INVALID_ARGUMENT']);

export const CodecStatus = enumOf(['OK', 'TRUNCATED', 'UNKNOWN_TYPE', 'BAD_STREAM_ID', 'PAYLOAD_TOO_LARGE',
  'BAD_PAYLOAD_SIZE', 'UNSUPPORTED_VERSION', 'BAD_IDENTIFIER', 'INVALID_ARGUMENT']);

export const Role = enumOf(['CLIENT', 'SERVER']);

export const Reason = enumOf(['OK', 'PEER_CLOSED', 'CONNECT_FAILED', 'POLICY_DENIED', 'LIMIT_EXCEEDED',
  'TRANSPORT_LOST', 'KILLED', 'AUTH_REVOKED', 'TIMEOUT', 'PROTOCOL_ERROR', 'UNSUPPORTED_KIND']);

export const FrameType = enumOf(['UNKNOWN', 'HELLO', 'OPEN', 'OPEN_OK', 'OPEN_ERR', 'DATA', 'EOF', 'RST',
  'PING', 'PONG', 'GOAWAY']);

export const EventType = enumOf(['UNKNOWN', 'CONNECTED', 'STREAM_OPENED', 'STREAM_ACCEPTED', 'DATA', 'EOF',
  'WRITABLE', 'STREAM_CLOSED', 'DISCONNECTED', 'SESSION_CLOSED']);

export const SessionState = enumOf(['DISCONNECTED', 'HANDSHAKING', 'ESTABLISHED', 'DRAINING', 'CLOSING',
  'CLOSED']);

export const StreamState = enumOf(['SENDER_OPENING', 'SENDER_OPENING_EOF_SENT', 'RECEIVER_OPENING',
  'RECEIVER_OPENING_EOF_RECEIVED', 'OPEN', 'HALF_CLOSED_LOCAL', 'HALF_CLOSED_REMOTE', 'CLOSED']);

export const NO_DEADLINE = 0xFFFFFFFFFFFFFFFFn;
