import type { StreamInfo as CoreStreamInfo } from './core.js';

export { loadPacketMux } from './core.js';
export const Reason: typeof import('./core.js').Reason;

export type ClientStateName = 'connecting' | 'connected' | 'reconnecting' | 'closed';
export const ClientState: Readonly<{
  CONNECTING: 'connecting';
  CONNECTED: 'connected';
  RECONNECTING: 'reconnecting';
  CLOSED: 'closed';
}>;

// --- Errors --------------------------------------------------------------------------------------

export class PacketMuxError extends Error {}
/** A Transport has closed. Only code that implements a Transport sees this. */
export class TransportClosed extends PacketMuxError {
  constructor(message?: string, options?: { cause?: unknown });
}
export class StreamReset extends PacketMuxError {
  readonly code: number;
}
/** Writing after sendEof, or after a clean close. */
export class StreamClosed extends PacketMuxError {}
/** No connection within connectTimeoutMs, or a server session is disconnected. */
export class NoConnection extends PacketMuxError {}
export class SessionClosed extends PacketMuxError {
  readonly code: number;
}
/** The server no longer has this session. Connect again for a new one. */
export class SessionNotFound extends SessionClosed {}
/** maxStreams is reached, or stream IDs are exhausted. */
export class LimitExceeded extends PacketMuxError {}
/** Typed params do not match their declared fields. */
export class InvalidParams extends PacketMuxError {}
/** Throw from a dial function to stop reconnecting. The client closes with this error. */
export class GiveUp extends PacketMuxError {}
/** Throw from a handler to reject the stream. */
export class Reject extends PacketMuxError {
  constructor(code: number, message?: Uint8Array);
  readonly code: number;
}
/** Throw from authorize to refuse the connection. */
export class Refuse extends PacketMuxError {
  constructor(code?: number);
  readonly code: number;
}

// --- Transport -----------------------------------------------------------------------------------

/**
 * One connection delivering whole messages, reliably and in order. After it closes, for any reason,
 * send, recv and waitBufferBelow reject with TransportClosed. A message that arrives before recv is
 * called must not be lost.
 */
export interface Transport {
  readonly maxMessageSize: number;
  /** Bytes accepted by send and not yet on the wire. */
  readonly bufferedAmount: number;
  send(message: Uint8Array): Promise<void>;
  recv(): Promise<Uint8Array>;
  waitBufferBelow(limit: number): Promise<void>;
  /** Idempotent. */
  close(): Promise<void>;
}

/** For sources that push: WebSocket, data channel. It keeps the queue behind recv. */
export abstract class PushTransport implements Transport {
  maxMessageSize: number;
  abstract readonly bufferedAmount: number;
  protected abstract sendMessage(message: Uint8Array): void | Promise<void>;
  protected abstract closeSource(): void;
  send(message: Uint8Array): Promise<void>;
  recv(): Promise<Uint8Array>;
  waitBufferBelow(limit: number): Promise<void>;
  close(): Promise<void>;
  /** Call with each whole message from the source. */
  handleMessage(message: Uint8Array): void;
  /** Call when the source closes. Give the error for an abnormal close. */
  handleClose(error?: Error): void;
  /** Call when the source's send buffer drains. */
  handleBufferLow(): void;
}

// --- Typed params --------------------------------------------------------------------------------

type FieldName = 'string' | 'int' | 'float' | 'bool';
type FieldSpec = FieldName | `${FieldName}?` | { type: FieldName | `${FieldName}?`; default: unknown };
type FieldValue<T> = T extends 'string' ? string
  : T extends 'int' | 'float' ? number
  : T extends 'bool' ? boolean
  : T extends `${infer Base}?` ? FieldValue<Base> | null
  : T extends { type: infer Inner } ? FieldValue<Inner>
  : never;
type Optional<F> = {
  [K in keyof F]: F[K] extends { default: unknown } ? K : F[K] extends `${string}?` ? K : never
}[keyof F];
export type ParamsOf<F> = { [K in keyof F]: FieldValue<F[K]> };
type ParamsInput<F> = Omit<ParamsOf<F>, Optional<F>> & Partial<Pick<ParamsOf<F>, Optional<F>>>;

export interface TypedParams<F> {
  readonly value: ParamsInput<F>;
}

export interface StreamType<F> {
  (value: ParamsInput<F>): TypedParams<F>;
  readonly kind: string;
}

/** Declares a stream type. TypeScript types do not exist at runtime, so fields are values. */
export function defineStream<const F extends Record<string, FieldSpec>>(kind: string, fields: F): StreamType<F>;

// --- Streams -------------------------------------------------------------------------------------

export interface StreamInfo extends CoreStreamInfo {
  readHighWater: number;
}

export class Stream implements AsyncIterable<Uint8Array> {
  readonly id: number;
  readonly kind: string;
  readonly session: Client | ServerSession;
  readonly isClosed: boolean;
  readonly resetCode: number | null;
  readonly readHighWater: number;
  /** The next chunk. null means the peer half-closed and everything has been read. */
  read(options?: { signal?: AbortSignal }): Promise<Uint8Array | null>;
  /** Resolves once every byte is queued. Aborting leaves an unknown amount sent. */
  write(data: Uint8Array, options?: { signal?: AbortSignal }): Promise<void>;
  sendEof(): Promise<void>;
  reset(code?: number): void;
  waitAccepted(options?: { signal?: AbortSignal }): Promise<void>;
  /** Resolves with the reason. Reason.OK means a clean close. */
  waitClosed(): Promise<number>;
  /** Details from the core, or null once the stream has closed. */
  info(): StreamInfo | null;
  [Symbol.asyncIterator](): AsyncGenerator<Uint8Array>;
}

/** A stream the peer opened. The handler accepts it or rejects it. */
export class IncomingStream<P = Uint8Array> {
  readonly kind: string;
  readonly params: P;
  readonly session: Client | ServerSession;
  accept(): Stream;
  reject(code: number, message?: Uint8Array): void;
}

export type Handler<P = Uint8Array> = (request: IncomingStream<P>) => Promise<void>;
export type HandlerEntry =
  | readonly [string, Handler]
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  | readonly [StreamType<any>, Handler<any>];

// --- Client --------------------------------------------------------------------------------------

export interface ClientOptions {
  connectTimeoutMs?: number;
  /** The last value repeats. */
  reconnectBackoffMs?: readonly number[];
  inflightLimit?: number;
  maxReadBuffer?: number;
  /** Core settings: maxStreams, pingIntervalMs, ... */
  session?: Record<string, number>;
}

export interface ConnectOptions extends ClientOptions {
  /** For streams the server opens. */
  handlers?: readonly HandlerEntry[];
  capabilities?: Uint8Array;
  signal?: AbortSignal;
}

export class Client implements AsyncDisposable {
  /** Generated by the SDK. */
  readonly sessionId: string;
  readonly state: ClientStateName;
  readonly transport: Transport | null;
  readonly rttMs: number | null;
  /** Why it closed, if not by close(). */
  readonly closeError: PacketMuxError | null;
  /** Opens a stream, or waits for a connection first. It does not wait for OPEN_OK. */
  openStream(
    target: string | TypedParams<any>, // eslint-disable-line @typescript-eslint/no-explicit-any
    params?: Uint8Array,
    options?: { signal?: AbortSignal },
  ): Promise<Stream>;
  streams(): StreamInfo[];
  /** Returns a function that removes the listener. */
  onStateChange(listener: (state: ClientStateName) => void): () => void;
  close(code?: number): Promise<void>;
  waitClosed(): Promise<void>;
  [Symbol.asyncDispose](): Promise<void>;
}

/** Resolves once the handshake has finished. Reconnecting after that is automatic. */
export function connect(
  dial: (sessionId: string) => Promise<Transport>,
  options?: ConnectOptions,
): Promise<Client>;

// --- Server --------------------------------------------------------------------------------------

export interface ConnectRequest {
  sessionId: string;
  capabilities: Uint8Array;
  resuming: boolean;
  /** What the consumer passed to serve. */
  context: unknown;
  /** The existing session, or null. */
  session: ServerSession | null;
}

export interface ServerOptions {
  /** For streams the client opens. */
  handlers?: readonly HandlerEntry[];
  /** Returns data for session.data, or throws Refuse. */
  authorize?: (request: ConnectRequest) => Promise<unknown>;
  /** Runs once for each session that ends, after it has left sessions(). */
  onSessionClosed?: (session: ServerSession, code: number) => void;
  capabilities?: Uint8Array;
  gracePeriodMs?: number;
  maxSessions?: number;
  inflightLimit?: number;
  maxReadBuffer?: number;
  session?: Record<string, number>;
}

export class ServerSession {
  readonly sessionId: string;
  readonly connected: boolean;
  /** What authorize returned. */
  readonly data: unknown;
  /** The active Transport, or null. */
  readonly transport: Transport | null;
  readonly rttMs: number | null;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  openStream(target: string | TypedParams<any>, params?: Uint8Array): Promise<Stream>;
  streams(): StreamInfo[];
  close(code?: number): Promise<void>;
}

export class Server {
  constructor(options?: ServerOptions);
  /** Runs one connection. Resolves when it ends. */
  serve(transport: Transport, context?: unknown): Promise<void>;
  sessions(): ServerSession[];
  close(code?: number): Promise<void>;
}
