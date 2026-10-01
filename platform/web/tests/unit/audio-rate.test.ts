/**
 * §14 dynamic rate control (src/audio/ring-control.ts): the ratio stays
 * within ±1%, speeds up when the ring is over-full and slows down when it
 * runs low, and a steady drift is absorbed rather than accumulating.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { RATE_LIMIT, nextRatio } from "../../src/audio/ring-control.ts";

const CAPACITY = 4096;

test("ratio is 1 at the setpoint and bounded everywhere", () => {
  assert.equal(nextRatio({ integral: 0 }, CAPACITY / 2, CAPACITY).ratio, 1);
  for (const fill of [0, 100, 1000, 2048, 3000, 4096]) {
    const { ratio } = nextRatio({ integral: 1e9 }, fill, CAPACITY);
    assert.ok(ratio >= 1 - RATE_LIMIT && ratio <= 1 + RATE_LIMIT, `fill ${fill}: ${ratio}`);
  }
});

test("over-full consumes faster, under-full slower", () => {
  assert.ok(nextRatio({ integral: 0 }, 3500, CAPACITY).ratio > 1);
  assert.ok(nextRatio({ integral: 0 }, 500, CAPACITY).ratio < 1);
});

test("a producer 0.5% fast settles near the setpoint instead of filling the ring", () => {
  let fill = CAPACITY / 2;
  let state = { integral: 0 };
  for (let block = 0; block < 200_000; block++) {
    const step = nextRatio(state, fill, CAPACITY);
    state = step.state;
    fill += 128 * 1.005 - 128 * step.ratio;
    fill = Math.max(0, Math.min(CAPACITY, fill));
  }
  assert.ok(Math.abs(fill - CAPACITY / 2) < CAPACITY * 0.1, `settled at ${fill}`);
});
