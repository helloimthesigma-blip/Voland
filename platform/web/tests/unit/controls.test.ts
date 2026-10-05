/**
 * Controls legend labels (src/input/key-labels.ts): physical key
 * codes read as the key caps a player sees.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { keyLabel } from "../../src/input/key-labels.ts";

test("letter, arrow and symbol keys get key-cap labels", () => {
  assert.equal(keyLabel("KeyZ"), "Z");
  assert.equal(keyLabel("ArrowLeft"), "←");
  assert.equal(keyLabel("Equal"), "=");
  assert.equal(keyLabel("Digit1"), "1");
  assert.equal(keyLabel("ShiftLeft"), "Left Shift");
  assert.equal(keyLabel("Space"), "Space");
});
