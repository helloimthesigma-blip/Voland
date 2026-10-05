/**
 * Game library keys and labels (src/library.ts).
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { displayNameOf, gameIdOf } from "../../src/library.ts";

test("a game is keyed by file name, size and modification time", () => {
  assert.equal(gameIdOf({ name: "silksong.nca", size: 10, lastModified: 5 }), "silksong.nca:10:5");
  assert.notEqual(gameIdOf({ name: "a.nca", size: 1, lastModified: 1 }), gameIdOf({ name: "a.nca", size: 1, lastModified: 2 }));
});

test("display names drop the game-file extension", () => {
  assert.equal(displayNameOf("silksong.nca"), "silksong");
  assert.equal(displayNameOf("Hollow Knight.NRO"), "Hollow Knight");
  assert.equal(displayNameOf("readme.txt"), "readme.txt");
});
