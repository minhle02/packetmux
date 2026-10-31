// JS binding for PacketMux, a thin layer over the C ABI compiled to WASM.
//
// The session is sans-I/O and synchronous: the host owns the transport, feeds in
// messages and the time, and polls frames and events out. Everything returned
// here owns its bytes. See include/packetmux/Session.h for the host loop.

import createModule from './dist/packetmux.mjs';
import { CodecStatus, FrameType, NO_DEADLINE, Reason, Role, Status } from './constants.js';
import { CONFIG, EVENT, FRAME, LIMITS, STREAM_INFO } from './layout.js';

export * from './constants.js';

const SCRATCH_SIZE = 16;
const DEFAULT_RECEIVE_WINDOW = 256 * 1024;
const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

export class SessionError extends Error {
  constructor(status) {
    super(Object.keys(Status)[status] ?? `status ${status}`);
    this.name = 'SessionError';
    this.status = status;
  }
}

export class CodecError extends Error {
  constructor(status) {
    super(Object.keys(CodecStatus)[status] ?? `status ${status}`);
    this.name = 'CodecError';
    this.status = status;
  }
}

function check(status) {
  if (status !== Status.OK) {
    throw new SessionError(status);
  }
}

// Reading and writing WASM memory. Views are made per call, because growing the
// memory replaces its buffer.
class Heap {
  constructor(wasm) {
    this.wasm = wasm;
  }

  view() {
    return new DataView(this.wasm.HEAPU8.buffer);
  }

  // Copies bytes in. The caller frees the result. An empty buffer is pointer 0.
  copyIn(bytes) {
    if (bytes.length === 0) {
      return 0;
    }
    const pointer = this.wasm._malloc(bytes.length);
    this.wasm.HEAPU8.set(bytes, pointer);
    return pointer;
  }

  free(pointer) {
    if (pointer !== 0) {
      this.wasm._free(pointer);
    }
  }

  // Reads a packetmux_bytes_t into a copy that owns its bytes.
  readBytes(address) {
    const view = this.view();
    const data = view.getUint32(address, true);
    const size = view.getUint32(address + 4, true);
    return this.wasm.HEAPU8.slice(data, data + size);
  }

  readText(address) {
    return textDecoder.decode(this.readBytes(address));
  }

  writeBytes(address, pointer, size) {
    const view = this.view();
    view.setUint32(address, pointer, true);
    view.setUint32(address + 4, size, true);
  }

  // Copies each buffer in for the length of the call.
  withBuffers(buffers, call) {
    const pointers = buffers.map((buffer) => this.copyIn(buffer));
    try {
      return call(...pointers);
    } finally {
      pointers.forEach((pointer) => this.free(pointer));
    }
  }

  // A temporary struct, zeroed.
  withStruct(size, call) {
    const pointer = this.wasm._malloc(size);
    this.wasm.HEAPU8.fill(0, pointer, pointer + size);
    try {
      return call(pointer);
    } finally {
      this.wasm._free(pointer);
    }
  }

  writeLimits(address, limits) {
    const view = this.view();
    const merged = { ...this.defaults, ...limits };
    view.setUint32(address + LIMITS.maxDataPayload, merged.maxDataPayload, true);
    view.setUint32(address + LIMITS.maxControlPayload, merged.maxControlPayload, true);
  }
}

function makeSessionClass(wasm, heap) {
  // size_t is 4 bytes on wasm32, so the size fields are written like uint32.
  function writeConfig(address, options, sessionId, capabilities) {
    wasm._packetmux_config_init(address);
    const view = heap.view();
    view.setUint8(address + CONFIG.role, options.role ?? Role.CLIENT);
    heap.writeBytes(address + CONFIG.sessionId, sessionId.pointer, sessionId.size);
    heap.writeBytes(address + CONFIG.capabilities, capabilities.pointer, capabilities.size);
    for (const name of ['maxDataPayload', 'maxControlPayload', 'maxStreams', 'maxQueuedBytes',
      'maxQueuedBytesPerStream', 'maxQueuedControlFrames', 'maxOutstandingPings', 'receiveWindow']) {
      if (options[name] !== undefined) {
        view.setUint32(address + CONFIG[name], options[name], true);
      }
    }
    for (const name of ['helloTimeoutMs', 'pingIntervalMs', 'deadAfterMs', 'drainTimeoutMs']) {
      if (options[name] !== undefined) {
        view.setBigUint64(address + CONFIG[name], BigInt(options[name]), true);
      }
    }
  }

  return class Session {
    #handle;
    #message = 0; // events borrow this until they are polled
    #event;
    #scratch;

    constructor(options) {
      const sessionId = textEncoder.encode(options.sessionId);
      const capabilities = options.capabilities ?? new Uint8Array(0);
      this.#handle = heap.withBuffers([sessionId, capabilities], (sessionIdAt, capabilitiesAt) =>
        heap.withStruct(CONFIG.size, (config) => {
          writeConfig(config, options, { pointer: sessionIdAt, size: sessionId.length },
            { pointer: capabilitiesAt, size: capabilities.length });
          return wasm._packetmux_session_create(config);
        }));
      this.#event = wasm._malloc(EVENT.size);
      this.#scratch = wasm._malloc(Math.max(SCRATCH_SIZE, STREAM_INFO.size));
    }

    // Frees the WASM memory, which the garbage collector cannot see.
    destroy() {
      if (this.#handle === 0) {
        return;
      }
      wasm._packetmux_session_destroy(this.#handle);
      heap.free(this.#message);
      wasm._free(this.#event);
      wasm._free(this.#scratch);
      this.#handle = 0;
      this.#message = 0;
    }

    attachConnection(nowMs) {
      check(wasm._packetmux_session_attach_connection(this.#handle, BigInt(nowMs)));
    }

    onMessage(message, nowMs) {
      const pointer = heap.copyIn(message);
      const status = wasm._packetmux_session_on_message(this.#handle, pointer, message.length, BigInt(nowMs));
      if (status !== Status.OK) {
        heap.free(pointer); // refused: the previous message may still be borrowed
        throw new SessionError(status);
      }
      heap.free(this.#message);
      this.#message = pointer;
    }

    onConnectionLost() {
      check(wasm._packetmux_session_on_connection_lost(this.#handle));
    }

    onTick(nowMs) {
      check(wasm._packetmux_session_on_tick(this.#handle, BigInt(nowMs)));
    }

    nextDeadlineMs() {
      const deadline = wasm._packetmux_session_next_deadline_ms(this.#handle);
      return BigInt.asUintN(64, deadline) === NO_DEADLINE ? null : Number(deadline);
    }

    close(code, nowMs) {
      check(wasm._packetmux_session_close(this.#handle, code, BigInt(nowMs)));
    }

    openStream(kind, params, nowMs) {
      const kindBytes = textEncoder.encode(kind);
      const paramsBytes = params ?? new Uint8Array(0);
      return heap.withBuffers([kindBytes, paramsBytes], (kindAt, paramsAt) => {
        check(wasm._packetmux_session_open_stream(this.#handle, kindAt, kindBytes.length, paramsAt,
          paramsBytes.length, BigInt(nowMs), this.#scratch));
        return heap.view().getUint32(this.#scratch, true);
      });
    }

    acceptStream(streamId) {
      check(wasm._packetmux_session_accept_stream(this.#handle, streamId));
    }

    rejectStream(streamId, code, message = new Uint8Array(0)) {
      heap.withBuffers([message], (messageAt) =>
        check(wasm._packetmux_session_reject_stream(this.#handle, streamId, code, messageAt, message.length)));
    }

    // Queues data. Returns how many bytes were accepted, which may be 0.
    write(streamId, data, nowMs) {
      return heap.withBuffers([data], (dataAt) => {
        check(wasm._packetmux_session_write(this.#handle, streamId, dataAt, data.length, BigInt(nowMs),
          this.#scratch));
        return heap.view().getUint32(this.#scratch, true);
      });
    }

    /** The consumer has taken this many DATA bytes. Without this the stream stalls. */
    consume(streamId, count) {
      check(wasm._packetmux_session_consume(this.#handle, streamId, BigInt(count)));
    }

    sendEof(streamId) {
      check(wasm._packetmux_session_send_eof(this.#handle, streamId));
    }

    resetStream(streamId, code = Reason.KILLED) {
      check(wasm._packetmux_session_reset_stream(this.#handle, streamId, code));
    }

    pollTransmit() {
      if (!wasm._packetmux_session_poll_transmit(this.#handle, this.#scratch)) {
        return null;
      }
      return heap.readBytes(this.#scratch);
    }

    pollEvent() {
      const at = this.#event;
      if (!wasm._packetmux_session_poll_event(this.#handle, at)) {
        return null;
      }
      const view = heap.view();
      return {
        type: view.getUint8(at + EVENT.type),
        streamId: view.getUint32(at + EVENT.streamId, true),
        code: view.getUint8(at + EVENT.code),
        codecStatus: view.getUint8(at + EVENT.codecStatus),
        peerSessionId: heap.readText(at + EVENT.peerSessionId),
        capabilities: heap.readBytes(at + EVENT.capabilities),
        kind: heap.readText(at + EVENT.kind),
        params: heap.readBytes(at + EVENT.params),
        data: heap.readBytes(at + EVENT.data),
      };
    }

    get state() {
      return wasm._packetmux_session_state(this.#handle);
    }

    get rttMs() {
      if (!wasm._packetmux_session_get_rtt_ms(this.#handle, this.#scratch)) {
        return null;
      }
      return Number(heap.view().getBigUint64(this.#scratch, true));
    }

    listStreams() {
      const count = wasm._packetmux_session_list_streams(this.#handle);
      const streams = [];
      for (let index = 0; index < count; ++index) {
        const at = this.#scratch;
        wasm._packetmux_session_stream_at(this.#handle, index, at);
        const view = heap.view();
        streams.push({
          streamId: view.getUint32(at + STREAM_INFO.streamId, true),
          kind: heap.readText(at + STREAM_INFO.kind),
          opener: view.getUint8(at + STREAM_INFO.opener),
          state: view.getUint8(at + STREAM_INFO.state),
          bytesSent: Number(view.getBigUint64(at + STREAM_INFO.bytesSent, true)),
          bytesReceived: Number(view.getBigUint64(at + STREAM_INFO.bytesReceived, true)),
          createdAtMs: Number(view.getBigUint64(at + STREAM_INFO.createdAtMs, true)),
          lastActivityMs: Number(view.getBigUint64(at + STREAM_INFO.lastActivityMs, true)),
          sendCredit: Number(view.getBigUint64(at + STREAM_INFO.sendCredit, true)),
        });
      }
      return streams;
    }
  };
}

function makeCodec(wasm, heap) {
  function decodeFrame(message, limits = {}) {
    return heap.withBuffers([message], (messageAt) => heap.withStruct(LIMITS.size, (limitsAt) =>
      heap.withStruct(FRAME.size, (at) => {
        heap.writeLimits(limitsAt, limits);
        const status = wasm._packetmux_frame_decode(messageAt, message.length, limitsAt, at);
        if (status !== CodecStatus.OK) {
          throw new CodecError(status);
        }
        const view = heap.view();
        return {
          type: view.getUint8(at + FRAME.type),
          streamId: view.getUint32(at + FRAME.streamId, true),
          version: view.getUint16(at + FRAME.version, true),
          resuming: view.getUint8(at + FRAME.resuming) !== 0,
          sessionId: heap.readText(at + FRAME.sessionId),
          capabilities: heap.readBytes(at + FRAME.capabilities),
          kind: heap.readText(at + FRAME.kind),
          params: heap.readBytes(at + FRAME.params),
          code: view.getUint8(at + FRAME.code),
          message: heap.readBytes(at + FRAME.message),
          data: heap.readBytes(at + FRAME.data),
          nonce: view.getBigUint64(at + FRAME.nonce, true),
          receiveWindow: view.getUint32(at + FRAME.receiveWindow, true),
          credit: view.getUint32(at + FRAME.credit, true),
        };
      })));
  }

  function encodeFrame(frame, limits = {}) {
    const empty = new Uint8Array(0);
    const sessionId = textEncoder.encode(frame.sessionId ?? '');
    const kind = textEncoder.encode(frame.kind ?? '');
    const buffers = [sessionId, frame.capabilities ?? empty, kind, frame.params ?? empty,
      frame.message ?? empty, frame.data ?? empty];
    const capacity = buffers.reduce((total, buffer) => total + buffer.length, 32);

    return heap.withBuffers([...buffers, new Uint8Array(capacity)], (...pointers) =>
      heap.withStruct(LIMITS.size, (limitsAt) => heap.withStruct(FRAME.size, (at) => {
        heap.writeLimits(limitsAt, limits);
        const view = heap.view();
        view.setUint8(at + FRAME.type, frame.type ?? FrameType.UNKNOWN);
        view.setUint32(at + FRAME.streamId, frame.streamId ?? 0, true);
        view.setUint16(at + FRAME.version, frame.version ?? 2, true);
        view.setUint8(at + FRAME.resuming, frame.resuming ? 1 : 0);
        view.setUint8(at + FRAME.code, frame.code ?? Reason.OK);
        view.setBigUint64(at + FRAME.nonce, BigInt(frame.nonce ?? 0), true);
        view.setUint32(at + FRAME.receiveWindow, frame.receiveWindow ?? DEFAULT_RECEIVE_WINDOW, true);
        view.setUint32(at + FRAME.credit, frame.credit ?? 0, true);
        [FRAME.sessionId, FRAME.capabilities, FRAME.kind, FRAME.params, FRAME.message, FRAME.data]
          .forEach((offset, index) => heap.writeBytes(at + offset, pointers[index], buffers[index].length));

        const output = pointers[buffers.length];
        return heap.withStruct(Uint32Array.BYTES_PER_ELEMENT, (sizeAt) => {
          const status = wasm._packetmux_frame_encode(at, limitsAt, output, capacity, sizeAt);
          if (status !== CodecStatus.OK) {
            throw new CodecError(status);
          }
          return wasm.HEAPU8.slice(output, output + heap.view().getUint32(sizeAt, true));
        });
      })));
  }

  return { decodeFrame, encodeFrame };
}

let loading = null;

// The core's default for every numeric setting, so no binding repeats them.
function readDefaults(wasm, heap) {
  return heap.withStruct(CONFIG.size, (at) => {
    wasm._packetmux_config_init(at);
    const view = heap.view();
    const defaults = {};
    for (const name of ['maxDataPayload', 'maxControlPayload', 'maxStreams', 'maxQueuedBytes',
      'maxQueuedBytesPerStream', 'maxQueuedControlFrames', 'maxOutstandingPings', 'receiveWindow']) {
      defaults[name] = view.getUint32(at + CONFIG[name], true);
    }
    for (const name of ['helloTimeoutMs', 'pingIntervalMs', 'deadAfterMs', 'drainTimeoutMs']) {
      defaults[name] = Number(view.getBigUint64(at + CONFIG[name], true));
    }
    return defaults;
  });
}

// Loads the WASM module once. Everything that needs it comes from here. wasmUrl is for
// bundlers that move the file: call this first, and connect and Server reuse the result.
export function loadPacketMux({ wasmUrl } = {}) {
  const options = wasmUrl === undefined ? {} : { locateFile: () => wasmUrl };
  loading ??= createModule(options).then((wasm) => {
    const heap = new Heap(wasm);
    heap.defaults = readDefaults(wasm, heap);
    return {
      Session: makeSessionClass(wasm, heap),
      configDefaults: () => ({ ...heap.defaults }),
      ...makeCodec(wasm, heap),
    };
  });
  return loading;
}
