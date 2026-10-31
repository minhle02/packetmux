// Field offsets of the C structs on wasm32. LayoutCheck.cpp repeats these numbers,
// so changing a struct without this file fails the WASM build.

export const CONFIG = {
  size: 88, role: 0, sessionId: 4, capabilities: 12, maxDataPayload: 20, maxControlPayload: 24,
  maxStreams: 28, maxQueuedBytes: 32, maxQueuedBytesPerStream: 36, maxQueuedControlFrames: 40,
  helloTimeoutMs: 48, pingIntervalMs: 56, deadAfterMs: 64, drainTimeoutMs: 72, maxOutstandingPings: 80,
};

export const LIMITS = { size: 8, maxDataPayload: 0, maxControlPayload: 4 };

export const EVENT = {
  size: 52, type: 0, streamId: 4, code: 8, codecStatus: 9, peerSessionId: 12, capabilities: 20,
  kind: 28, params: 36, data: 44,
};

export const STREAM_INFO = {
  size: 48, streamId: 0, kind: 4, opener: 12, state: 13, bytesSent: 16, bytesReceived: 24,
  createdAtMs: 32, lastActivityMs: 40,
};

export const FRAME = {
  size: 72, type: 0, streamId: 4, version: 8, resuming: 10, sessionId: 12, capabilities: 20, kind: 28, params: 36,
  code: 44, message: 48, data: 56, nonce: 64,
};
