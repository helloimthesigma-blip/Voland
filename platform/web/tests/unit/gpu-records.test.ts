/**
 * Draw records (bindings/gpu-records.ts): parseDrawInto fills the
 * executor's reused scratch with what parseDraw returns, and a later
 * record with fewer targets does not keep the earlier one's.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { DRAW_BYTES, createDrawScratch, parseDraw, parseDrawInto } from "../../bindings/gpu-records.ts";

/** A draw record whose every word is distinct (word i = i * 7 + seed), with `targets` targets. */
function record(targets: number, seed: number): DataView {
  const v = new DataView(new ArrayBuffer(DRAW_BYTES));
  for (let at = 0; at + 4 <= DRAW_BYTES; at += 4) v.setUint32(at, (at / 4) * 7 + seed, true);
  v.setUint32(4, targets, true);
  return v;
}

test("parseDrawInto gives what parseDraw does", () => {
  const scratch = createDrawScratch();
  for (const [targets, seed] of [[3, 1], [1, 2], [8, 3], [0, 4]] as const) {
    const v = record(targets, seed);
    assert.deepEqual(parseDrawInto(v, scratch), parseDraw(v));
  }
});

test("parseDrawInto reuses one object and its target arrays", () => {
  const scratch = createDrawScratch();
  const first = parseDrawInto(record(2, 5), scratch);
  const firstTargets = first.targets;
  const second = parseDrawInto(record(2, 9), scratch);
  assert.equal(second, first);
  assert.equal(second.targets, firstTargets);
  assert.equal(second.targets.length, 2);
});
