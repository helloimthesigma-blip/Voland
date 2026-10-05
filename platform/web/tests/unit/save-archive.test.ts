/**
 * The save browser's archive model (bindings/save-archive.ts): the core's
 * VSAV format round-trips, edits are pure, and save names decode.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import {
  addSaveDirectory, describeSaveName, normalizeSavePath, parseSaveArchive, putSaveFile,
  removeSavePath, sortedSaveEntries, writeSaveArchive,
} from "../../bindings/save-archive.ts";

const bytes = (...values: number[]) => Uint8Array.from(values);

test("archives round-trip, parents first, missing parents added", () => {
  const tree = new Map<string, Uint8Array | null>([
    ["/user1.dat", bytes(1, 2, 3)],
    ["/Restore_Points1/a.dat", bytes(9)],
    ["/shared.dat", new Uint8Array(0)],
  ]);
  const archive = writeSaveArchive(tree);
  const back = parseSaveArchive(archive);
  assert.ok(back);
  assert.deepEqual([...back.keys()], ["/Restore_Points1", "/Restore_Points1/a.dat", "/shared.dat", "/user1.dat"]);
  assert.deepEqual(back.get("/user1.dat"), bytes(1, 2, 3));
  assert.equal(back.get("/Restore_Points1"), null);
  /* Header: "VSAV", version 1, 4 entries. */
  assert.deepEqual([...archive.subarray(0, 12)], [0x56, 0x53, 0x41, 0x56, 1, 0, 0, 0, 4, 0, 0, 0]);
});

test("damaged archives are refused", () => {
  const archive = writeSaveArchive(new Map([["/a", bytes(1)]]));
  assert.equal(parseSaveArchive(archive.subarray(0, archive.byteLength - 1)), null);
  assert.equal(parseSaveArchive(bytes(1, 2, 3)), null);
  const trailing = new Uint8Array(archive.byteLength + 1);
  trailing.set(archive);
  assert.equal(parseSaveArchive(trailing), null);
});

test("edits return new trees", () => {
  const empty = new Map<string, Uint8Array | null>();
  const one = putSaveFile(empty, "dir//sub/f.bin", bytes(7));
  assert.equal(empty.size, 0);
  assert.deepEqual(one.get("/dir/sub/f.bin"), bytes(7));
  const replaced = putSaveFile(one, "/dir/sub/f.bin", bytes(8));
  assert.deepEqual(replaced.get("/dir/sub/f.bin"), bytes(8));
  const withDir = addSaveDirectory(replaced, "/dir/empty");
  assert.equal(withDir.get("/dir/empty"), null);
  const removed = removeSavePath(withDir, "/dir/sub");
  assert.deepEqual([...removed.keys()], ["/dir/empty"]);
  assert.equal(removeSavePath(withDir, "/").size, 0);
  assert.equal(normalizeSavePath("/a/../b"), null);
  assert.deepEqual(sortedSaveEntries(putSaveFile(one, "/a", bytes())).map(([p]) => p), ["/a", "/dir/sub/f.bin"]);
});

test("save names decode to program id, user and type", () => {
  const attribute = "0000930ec0130001" + "0100444e414c4f560100000000000000" + "0000000000000000" + "01" + "00".repeat(31);
  const identity = describeSaveName(`01-${attribute}`);
  assert.ok(identity);
  assert.equal(identity.programId, "010013C00E930000");
  assert.equal(identity.type, 1);
  assert.equal(identity.typeLabel, "Player");
  assert.equal(identity.space, 1);
  assert.equal(describeSaveName("01-abc"), null);
});
