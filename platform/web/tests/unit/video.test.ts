/**
 * Unit tests for bindings/video.ts - the video region's request parsing,
 * slot handshake and NV12 packing (core/video/video_stream.h, §13).
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import {
  VIDEO_DECODE_HEADER_BYTES,
  VIDEO_REC_CONFIGURE,
  VIDEO_REC_DECODE,
  VIDEO_SLOT_COUNT,
  VIDEO_SLOT_HEADER_BYTES,
  VIDEO_SLOT_READY,
  VIDEO_SLOTS_OFFSET,
  abandonVideoSlot,
  acquireVideoSlot,
  packNv12,
  parseVideoRequest,
  publishVideoSlot,
} from "../../bindings/video.ts";

test("parseVideoRequest reads CONFIGURE and copies DECODE data", () => {
  const configure = new Uint8Array(32);
  const cv = new DataView(configure.buffer);
  cv.setUint32(0, 3, true);
  cv.setUint32(4, 1280, true);
  cv.setUint32(8, 720, true);
  configure.set([...new TextEncoder().encode("avc1.640033")], 16);
  assert.deepEqual(parseVideoRequest(VIDEO_REC_CONFIGURE, configure), {
    kind: "configure", generation: 3, width: 1280, height: 720, codec: "avc1.640033",
  });

  const decode = new Uint8Array(VIDEO_DECODE_HEADER_BYTES + 5);
  const dv = new DataView(decode.buffer);
  dv.setUint32(0, 3, true);
  dv.setUint32(4, 42, true);
  dv.setUint32(8, 1, true);
  dv.setUint32(12, 4, true);
  decode.set([0, 0, 1, 0x65, 0xFF], VIDEO_DECODE_HEADER_BYTES);
  const parsed = parseVideoRequest(VIDEO_REC_DECODE, decode);
  assert.equal(parsed.kind, "decode");
  if (parsed.kind !== "decode") return;
  assert.equal(parsed.sequence, 42);
  assert.equal(parsed.key, true);
  assert.deepEqual([...parsed.data], [0, 0, 1, 0x65]);
  decode[VIDEO_DECODE_HEADER_BYTES] = 9;
  assert.equal(parsed.data[0], 0, "the data is a copy");
});

test("slots: acquire until full, publish READY, abandon frees", () => {
  const buffer = new SharedArrayBuffer(VIDEO_SLOTS_OFFSET + VIDEO_SLOT_COUNT * VIDEO_SLOT_HEADER_BYTES);
  const region = { buffer, base: 0 };
  const slots = Array.from({ length: VIDEO_SLOT_COUNT }, () => acquireVideoSlot(region));
  assert.deepEqual(slots, [...Array(VIDEO_SLOT_COUNT).keys()]);
  assert.equal(acquireVideoSlot(region), -1);
  publishVideoSlot(region, 2, { output: 7, sequence: 9, generation: 1, width: 64, height: 32, pitch: 64, chromaOffset: 2048 });
  const header = new DataView(buffer, VIDEO_SLOTS_OFFSET + 2 * VIDEO_SLOT_HEADER_BYTES, VIDEO_SLOT_HEADER_BYTES);
  assert.equal(header.getInt32(0, true), VIDEO_SLOT_READY);
  assert.equal(header.getUint32(4, true), 7);
  assert.equal(header.getUint32(8, true), 9);
  assert.equal(header.getUint32(28, true), 2048);
  abandonVideoSlot(region, 4);
  assert.equal(acquireVideoSlot(region), 4);
});

test("packNv12 interleaves I420 chroma and strips NV12 padding", () => {
  const width = 4, height = 2;
  /* I420 with padded strides: Y stride 6, U/V stride 3. */
  const i420 = new Uint8Array(32);
  i420.set([1, 2, 3, 4, 0, 0, 5, 6, 7, 8, 0, 0], 0);
  i420.set([10, 11, 0], 12);
  i420.set([20, 21, 0], 15);
  const out = new Uint8Array(12);
  const chroma = packNv12(out, i420, "I420", width, height, [
    { offset: 0, stride: 6 }, { offset: 12, stride: 3 }, { offset: 15, stride: 3 },
  ]);
  assert.equal(chroma, 8);
  assert.deepEqual([...out], [1, 2, 3, 4, 5, 6, 7, 8, 10, 20, 11, 21]);

  const nv12 = new Uint8Array(32);
  nv12.set([1, 2, 3, 4, 9, 5, 6, 7, 8, 9], 0);
  nv12.set([10, 20, 11, 21, 9], 10);
  const out2 = new Uint8Array(12);
  packNv12(out2, nv12, "NV12", width, height, [{ offset: 0, stride: 5 }, { offset: 10, stride: 5 }]);
  assert.deepEqual([...out2], [1, 2, 3, 4, 5, 6, 7, 8, 10, 20, 11, 21]);
});
