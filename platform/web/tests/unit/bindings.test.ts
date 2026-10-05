/**
 * Remappable controls (src/input/bindings.ts): defaults reproduce the old
 * fixed layouts, rebinding moves a key/button off its old control, and
 * stored bindings are sanitized.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import {
  DEFAULT_BINDINGS, bindKey, bindPadButton, clearKey, sanitizeBindings,
} from "../../src/input/bindings.ts";
import { InputButton, INPUT_AXIS_MAX } from "../../src/input/input-region.ts";
import { isMappedKey, mapKeyboard } from "../../src/input/keyboard-mapping.ts";
import { mapStandardGamepad } from "../../src/input/gamepad-mapping.ts";

const pad = (pressed: readonly number[]) => ({
  connected: true, mapping: "standard", axes: [0, 0, 0, 0],
  buttons: Array.from({ length: 17 }, (_, i) => ({ pressed: pressed.includes(i) })),
});

test("a rebound key drives its new control and leaves the old one", () => {
  const next = bindKey(DEFAULT_BINDINGS, "A", "Space");
  assert.equal(mapKeyboard(new Set(["Space"]), next.keyboard).buttons, InputButton.A);
  assert.equal(mapKeyboard(new Set(["KeyZ"]), next.keyboard).buttons, 0);
  /* Binding Z to B takes it off nothing else; binding X to A takes it off B. */
  const moved = bindKey(next, "A", "KeyX");
  assert.equal(mapKeyboard(new Set(["KeyX"]), moved.keyboard).buttons, InputButton.A);
  assert.deepEqual(moved.keyboard["B"], []);
  assert.equal(isMappedKey("Space", moved.keyboard), false);
  /* The input is not mutated. */
  assert.deepEqual(DEFAULT_BINDINGS.keyboard["A"], ["KeyZ"]);
});

test("stick directions rebind like buttons", () => {
  const next = bindKey(DEFAULT_BINDINGS, "LStickLeft", "KeyH");
  assert.equal(mapKeyboard(new Set(["KeyH"]), next.keyboard).axes[0], -INPUT_AXIS_MAX);
  assert.equal(mapKeyboard(new Set(["KeyA"]), next.keyboard).axes[0], 0);
});

test("gamepad buttons rebind; cleared controls do nothing", () => {
  const swapped = bindPadButton(DEFAULT_BINDINGS, "A", 0); /* bottom face -> A (Xbox-style) */
  assert.equal(mapStandardGamepad(pad([0]), swapped.gamepad).buttons, InputButton.A);
  assert.equal(mapStandardGamepad(pad([1]), swapped.gamepad).buttons, 0);
  const cleared = clearKey(DEFAULT_BINDINGS, "Plus");
  assert.equal(mapKeyboard(new Set(["Equal"]), cleared.keyboard).buttons, 0);
});

test("stored bindings are sanitized; missing controls get defaults", () => {
  const loaded = sanitizeBindings({ keyboard: { A: ["Space", 7], Bogus: ["KeyQ"] }, gamepad: { A: [0, -1, "x"] } });
  assert.deepEqual(loaded.keyboard["A"], ["Space"]);
  assert.equal(loaded.keyboard["Bogus"], undefined);
  assert.deepEqual(loaded.keyboard["B"], ["KeyX"]);
  assert.deepEqual(loaded.gamepad["A"], [0]);
  assert.deepEqual(sanitizeBindings("garbage"), sanitizeBindings({}));
  assert.deepEqual(sanitizeBindings({}).keyboard, DEFAULT_BINDINGS.keyboard);
});
