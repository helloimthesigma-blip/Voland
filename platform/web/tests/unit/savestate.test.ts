/**
 * Save-state plans (workers/savestate.ts): the core's Savestate_Plan is
 * decoded as laid out in core/emulator.h, and a stored plan restores only
 * where every range sits at the same address with the same capacity.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { PLAN_BYTES, newStateId, parsePlan, planMismatch } from "../../workers/savestate.ts";

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
