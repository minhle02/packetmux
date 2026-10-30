export type EnumOf<T extends string> = Readonly<Record<T, number>>;

export const Status: EnumOf<'OK' | 'CLOSED' | 'NOT_CONNECTED' | 'ALREADY_CONNECTED' | 'EVENTS_PENDING' |
  'UNKNOWN_STREAM' | 'INVALID_STATE' | 'LIMIT_EXCEEDED' | 'IDS_EXHAUSTED' | 'INVALID_ARGUMENT'>;
export const CodecStatus: EnumOf<'OK' | 'TRUNCATED' | 'UNKNOWN_TYPE' | 'BAD_STREAM_ID' | 'PAYLOAD_TOO_LARGE' |
  'BAD_PAYLOAD_SIZE' | 'UNSUPPORTED_VERSION' | 'BAD_IDENTIFIER' | 'INVALID_ARGUMENT'>;
export const Role: EnumOf<'CLIENT' | 'SERVER'>;
export const Reason: EnumOf<'OK' | 'PEER_CLOSED' | 'CONNECT_FAILED' | 'POLICY_DENIED' | 'LIMIT_EXCEEDED' |
  'TRANSPORT_LOST' | 'KILLED' | 'AUTH_REVOKED' | 'TIMEOUT' | 'PROTOCOL_ERROR' | 'UNSUPPORTED_KIND'>;
export const FrameType: EnumOf<'UNKNOWN' | 'HELLO' | 'OPEN' | 'OPEN_OK' | 'OPEN_ERR' | 'DATA' | 'EOF' | 'RST' |
  'PING' | 'PONG' | 'GOAWAY'>;
export const EventType: EnumOf<'UNKNOWN' | 'CONNECTED' | 'STREAM_OPENED' | 'STREAM_ACCEPTED' | 'DATA' | 'EOF' |
  'WRITABLE' | 'STREAM_CLOSED' | 'DISCONNECTED' | 'SESSION_CLOSED'>;
export const SessionState: EnumOf<'DISCONNECTED' | 'HANDSHAKING' | 'ESTABLISHED' | 'DRAINING' | 'CLOSING' | 'CLOSED'>;
export const StreamState: EnumOf<'SENDER_OPENING' | 'SENDER_OPENING_EOF_SENT' | 'RECEIVER_OPENING' |
  'RECEIVER_OPENING_EOF_RECEIVED' | 'OPEN' | 'HALF_CLOSED_LOCAL' | 'HALF_CLOSED_REMOTE' | 'CLOSED'>;
export const NO_DEADLINE: bigint;

export class SessionError extends Error {
  readonly status: number;
}

export class CodecError extends Error {
  readonly status: number;
}

export interface SessionOptions {
  role?: number;
  sessionId: string;
  capabilities?: Uint8Array;
  maxDataPayload?: number;
  maxControlPayload?: number;
  maxStreams?: number;
  maxQueuedBytes?: number;
  maxQueuedBytesPerStream?: number;
  maxQueuedControlFrames?: number;
  maxOutstandingPings?: number;
  helloTimeoutMs?: number;
  pingIntervalMs?: number;
  deadAfterMs?: number;
  drainTimeoutMs?: number;
}

/** One event. It owns its bytes, so it stays valid after the next call. */
export interface SessionEvent {
  type: number;
  streamId: number;
  code: number;
  codecStatus: number;
  peerSessionId: string;
  capabilities: Uint8Array;
  kind: string;
  params: Uint8Array;
  data: Uint8Array;
}

export interface StreamInfo {
  streamId: number;
  kind: string;
  opener: number;
  state: number;
  bytesSent: number;
  bytesReceived: number;
  createdAtMs: number;
  lastActivityMs: number;
}

export interface Frame {
  type: number;
  streamId: number;
  version: number;
  sessionId: string;
  capabilities: Uint8Array;
  kind: string;
  params: Uint8Array;
  code: number;
  message: Uint8Array;
  data: Uint8Array;
  nonce: bigint;
}

export interface FrameLimits {
  maxDataPayload?: number;
  maxControlPayload?: number;
}

/** See include/packetmux/Session.h. A status other than OK throws SessionError. */
export interface Session {
  /** Frees the WASM memory, which the garbage collector cannot see. */
  destroy(): void;
  attachConnection(nowMs: number): void;
  onMessage(message: Uint8Array, nowMs: number): void;
  onConnectionLost(): void;
  onTick(nowMs: number): void;
  nextDeadlineMs(): number | null;
  close(code: number, nowMs: number): void;
  openStream(kind: string, params: Uint8Array | null, nowMs: number): number;
  acceptStream(streamId: number): void;
  rejectStream(streamId: number, code: number, message?: Uint8Array): void;
  /** Returns how many bytes were accepted, which may be 0. */
  write(streamId: number, data: Uint8Array, nowMs: number): number;
  sendEof(streamId: number): void;
  resetStream(streamId: number, code?: number): void;
  pollTransmit(): Uint8Array | null;
  pollEvent(): SessionEvent | null;
  readonly state: number;
  readonly rttMs: number | null;
  listStreams(): StreamInfo[];
}

export interface PacketMux {
  Session: new (options: SessionOptions) => Session;
  decodeFrame(message: Uint8Array, limits?: FrameLimits): Frame;
  encodeFrame(frame: Partial<Frame>, limits?: FrameLimits): Uint8Array;
}

/** Loads the WASM module once. Everything that needs it comes from here. */
export function loadPacketMux(): Promise<PacketMux>;
