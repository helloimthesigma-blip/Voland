/**
 * The JIT's cross-session hot list file (workers/jit-hot-store.ts).
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { JIT_HOT_MAX_OFFSETS, hotListName, parseHotList, serializeHotList } from "../../workers/jit-hot-store.ts";

function bufferOf(bytes: Uint8Array): ArrayBuffer {
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer;
}

test("a hot list survives a round trip", () => {
  const offsets = new Uint32Array([0, 0x40, 0x12345678, 0xfffffffc]);
  assert.deepEqual([...parseHotList(bufferOf(serializeHotList(offsets)))], [...offsets]);
  assert.equal(parseHotList(bufferOf(serializeHotList(new Uint32Array(0)))).length, 0);
});

test("damaged, truncated or other-version lists are empty", () => {
  const good = serializeHotList(new Uint32Array([4, 8]));
  assert.equal(parseHotList(bufferOf(good.subarray(0, good.length - 4))).length, 0); /* truncated */
  assert.equal(parseHotList(new ArrayBuffer(5)).length, 0);
  const wrongMagic = good.slice();
  wrongMagic[0] = (wrongMagic[0] ?? 0) ^ 1;
  assert.equal(parseHotList(bufferOf(wrongMagic)).length, 0);
  const wrongVersion = good.slice();
  wrongVersion[4] = (wrongVersion[4] ?? 0) + 1;
  assert.equal(parseHotList(bufferOf(wrongVersion)).length, 0);
  const huge = new DataView(new ArrayBuffer(12));
  huge.setUint32(0, new DataView(bufferOf(good)).getUint32(0, true), true);
  huge.setUint32(4, 1, true);
  huge.setUint32(8, JIT_HOT_MAX_OFFSETS + 1, true);
  assert.equal(parseHotList(huge.buffer).length, 0);
});

test("lists are named by title and code key", () => {
  assert.equal(hotListName("0100000000010000", 0xabcn), "0100000000010000-0000000000000abc.bin");
  assert.equal(hotListName("0100000000010000", 0n), null);
  assert.equal(hotListName("../etc", 1n), null);
});
