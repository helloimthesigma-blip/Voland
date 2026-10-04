/**
 * Unit tests for src/input/input-loop.ts pure helpers: the tap minimum hold.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { applyMinimumHold, type HoldState, minimumHoldMs } from "../../src/input/input-loop.ts";

test("minimumHoldMs is two guest frames, bounded", () => {
  assert.equal(minimumHoldMs(10), 200);
  assert.equal(minimumHoldMs(60), 40);
  assert.equal(minimumHoldMs(2), 300);
  assert.equal(minimumHoldMs(0), 300);
});

test("applyMinimumHold: presses at once, short taps held, long holds released at once", () => {
  const A = 1, B = 2;
  let s: HoldState = new Map();
  let r = applyMinimumHold(s, A, 1000, 200); /* press A */
  assert.equal(r.buttons, A);
  s = r.state;
  r = applyMinimumHold(s, 0, 1050, 200); /* released after 50 ms: still down */
  assert.equal(r.buttons, A);
  s = r.state;
  r = applyMinimumHold(s, B, 1150, 200); /* B pressed; A still within its hold */
  assert.equal(r.buttons, A | B);
  s = r.state;
  r = applyMinimumHold(s, B, 1200, 200); /* A's 200 ms are up */
  assert.equal(r.buttons, B);
  s = r.state;
  r = applyMinimumHold(s, 0, 1500, 200); /* B held 350 ms: released immediately */
  assert.equal(r.buttons, 0);
  r = applyMinimumHold(r.state, A, 1501, 200); /* a re-press after release counts anew */
  assert.equal(r.buttons, A);
});
