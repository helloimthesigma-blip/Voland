/**
 * Save-state plans (workers/savestate.ts): the core's Savestate_Plan is
 * decoded as laid out in core/emulator.h, and a stored plan restores only
 * where every range sits at the same address with the same capacity.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import {
  CHUNK_BYTES, type ChunkCodec, PLAN_BYTES, newStateId, parsePlan, planMismatch, readState, readStoredPlan, writeState,
} from "../../workers/savestate.ts";

function planBytes(program: bigint, ranges: readonly (readonly [number, number, number, number])[]): Uint8Array {
  const bytes = new Uint8Array(PLAN_BYTES);
  const v = new DataView(bytes.buffer);
  v.setUint32(0, 1, true);
  v.setUint32(4, ranges.length, true);
  v.setBigUint64(8, program, true);
  v.setBigUint64(16, 600000n, true);
  v.setBigUint64(24, 19_200_000n * 5n, true);
  ranges.forEach(([address, length, capacity, kind], i) => {
    const at = 32 + 32 * i;
    v.setBigUint64(at, BigInt(address), true);
    v.setBigUint64(at + 8, BigInt(length), true);
    v.setBigUint64(at + 16, BigInt(capacity), true);
    v.setUint32(at + 24, kind, true);
  });
  return bytes;
}

const RANGES = [[0x1000, 600000, 600000, 1], [0x100000000, 0x2000_0000, 0x1_0000_0000, 5]] as const;

test("plans decode as the core lays them out", () => {
  const plan = parsePlan(planBytes(0x010013c00e930000n, RANGES));
  assert.equal(plan.version, 1);
  assert.equal(plan.programId, 0x010013c00e930000n);
  assert.equal(plan.virtualTicks / 19_200_000, 5);
  assert.deepEqual(plan.ranges[1], { address: 0x100000000, bytes: 0x2000_0000, capacity: 0x1_0000_0000, kind: 5 });
});

test("a state loads only into the same layout and game", () => {
  const stored = parsePlan(planBytes(1n, RANGES));
  assert.equal(planMismatch(stored, parsePlan(planBytes(1n, [[0x1000, 600000, 600000, 1], [0x100000000, 0x3000_0000, 0x1_0000_0000, 5]]))), null);
  assert.match(planMismatch(stored, parsePlan(planBytes(2n, RANGES))) ?? "", /different game/);
  assert.match(planMismatch(stored, parsePlan(planBytes(1n, [[0x2000, 600000, 600000, 1], RANGES[1]]))) ?? "", /laid out differently/);
});

test("state ids name the title and moment", () => {
  assert.equal(newStateId("010013C00E930000", 36), "010013C00E930000-10");
});

/* An in-memory FileSystemSyncAccessHandle: the calls savestate.ts makes. */
function memoryFile(): FileSystemSyncAccessHandle {
  let data = new Uint8Array(0);
  const handle = {
    truncate(size: number): void { data = data.slice(0, size); },
    write(bytes: Uint8Array, options: { at: number }): number {
      const end = options.at + bytes.byteLength;
      if (end > data.byteLength) {
        const grown = new Uint8Array(end);
        grown.set(data);
        data = grown;
      }
      data.set(bytes, options.at);
      return bytes.byteLength;
    },
    read(into: Uint8Array, options: { at: number }): number {
      const part = data.subarray(options.at, options.at + into.byteLength);
      into.set(part);
      return part.byteLength;
    },
    getSize(): number { return data.byteLength; },
    flush(): void {},
    close(): void {},
  };
  return handle as unknown as FileSystemSyncAccessHandle;
}

/* Packs a chunk of one repeated byte into that byte; anything else raw. */
function fillCodec(memory: ArrayBuffer): ChunkCodec {
  return {
    compress(address, length) {
      const bytes = new Uint8Array(memory, address, length);
      return bytes.every((b) => b === bytes[0]) ? Uint8Array.of(bytes[0] ?? 0) : null;
    },
    decompress(packed, address, length) {
      if (packed.byteLength !== 1) return false;
      new Uint8Array(memory, address, length).fill(packed[0] ?? 0);
      return true;
    },
  };
}

test("states round-trip packed, raw and zero chunks", () => {
  const base = 0x10000;
  const bytes = 3 * CHUNK_BYTES + 100;
  const plan = parsePlan(planBytes(1n, [[base, bytes, bytes, 1]]));
  const memory = new ArrayBuffer(base + 4 * CHUNK_BYTES);
  const view = new Uint8Array(memory, base, bytes);
  view.fill(0xAB, 0, CHUNK_BYTES);                                   /* packs */
  for (let i = CHUNK_BYTES; i < 2 * CHUNK_BYTES; i++) view[i] = i * 7; /* raw */
  view.fill(0x5A, 3 * CHUNK_BYTES);                                  /* short tail, packs */
  const expected = view.slice();
  const file = memoryFile();
  const written = writeState(file, memory, plan, fillCodec(memory));
  assert.ok(written < 2 * CHUNK_BYTES);
  const stored = readStoredPlan(file);
  assert.ok(stored);
  view.fill(0xFF);
  assert.ok(readState(file, memory, stored, plan, fillCodec(memory)));
  assert.deepEqual(new Uint8Array(memory, base, bytes), expected);
});

test("a packed chunk that does not decode refuses the load", () => {
  const plan = parsePlan(planBytes(1n, [[0, CHUNK_BYTES, CHUNK_BYTES, 1]]));
  const memory = new ArrayBuffer(CHUNK_BYTES);
  new Uint8Array(memory).fill(3);
  const file = memoryFile();
  writeState(file, memory, plan, fillCodec(memory));
  const refuse: ChunkCodec = { compress: () => null, decompress: () => false };
  assert.equal(readState(file, memory, plan, plan, refuse), false);
});
