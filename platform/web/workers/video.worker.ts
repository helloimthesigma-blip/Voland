/**
 * Video worker (DESIGN §13 "Video decode"): the WebCodecs backend for
 * NVDEC. Reads decode requests from the video region's ring
 * (core/video/video_stream.h), feeds them to a hardware VideoDecoder,
 * and writes each decoded picture as NV12 into a free frame slot for the
 * core's VIC to convert. Everything per-frame travels through linear
 * memory; postMessage carries only init and log lines (rule 6).
 */

import { OFF_CAPACITY, OFF_MAGIC, OFF_READ, OFF_READ_SIGNAL, OFF_RING_BASE, OFF_WRITE, OFF_WRITE_SIGNAL, GPU_STREAM_MAGIC, REC_PAD } from "@bindings/gpu-records";
import {
  type MainToVideoMessage,
  type VideoRegion,
  type VideoToMainMessage,
  VIDEO_MAX_HEIGHT,
  VIDEO_MAX_WIDTH,
  abandonVideoSlot,
  acquireVideoSlot,
  packNv12,
  parseVideoRequest,
  publishVideoSlot,
  videoRegion,
  videoSlotPixels,
} from "@bindings/video";

const self: DedicatedWorkerGlobalScope = globalThis as unknown as DedicatedWorkerGlobalScope;

function log(level: "debug" | "info" | "warn" | "error", message: string): void {
  const msg: VideoToMainMessage = { type: "log", level, message };
  self.postMessage(msg);
}

const POLL_MS = 4;
const STREAM_IDLE_POLL_MS = 50;
/* Requests stay in the ring (the core's producer waits for room) while
 * this many decoded frames wait for a free slot, or this many chunks
 * wait in the decoder: backpressure, never dropped frames. */
const MAX_PENDING_FRAMES = 4;
const MAX_DECODE_QUEUE = 8;

type WaitAsync = (a: Int32Array, i: number, v: number) => { async: boolean; value: Promise<string> | string };
const waitAsync = (Atomics as unknown as { waitAsync?: WaitAsync }).waitAsync;

async function waitForChange(word: Int32Array, index: number, seen: number): Promise<void> {
  if (waitAsync) {
    const waited = waitAsync(word, index, seen);
    if (waited.async) await waited.value;
  } else {
    await new Promise((resolve) => setTimeout(resolve, POLL_MS));
  }
}

interface Stream {
  readonly generation: number;
  readonly decoder: VideoDecoder;
  outputs: number;
  sawKey: boolean;
}

class VideoBackend {
  private stream: Stream | null = null;
  private readonly pending: VideoFrame[] = [];
  private scratch = new Uint8Array(0);
  decoded = 0;

  constructor(private readonly region: VideoRegion) {}

  configure(generation: number, width: number, height: number, codec: string): void {
    this.reset();
    const stream: Stream = {
      generation,
      outputs: 0,
      sawKey: false,
      decoder: new VideoDecoder({
        output: (frame) => this.onFrame(stream, frame),
        error: (e) => log("error", `VideoDecoder (${codec}): ${e.message}`),
      }),
    };
    stream.decoder.configure({
      codec,
      codedWidth: width,
      codedHeight: height,
      optimizeForLatency: true,
      hardwareAcceleration: "no-preference",
    });
    this.stream = stream;
    log("info", `video stream ${generation}: ${codec} ${width}x${height}`);
  }

  decode(generation: number, sequence: number, key: boolean, data: Uint8Array): void {
    const stream = this.stream;
    if (!stream || stream.generation !== generation || stream.decoder.state !== "configured") return;
    if (!stream.sawKey && !key) return; /* a decoder starts at a key frame */
    stream.sawKey = true;
    stream.decoder.decode(new EncodedVideoChunk({ type: key ? "key" : "delta", timestamp: sequence, data }));
  }

  private reset(): void {
    for (const frame of this.pending.splice(0)) frame.close();
    if (this.stream && this.stream.decoder.state !== "closed") this.stream.decoder.close();
    this.stream = null;
  }

  private onFrame(stream: Stream, frame: VideoFrame): void {
    if (stream !== this.stream) {
      frame.close();
      return;
    }
    this.pending.push(frame);
    this.drain();
  }

  /** True while new requests should wait in the ring. */
  backlogged(): boolean {
    const queued = this.stream ? this.stream.decoder.decodeQueueSize : 0;
    return this.pending.length >= MAX_PENDING_FRAMES || queued >= MAX_DECODE_QUEUE;
  }

  /** Writes pending frames into free slots, oldest first. */
  drain(): void {
    const stream = this.stream;
    while (stream && this.pending.length) {
      const slot = acquireVideoSlot(this.region);
      if (slot < 0) return;
      const frame = this.pending.shift();
      if (!frame) {
        abandonVideoSlot(this.region, slot);
        return;
      }
      this.write(stream, slot, frame).finally(() => frame.close());
    }
  }

  private async write(stream: Stream, slot: number, frame: VideoFrame): Promise<void> {
    const visible = frame.visibleRect;
    const left = visible ? visible.x & ~1 : 0;
    const top = visible ? visible.y & ~1 : 0;
    const width = Math.min(visible ? visible.width : frame.codedWidth, VIDEO_MAX_WIDTH) & ~1;
    const height = Math.min(visible ? visible.height : frame.codedHeight, VIDEO_MAX_HEIGHT) & ~1;
    const format = frame.format;
    if (format !== "I420" && format !== "NV12") {
      log("warn", `video frame format ${String(format)} unsupported`);
      abandonVideoSlot(this.region, slot);
      return;
    }
    const rect = { x: left, y: top, width, height };
    const size = frame.allocationSize({ rect });
    if (this.scratch.byteLength < size) this.scratch = new Uint8Array(size);
    const layout = await frame.copyTo(this.scratch, { rect });
    const chromaOffset = packNv12(videoSlotPixels(this.region, slot), this.scratch, format, width, height, layout);
    stream.outputs++;
    publishVideoSlot(this.region, slot, {
      output: stream.outputs,
      sequence: frame.timestamp,
      generation: stream.generation,
      width,
      height,
      pitch: width,
      chromaOffset,
    });
    this.decoded++;
    if (this.decoded === 1) log("info", `first video frame decoded: ${width}x${height} ${format}`);
  }
}

async function consumeRequests(memory: WebAssembly.Memory, region: VideoRegion): Promise<void> {
  const buffer = memory.buffer;
  const words = new Int32Array(buffer, region.base, 12);
  const wide = new BigInt64Array(buffer, region.base, 6);
  while (Atomics.load(words, OFF_MAGIC / 4) !== GPU_STREAM_MAGIC) {
    await new Promise((resolve) => setTimeout(resolve, STREAM_IDLE_POLL_MS));
  }
  const ringBase = Number(Atomics.load(wide, OFF_RING_BASE / 8));
  const capacity = Number(Atomics.load(wide, OFF_CAPACITY / 8));
  const backend = new VideoBackend(region);
  for (;;) {
    const seen = Atomics.load(words, OFF_WRITE_SIGNAL / 4);
    const write = Number(Atomics.load(wide, OFF_WRITE / 8));
    let read = Number(Atomics.load(wide, OFF_READ / 8));
    while (read < write && !backend.backlogged()) {
      const at = ringBase + (read % capacity);
      const head = new DataView(buffer, at, 8);
      const type = head.getUint32(0, true);
      const size = head.getUint32(4, true);
      if (type !== REC_PAD) {
        const request = parseVideoRequest(type, new Uint8Array(buffer, at + 8, size - 8));
        if (request.kind === "configure") {
          backend.configure(request.generation, request.width, request.height, request.codec);
        } else if (request.kind === "decode") {
          backend.decode(request.generation, request.sequence, request.key, request.data);
        }
      }
      read += size;
      Atomics.store(wide, OFF_READ / 8, BigInt(read));
      Atomics.add(words, OFF_READ_SIGNAL / 4, 1);
      Atomics.notify(words, OFF_READ_SIGNAL / 4);
    }
    /* Slots free up as the core's VIC consumes frames: retry pending ones. */
    backend.drain();
    if (read < write) await new Promise((resolve) => setTimeout(resolve, POLL_MS)); /* backlogged */
    if (Number(Atomics.load(wide, OFF_WRITE / 8)) === write && read >= write) {
      await Promise.race([
        waitForChange(words, OFF_WRITE_SIGNAL / 4, seen),
        new Promise((resolve) => setTimeout(resolve, POLL_MS)),
      ]);
    }
  }
}

self.addEventListener("message", (event: MessageEvent<MainToVideoMessage>) => {
  if (event.data.type !== "init") return;
  const webCodecs = typeof VideoDecoder !== "undefined";
  const ready: VideoToMainMessage = { type: "ready", webCodecs };
  self.postMessage(ready);
  if (!webCodecs) {
    log("warn", "WebCodecs VideoDecoder unavailable: cutscenes stay black");
    return;
  }
  const region = videoRegion(event.data.memory, event.data.layout);
  consumeRequests(event.data.memory, region).catch((e: unknown) => {
    log("error", `video request consumer stopped: ${e instanceof Error ? e.stack ?? e.message : String(e)}`);
  });
});
