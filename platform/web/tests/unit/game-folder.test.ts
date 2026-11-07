/**
 * Dumped game folders (src/game-folder.ts): which NCA boots, what goes
 * with it, and what is left out.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { classifyNcaHeader, noGameMessage, scanGameFolder } from "../../src/game-folder.ts";

const HEADER_BYTES = 0x400;

function ncaHeader(contentType: number, programId: bigint, magic = "NCA3"): Uint8Array {
  const bytes = new Uint8Array(HEADER_BYTES);
  for (let i = 0; i < 4; i++) bytes[0x200 + i] = magic.charCodeAt(i);
  bytes[0x205] = contentType;
  new DataView(bytes.buffer).setBigUint64(0x210, programId, true);
  return bytes;
}

function nca(name: string, header: Uint8Array, extraBytes = 0): File {
  return new File([header, new Uint8Array(extraBytes)], name);
}

const BASE = 0x0100aaaa00000000n;

test("headers classify by magic, content type and the update bit", () => {
  assert.deepEqual(classifyNcaHeader(ncaHeader(0, BASE)), { kind: "program", programId: BASE });
  assert.deepEqual(classifyNcaHeader(ncaHeader(0, BASE + 0x800n)), { kind: "update", programId: BASE + 0x800n });
  assert.deepEqual(classifyNcaHeader(ncaHeader(2, BASE)), { kind: "other", programId: BASE });
  assert.equal(classifyNcaHeader(ncaHeader(0, BASE, "\x12\x34\x56\x78")).kind, "encrypted");
  assert.equal(classifyNcaHeader(new Uint8Array(16)).kind, "encrypted");
});

test("a folder boots its largest base program and carries the rest", async () => {
  const program = nca("big.nca", ncaHeader(0, BASE), 4096);
  const small = nca("small.nca", ncaHeader(0, BASE + 1n));
  const control = nca("control.nca", ncaHeader(2, BASE));
  const update = nca("update.nca", ncaHeader(0, BASE + 0x800n), 8192);
  const locked = nca("locked.nca", ncaHeader(0, BASE, "\x00\x00\x00\x00"), 9999);
  const readme = new File(["hi"], "readme.txt");
  const folder = await scanGameFolder([small, control, readme, update, locked, program]);
  assert.equal(folder.program, program);
  assert.deepEqual(folder.extras, [small, control, update]);
  assert.equal(folder.encrypted, 1);
});

test("a folder of encrypted NCAs says so", async () => {
  const folder = await scanGameFolder([nca("a.nca", ncaHeader(0, BASE, "XXXX"))]);
  assert.equal(folder.program, null);
  assert.match(noGameMessage(folder), /1 encrypted/);
  assert.match(noGameMessage({ program: null, extras: [], encrypted: 0 }), /No Program NCA/);
});
