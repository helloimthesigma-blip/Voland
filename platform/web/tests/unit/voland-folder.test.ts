/**
 * The Voland folder (src/voland-folder.ts): how games/, firmware/ and
 * mods/ are sorted, which loose NCAs go with which game, and how a
 * mods/<name> folder finds its game.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { type FolderFile, belongsTo, inputFiles, modTitleId, scanFolder } from "../../src/voland-folder.ts";

const HEADER_BYTES = 0x400;
const BASE = 0x0100aaaa00000000n;
const OTHER = 0x0100bbbb00000000n;

function ncaBytes(contentType: number, programId: bigint, magic = "NCA3", extra = 0): Uint8Array {
  const bytes = new Uint8Array(HEADER_BYTES + extra);
  for (let i = 0; i < 4; i++) bytes[0x200 + i] = magic.charCodeAt(i);
  bytes[0x205] = contentType;
  new DataView(bytes.buffer).setBigUint64(0x210, programId, true);
  return bytes;
}

function at(path: string, bytes: Uint8Array | string = ""): FolderFile {
  const parts = path.split("/");
  return { path: parts, file: new File([bytes], parts[parts.length - 1] ?? path) };
}

const PROGRAM = 0;
const CONTROL = 2;
const DATA = 4;

test("an update, DLC and control NCA belong to their game by program id", () => {
  assert.equal(belongsTo(BASE, BASE), true);
  assert.equal(belongsTo(BASE, BASE + 0x800n), true);
  assert.equal(belongsTo(BASE, BASE + 0x1001n), true);
  assert.equal(belongsTo(BASE, BASE + 0x1fffn), true);
  assert.equal(belongsTo(BASE, BASE + 0x1000n), false);
  assert.equal(belongsTo(BASE, BASE + 0x2000n), false);
  assert.equal(belongsTo(BASE, OTHER), false);
});

test("games/: subfolders and loose NCAs, matched by program id", async () => {
  const scan = await scanFolder([
    at("games/Big Game/main.nca", ncaBytes(PROGRAM, OTHER, "NCA3", 4096)),
    at("games/Big Game/control.nca", ncaBytes(CONTROL, OTHER)),
    at("games/Big Game/locked.nca", ncaBytes(PROGRAM, OTHER, "XXXX")),
    at("games/loose.nca", ncaBytes(PROGRAM, BASE)),
    at("games/loose-update.nca", ncaBytes(PROGRAM, BASE + 0x800n)),
    at("games/loose-dlc1.nca", ncaBytes(DATA, BASE + 0x1001n)),
    at("games/loose-dlc2.nca", ncaBytes(DATA, BASE + 0x1002n)),
    at("games/lost-update.nca", ncaBytes(PROGRAM, 0x0100cccc00000800n)),
    at("games/notes.txt", "hi"),
  ], ["games", "firmware", "mods"]);
  assert.deepEqual(scan.games.map((g) => g.name), ["Big Game", "loose"]);
  const [big, loose] = scan.games;
  assert.equal(big?.dir, "Big Game");
  assert.equal(big?.program.file.name, "main.nca");
  assert.deepEqual(big?.extras.map((f) => f.file.name), ["control.nca"]);
  assert.equal(big?.hasUpdate, false);
  assert.match(big?.id ?? "", /^folder:Big Game\/main\.nca:/);
  assert.equal(loose?.dir, null);
  assert.equal(loose?.programId, BASE);
  assert.deepEqual(loose?.extras.map((f) => f.file.name).sort(), ["loose-dlc1.nca", "loose-dlc2.nca", "loose-update.nca"]);
  assert.equal(loose?.hasUpdate, true);
  assert.equal(loose?.dlc, 2);
  assert.match(loose?.id ?? "", /^loose\.nca:/);
  assert.deepEqual(scan.encrypted, ["games/Big Game/locked.nca"]);
  assert.deepEqual(scan.orphans, ["games/lost-update.nca"]);
  assert.deepEqual(scan.missingDirs, []);
});

test("firmware/, mods/, stray NCAs and missing folders", async () => {
  const scan = await scanFolder([
    at("Firmware/a.nca", ncaBytes(DATA, 0x0100000000000802n)),
    at("Firmware/sub/b.nca", "x"),
    at("mods/Celeste/Big Mod/romfs/a.bin", "aa"),
    at("mods/Celeste/Big Mod/romfs/b/c.bin", "b"),
    at("mods/0100AAAA00000000/pack.zip", "zip"),
    at("mods/0100AAAA00000000/update.nca", "nca"),
    at("mods/0100AAAA00000000/readme.txt", "ignored"),
    at("stray.nca", "x"),
  ], ["Firmware", "Firmware/sub", "mods"]);
  assert.deepEqual(scan.missingDirs, ["games"]);
  assert.deepEqual(scan.firmware.map((f) => f.file.name), ["a.nca", "b.nca"]);
  assert.deepEqual(scan.stray, ["stray.nca"]);
  assert.deepEqual(scan.mods.map((m) => [m.key, m.name, m.archive, m.files.map((f) => f.path.join("/"))]), [
    ["0100AAAA00000000", "pack.zip", true, ["pack.zip"]],
    ["0100AAAA00000000", "update.nca", false, ["update.nca"]],
    ["Celeste", "Big Mod", false, ["romfs/a.bin", "romfs/b/c.bin"]],
  ]);
  assert.match(scan.mods[2]?.fingerprint ?? "", /^2:3:/);
});

test("a mods/<key> folder finds its game by title id or name", () => {
  const known = [
    { titleId: "01002B30028F6000", names: ["Celeste"] },
    { titleId: "0100A6301214E000", names: ["Hollow Knight: Silksong", "silksong"] },
  ];
  assert.equal(modTitleId("0100a6301214e000", known), "0100A6301214E000");
  assert.equal(modTitleId("celeste", known), "01002B30028F6000");
  assert.equal(modTitleId("Hollow Knight - Silksong", known), "0100A6301214E000");
  assert.equal(modTitleId("Silksong", known), "0100A6301214E000");
  assert.equal(modTitleId("Mario", known), null);
  assert.equal(modTitleId("--", known), null);
});

test("a folder input's files drop the chosen folder's name and hidden files", () => {
  const file = (rel: string): File => {
    const f = new File(["x"], rel.split("/").pop() ?? rel);
    Object.defineProperty(f, "webkitRelativePath", { value: rel });
    return f;
  };
  const picked = inputFiles([file("Voland/games/a.nca"), file("Voland/.DS_Store"), file("Voland/mods/X/.git/config")]);
  assert.equal(picked.name, "Voland");
  assert.deepEqual(picked.files.map((f) => f.path.join("/")), ["games/a.nca"]);
  assert.deepEqual(picked.dirs, ["games"]);
});
