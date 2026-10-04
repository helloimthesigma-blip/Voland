/**
 * Unit tests for workers/save-store.ts: save names, the tar export
 * format (long names through the ustar prefix), and its round trip.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { isSaveName, readTar, saveNameOfTar, tarNameOfSave, writeTar } from "../../workers/save-store.ts";

const NAME = `01-${"ab".repeat(64)}`;

test("save names: SS-<128 hex> only", () => {
  assert.equal(isSaveName(NAME), true);
  assert.equal(isSaveName("01-abc"), false);
  assert.equal(isSaveName(`${NAME}.crswap`), false);
});

test("tar names split for ustar and map back", () => {
  const entry = tarNameOfSave(NAME);
  assert.ok(entry.endsWith(".vsave"));
  assert.equal(saveNameOfTar(entry), NAME);
  assert.equal(saveNameOfTar("README.txt"), null);
});

test("tar round trip, with long names and odd sizes", () => {
  const files = new Map<string, Uint8Array>([
    [tarNameOfSave(NAME), Uint8Array.from({ length: 1300 }, (_, i) => i & 0xff)],
    [tarNameOfSave(`02-${"cd".repeat(64)}`), new Uint8Array(0)],
  ]);
  const tar = writeTar(files);
  assert.equal(tar.byteLength % 512, 0);
  const back = readTar(tar);
  assert.equal(back.size, 2);
  for (const [name, bytes] of files) assert.deepEqual([...(back.get(name) ?? [])], [...bytes]);
  /* A truncated tar yields what is whole, never garbage. */
  const cut = readTar(tar.slice(0, 700));
  assert.equal(cut.size, 0);
});
