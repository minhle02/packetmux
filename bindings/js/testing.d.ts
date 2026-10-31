import { PushTransport } from './index.js';
import type { Server, Transport } from './index.js';

/** One side of a memory link. Tests can stall it, release it and break it. */
export class MemoryTransport extends PushTransport {
  constructor(maxMessageSize?: number);
  peer: MemoryTransport | null;
  readonly bufferedAmount: number;
  protected sendMessage(message: Uint8Array): void;
  protected closeSource(): void;
  /** The link is full: the sender waits, and nothing is delivered. */
  stall(): void;
  release(): void;
  /** The link dies, on both sides. */
  breakConnection(): Promise<void>;
  /** This side sees the link die. The peer still thinks it is alive. */
  failLocally(): void;
}

export function memoryTransportPair(maxMessageSize?: number): [MemoryTransport, MemoryTransport];

/** Connects clients to a Server over memory transports. network.dial is a dial function. */
export class MemoryNetwork {
  constructor(server: Server, context?: unknown);
  readonly links: [MemoryTransport, MemoryTransport][];
  readonly dials: number;
  context: unknown;
  dial(sessionId: string): Promise<MemoryTransport>;
  failNext(count: number): void;
  giveUp(): void;
  /** Dials wait until this is called with false. */
  hang(value?: boolean): void;
}

export const TRANSPORT_CHECKS: Record<string, (a: Transport, b: Transport) => Promise<void>>;
export function runTransportCheck(
  name: string,
  factory: () => [Transport, Transport] | Promise<[Transport, Transport]>,
): Promise<void>;
/** Runs every check on your Transport. factory returns a connected pair of it. */
export function checkTransport(
  factory: () => [Transport, Transport] | Promise<[Transport, Transport]>,
): Promise<void>;
