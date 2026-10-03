/**
 * Video worker harness (e2e/video.spec.ts): plays the core's part
 * against workers/video.worker.ts in a real browser - lays out a video
 * region (core/video/video_stream.h) in a shared memory, writes CONFIGURE
 * and DECODE records for SMALL_CLIP the way video_stream.c does, and
 * consumes the NV12 slots the worker fills, like VIC. Results land on
 * window.__videoResult. Served by Vite's dev server only (not built).
 */
import {
  GPU_STREAM_MAGIC,
  GPU_STREAM_VERSION,
  OFF_CAPACITY,
  OFF_MAGIC,
  OFF_RING_BASE,
  OFF_VERSION,
  OFF_WRITE,
  OFF_WRITE_SIGNAL,
} from "@bindings/gpu-records";
import type { MemoryLayout } from "@bindings/layout";
import {
  VIDEO_DECODE_HEADER_BYTES,
  VIDEO_DECODE_KEY,
  VIDEO_PIXELS_OFFSET,
  VIDEO_REC_CONFIGURE,
  VIDEO_REC_DECODE,
  VIDEO_RING_BYTES,
  VIDEO_RING_OFFSET,
  VIDEO_SLOT_COUNT,
  VIDEO_SLOT_FREE,
  VIDEO_SLOT_HEADER_BYTES,
  VIDEO_SLOT_PIXEL_BYTES,
  VIDEO_SLOT_READY,
  VIDEO_SLOTS_OFFSET,
  type MainToVideoMessage,
  type VideoToMainMessage,
} from "@bindings/video";
import { SMALL_CLIP } from "./fixtures/h264-clip";

interface VideoResult {
  decoded: number;
  sequences: number[];
  lumaMeans: number[];
  logs: string[];
  supported: boolean | null;
}

const result: VideoResult = { decoded: 0, sequences: [], lumaMeans: [], logs: [], supported: null };
(window as unknown as { __videoResult: VideoResult }).__videoResult = result;

const WASM_PAGE = 65536;
const pages = Math.ceil((VIDEO_PIXELS_OFFSET + VIDEO_SLOT_COUNT * VIDEO_SLOT_PIXEL_BYTES) / WASM_PAGE);
const memory = new WebAssembly.Memory({ initial: pages, maximum: pages, shared: true });
const buffer = memory.buffer;
const header = new DataView(buffer, 0, VIDEO_RING_OFFSET);
const words = new Int32Array(buffer, 0, VIDEO_RING_OFFSET / 4);
header.setUint32(OFF_MAGIC, GPU_STREAM_MAGIC, true);
header.setUint32(OFF_VERSION, GPU_STREAM_VERSION, true);
header.setBigUint64(OFF_RING_BASE, BigInt(VIDEO_RING_OFFSET), true);
header.setBigUint64(OFF_CAPACITY, BigInt(VIDEO_RING_BYTES), true);

let write = 0;
function record(type: number, payload: Uint8Array): void {
  const size = (8 + payload.byteLength + 7) & ~7;
  const at = VIDEO_RING_OFFSET + write;
  const view = new DataView(buffer, at, 8);
  view.setUint32(0, type, true);
  view.setUint32(4, size, true);
  new Uint8Array(buffer, at + 8, payload.byteLength).set(payload);
  write += size;
}
function publish(): void {
  header.setBigUint64(OFF_WRITE, BigInt(write), true);
  Atomics.add(words, OFF_WRITE_SIGNAL / 4, 1);
  Atomics.notify(words, OFF_WRITE_SIGNAL / 4);
}
function bytes(base64: string): Uint8Array {
  return Uint8Array.from(atob(base64), (c) => c.charCodeAt(0));
}

const configure = new Uint8Array(32);
const cv = new DataView(configure.buffer);
cv.setUint32(0, 1, true);
cv.setUint32(4, SMALL_CLIP.width, true);
cv.setUint32(8, SMALL_CLIP.height, true);
configure.set(new TextEncoder().encode(SMALL_CLIP.codec), 16);
record(VIDEO_REC_CONFIGURE, configure);
const parameterSets = bytes(SMALL_CLIP.parameterSets);
SMALL_CLIP.units.forEach(([key, unit], i) => {
  const slices = bytes(unit);
  const au = new Uint8Array((key ? parameterSets.byteLength : 0) + slices.byteLength);
  if (key) au.set(parameterSets);
  au.set(slices, key ? parameterSets.byteLength : 0);
  const payload = new Uint8Array(VIDEO_DECODE_HEADER_BYTES + au.byteLength);
  const dv = new DataView(payload.buffer);
  dv.setUint32(0, 1, true);
  dv.setUint32(4, i + 1, true);
  dv.setUint32(8, key ? VIDEO_DECODE_KEY : 0, true);
  dv.setUint32(12, au.byteLength, true);
  payload.set(au, VIDEO_DECODE_HEADER_BYTES);
  record(VIDEO_REC_DECODE, payload);
});

/* Consume slots like VIC: read, then free. */
function consume(): void {
  const slots = new Int32Array(buffer, VIDEO_SLOTS_OFFSET, (VIDEO_SLOT_COUNT * VIDEO_SLOT_HEADER_BYTES) / 4);
  for (let slot = 0; slot < VIDEO_SLOT_COUNT; slot++) {
    const base = (slot * VIDEO_SLOT_HEADER_BYTES) / 4;
    if (Atomics.load(slots, base) !== VIDEO_SLOT_READY) continue;
    const sequence = slots[base + 2] ?? 0;
    const width = slots[base + 4] ?? 0;
    const height = slots[base + 5] ?? 0;
    const luma = new Uint8Array(buffer, VIDEO_PIXELS_OFFSET + slot * VIDEO_SLOT_PIXEL_BYTES, width * height);
    let sum = 0;
    for (const v of luma) sum += v;
    result.sequences.push(sequence);
    result.lumaMeans.push(Math.round(sum / Math.max(1, luma.length)));
    result.decoded++;
    Atomics.store(slots, base, VIDEO_SLOT_FREE);
  }
}

async function main(): Promise<void> {
  if (typeof VideoDecoder !== "undefined") {
    const support = await VideoDecoder.isConfigSupported({ codec: SMALL_CLIP.codec, codedWidth: 64, codedHeight: 64 });
    result.supported = support.supported ?? false;
  } else {
    result.supported = false;
  }
  const worker = new Worker(new URL("../workers/video.worker.ts", import.meta.url), { type: "module" });
  worker.addEventListener("message", (event: MessageEvent<VideoToMainMessage>) => {
    if (event.data.type === "log" || event.data.type === "error") result.logs.push(event.data.message);
  });
  const zero = 0n;
  const layout: MemoryLayout = {
    guestRamBase: zero, guestRamSize: zero, pageTableL1Base: zero, framebufferSlotBase: zero, audioRingBase: zero,
    gpuRingBase: zero, gpuCompletionRingBase: zero, inputRegionBase: zero, traceBufferBase: zero,
    breakpointRegionBase: zero, videoRegionBase: zero,
  };
  worker.postMessage({ type: "init", memory, layout } satisfies MainToVideoMessage);
  publish();
  const poll = (): void => {
    consume();
    if (result.decoded < SMALL_CLIP.units.length) setTimeout(poll, 5);
  };
  poll();
}

main().catch((e: unknown) => result.logs.push(`harness: ${String(e)}`));
