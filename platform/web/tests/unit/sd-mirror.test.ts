/**
 * §15 guest-write mirroring (workers/sd-persistence.ts): the core's SD
 * manifest parses into path -> "version size", and a diff names exactly
 * the files to store (new or changed) and to remove.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { diffManifests, parseManifest, savePathFromStored, storedLocation } from "../../workers/sd-persistence.ts";

test("manifest lines parse, paths may contain spaces", () => {
  const m = parseManifest("3 120 /switch/app.nro\n1 5 /config/my app/settings.ini\n");
  assert.equal(m.get("/switch/app.nro"), "3 120");
  assert.equal(m.get("/config/my app/settings.ini"), "1 5");
  assert.equal(m.size, 2);
});

test("diff stores new and changed files and removes deleted ones", () => {
  const before = parseManifest("1 10 /a\n2 20 /b\n3 30 /c\n");
  const after = parseManifest("1 10 /a\n4 21 /b\n1 1 /d\n");
  const diff = diffManifests(before, after);
  assert.deepEqual([...diff.changed].sort(), ["/b", "/d"]);
  assert.deepEqual(diff.removed, ["/c"]);
});

test("an unchanged manifest needs nothing", () => {
  const m = parseManifest("1 10 /a\n");
  const diff = diffManifests(m, parseManifest("1 10 /a\n"));
  assert.equal(diff.changed.length + diff.removed.length, 0);
});

test("save paths map to their own OPFS directory and back", () => {
  const key = "a5".padEnd(128, "0");
  const path = `save:03:${key}/progress/slot 1.dat`;
  const location = storedLocation(path);
  assert.deepEqual(location, { directory: "saves", parts: [`03-${key}`, "progress", "slot 1.dat"] });
  assert.equal(savePathFromStored(location?.parts ?? []), path);
  assert.deepEqual(storedLocation("/switch/app.nro"), { directory: "sdmc", parts: ["switch", "app.nro"] });
  assert.equal(storedLocation("save:zz:00/x"), null);
  assert.equal(savePathFromStored(["not-a-save", "x"]), null);
  assert.equal(savePathFromStored([`03-${key}`]), null);
  // Save manifest lines parse like SD ones.
  assert.equal(parseManifest(`2 4 ${path}\n`).get(path), "2 4");
});
