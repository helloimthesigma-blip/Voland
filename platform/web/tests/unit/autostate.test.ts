/**
 * Automatic save states (src/autostate.ts): when the next one is due.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { nextAutoStateAt } from "../../src/autostate.ts";

const MINUTE = 60_000;

test("the first state comes two minutes in, then one per interval", () => {
  assert.equal(nextAutoStateAt(1000, null, 10), 1000 + 2 * MINUTE);
  assert.equal(nextAutoStateAt(1000, null, 1), 1000 + MINUTE);
  assert.equal(nextAutoStateAt(1000, 5 * MINUTE, 10), 15 * MINUTE);
  assert.equal(nextAutoStateAt(1000, 5 * MINUTE, 0), Number.POSITIVE_INFINITY);
});
