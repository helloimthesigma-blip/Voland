/**
 * Mirror of core/video/video_stream.h: the video region's layout, the
 * decode-request records and the decoded-frame slots (DESIGN §13 "Video
 * decode"). Requests use the GPU stream's header and record framing
 * (bindings/gpu-records.ts OFF_*), at the region's start. Pure logic
 * only - the WebCodecs side is workers/video.worker.ts.
 */

import type { MemoryLayout } from "./layout";

export const VIDEO_SLOT_COUNT = 6;
export const VIDEO_MAX_WIDTH = 1920;
export const VIDEO_MAX_HEIGHT = 1088;
export const VIDEO_SLOT_PIXEL_BYTES = (VIDEO_MAX_WIDTH * VIDEO_MAX_HEIGHT * 3) / 2;
export const VIDEO_SLOT_HEADER_BYTES = 32;
export const VIDEO_SLOTS_OFFSET = 64;
export const VIDEO_RING_OFFSET = 4096;
export const VIDEO_RING_BYTES = 4 * 1024 * 1024;
export const VIDEO_PIXELS_OFFSET = VIDEO_RING_OFFSET + VIDEO_RING_BYTES;

export const VIDEO_REC_CONFIGURE = 1;
export const VIDEO_REC_DECODE = 2;
export const VIDEO_DECODE_HEADER_BYTES = 16;
export const VIDEO_DECODE_KEY = 1;
export const VIDEO_CODEC_STRING_OFFSET = 16;
export const VIDEO_CODEC_STRING_BYTES = 16;

export const VIDEO_SLOT_FREE = 0;
export const VIDEO_SLOT_WRITING = 1;
export const VIDEO_SLOT_READY = 2;

const SLOT_OFF_STATE = 0;
const SLOT_OFF_OUTPUT = 4;
const SLOT_OFF_SEQUENCE = 8;
const SLOT_OFF_GENERATION = 12;
const SLOT_OFF_WIDTH = 16;
const SLOT_OFF_HEIGHT = 20;
const SLOT_OFF_PITCH = 24;
const SLOT_OFF_CHROMA = 28;

export type VideoRequest =
  | { readonly kind: "configure"; readonly generation: number; readonly width: number; readonly height: number; readonly codec: string }
  | { readonly kind: "decode"; readonly generation: number; readonly sequence: number; readonly key: boolean; readonly data: Uint8Array }
  | { readonly kind: "unknown"; readonly type: number };

/** Parses one request record's payload (a view into the ring). The
 * DECODE data is copied out: the ring slot is reused once read. */
export function parseVideoRequest(type: number, payload: Uint8Array): VideoRequest {
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  if (type === VIDEO_REC_CONFIGURE) {
    const raw = payload.subarray(VIDEO_CODEC_STRING_OFFSET, VIDEO_CODEC_STRING_OFFSET + VIDEO_CODEC_STRING_BYTES);
    const end = raw.indexOf(0);
    const codec = String.fromCharCode(...raw.subarray(0, end < 0 ? raw.length : end));
    return {
      kind: "configure",
      generation: view.getUint32(0, true),
      width: view.getUint32(4, true),
      height: view.getUint32(8, true),
      codec,
    };
  }
  if (type === VIDEO_REC_DECODE) {
    const bytes = view.getUint32(12, true);
    const data = new Uint8Array(bytes);
    data.set(payload.subarray(VIDEO_DECODE_HEADER_BYTES, VIDEO_DECODE_HEADER_BYTES + bytes));
    return {
      kind: "decode",
      generation: view.getUint32(0, true),
      sequence: view.getUint32(4, true),
      key: (view.getUint32(8, true) & VIDEO_DECODE_KEY) !== 0,
      data,
    };
  }
  return { kind: "unknown", type };
}

export interface VideoRegion {
  readonly buffer: ArrayBufferLike;
  readonly base: number;
}

export function videoRegion(memory: WebAssembly.Memory, layout: MemoryLayout): VideoRegion {
  return { buffer: memory.buffer, base: Number(layout.videoRegionBase) };
}

function slotStateWord(region: VideoRegion): Int32Array {
  return new Int32Array(region.buffer, region.base + VIDEO_SLOTS_OFFSET, (VIDEO_SLOT_COUNT * VIDEO_SLOT_HEADER_BYTES) / 4);
}

/** Claims a free slot (FREE -> WRITING), or -1 if every slot is busy. */
export function acquireVideoSlot(region: VideoRegion): number {
  const words = slotStateWord(region);
  for (let slot = 0; slot < VIDEO_SLOT_COUNT; slot++) {
    const index = (slot * VIDEO_SLOT_HEADER_BYTES + SLOT_OFF_STATE) / 4;
    if (Atomics.compareExchange(words, index, VIDEO_SLOT_FREE, VIDEO_SLOT_WRITING) === VIDEO_SLOT_FREE) return slot;
  }
  return -1;
}

export function videoSlotPixels(region: VideoRegion, slot: number): Uint8Array {
  return new Uint8Array(region.buffer, region.base + VIDEO_PIXELS_OFFSET + slot * VIDEO_SLOT_PIXEL_BYTES, VIDEO_SLOT_PIXEL_BYTES);
}

export interface PublishedVideoFrame {
  readonly output: number;
  readonly sequence: number;
  readonly generation: number;
  readonly width: number;
  readonly height: number;
  readonly pitch: number;
  readonly chromaOffset: number;
}

/** Fills a WRITING slot's header and hands it to the core (READY). */
export function publishVideoSlot(region: VideoRegion, slot: number, frame: PublishedVideoFrame): void {
  const at = region.base + VIDEO_SLOTS_OFFSET + slot * VIDEO_SLOT_HEADER_BYTES;
  const view = new DataView(region.buffer, at, VIDEO_SLOT_HEADER_BYTES);
  view.setUint32(SLOT_OFF_OUTPUT, frame.output, true);
  view.setUint32(SLOT_OFF_SEQUENCE, frame.sequence, true);
  view.setUint32(SLOT_OFF_GENERATION, frame.generation, true);
  view.setUint32(SLOT_OFF_WIDTH, frame.width, true);
  view.setUint32(SLOT_OFF_HEIGHT, frame.height, true);
  view.setUint32(SLOT_OFF_PITCH, frame.pitch, true);
  view.setUint32(SLOT_OFF_CHROMA, frame.chromaOffset, true);
  Atomics.store(slotStateWord(region), (slot * VIDEO_SLOT_HEADER_BYTES + SLOT_OFF_STATE) / 4, VIDEO_SLOT_READY);
}

/** Returns a WRITING slot unused (the frame was dropped). */
export function abandonVideoSlot(region: VideoRegion, slot: number): void {
  Atomics.store(slotStateWord(region), (slot * VIDEO_SLOT_HEADER_BYTES + SLOT_OFF_STATE) / 4, VIDEO_SLOT_FREE);
}

/**
 * Packs a decoded picture into NV12 (Y plane then interleaved CbCr, both
 * with pitch `width`) at `out`. `planes` is I420 (Y, U, V) or NV12 (Y,
 * UV) as VideoFrame.copyTo laid it out. Returns the chroma offset.
 */
export function packNv12(
  out: Uint8Array,
  source: Uint8Array,
  format: "I420" | "NV12",
  width: number,
  height: number,
  layout: readonly { readonly offset: number; readonly stride: number }[],
): number {
  const y = layout[0];
  if (!y) return 0;
  for (let row = 0; row < height; row++) {
    out.set(source.subarray(y.offset + row * y.stride, y.offset + row * y.stride + width), row * width);
  }
  const chromaOffset = width * height;
  const chromaRows = height >> 1;
  if (format === "NV12") {
    const uv = layout[1];
    if (!uv) return chromaOffset;
    for (let row = 0; row < chromaRows; row++) {
      out.set(source.subarray(uv.offset + row * uv.stride, uv.offset + row * uv.stride + width), chromaOffset + row * width);
    }
    return chromaOffset;
  }
  const u = layout[1];
  const v = layout[2];
  if (!u || !v) return chromaOffset;
  const half = width >> 1;
  for (let row = 0; row < chromaRows; row++) {
    const dst = chromaOffset + row * width;
    const us = u.offset + row * u.stride;
    const vs = v.offset + row * v.stride;
    for (let x = 0; x < half; x++) {
      out[dst + 2 * x] = source[us + x] ?? 0;
      out[dst + 2 * x + 1] = source[vs + x] ?? 0;
    }
  }
  return chromaOffset;
}

export type MainToVideoMessage = { readonly type: "init"; readonly memory: WebAssembly.Memory; readonly layout: MemoryLayout };

export type VideoToMainMessage =
  | { readonly type: "log"; readonly level: "debug" | "info" | "warn" | "error"; readonly message: string }
  | { readonly type: "error"; readonly message: string }
  | { readonly type: "ready"; readonly webCodecs: boolean };
