/**
 * The heavy-game warning (src/memory-guard.ts): when it shows and what it
 * says.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { memoryWarning } from "../../src/memory-guard.ts";

const GIB = 1024 ** 3;

test("a game that used little memory, or a small one never run, gets no warning", () => {
  assert.equal(memoryWarning(1 * GIB, 16 * GIB, 8), null);
  assert.equal(memoryWarning(null, 2 * GIB, null), null);
});

test("a heavy game is named by what it used last time", () => {
  const text = memoryWarning(3.4 * GIB, 1 * GIB, 8) ?? "";
  assert.match(text, /^Heavy game\. Last time it used 3\.4 GB/);
  assert.doesNotMatch(text, /only/);
});

test("a big game never run here is judged by its size", () => {
  assert.match(memoryWarning(null, 16 * GIB, null) ?? "", /big game \(16 GB\)/);
});

test("a device with little memory gets the stronger wording", () => {
  assert.match(memoryWarning(3 * GIB, 0, 4) ?? "", /reports only 4 GB/);
});
