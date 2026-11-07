/**
 * Game library keys and labels (src/library.ts).
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { type LibraryGame, displayNameOf, folderIdOf, gameIdOf, shownNameOf } from "../../src/library.ts";

test("a game is keyed by file name, size and modification time", () => {
  assert.equal(gameIdOf({ name: "silksong.nca", size: 10, lastModified: 5 }), "silksong.nca:10:5");
  assert.notEqual(gameIdOf({ name: "a.nca", size: 1, lastModified: 1 }), gameIdOf({ name: "a.nca", size: 1, lastModified: 2 }));
});

test("display names drop the game-file extension", () => {
  assert.equal(displayNameOf("silksong.nca"), "silksong");
  assert.equal(displayNameOf("Hollow Knight.NRO"), "Hollow Knight");
  assert.equal(displayNameOf("readme.txt"), "readme.txt");
});

test("a game folder is keyed by its name and its Program NCA", () => {
  const program = { name: "abc.nca", size: 10, lastModified: 5 };
  assert.equal(folderIdOf("Silksong", program), "folder:Silksong/abc.nca:10:5");
  assert.notEqual(folderIdOf("Silksong", program), gameIdOf(program));
});

test("a game shows its own title once its control NCA gave one", () => {
  const game: LibraryGame = { id: "x", name: "0100abc", fileName: "a.nca", size: 1, titleId: null, addedAt: 0,
                              lastPlayedAt: null, handle: null };
  assert.equal(shownNameOf(game), "0100abc");
  assert.equal(shownNameOf({ ...game, title: "Hollow Knight Silksong" }), "Hollow Knight Silksong");
});
