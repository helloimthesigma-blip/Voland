/**
 * §19 mods in the browser: the 7z reader (workers/sevenzip.ts). Fixtures
 * in tests/fixtures/sevenzip/ were made with p7zip / xz from the
 * deterministic contents in tree.ts (commands listed there).
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import { sevenZipFiles, type SevenZipOptions } from "../../workers/sevenzip.ts";
import { arm64File, bigFile, build7z, emptyDirectories, treeFiles, x86File } from "../fixtures/sevenzip/tree.ts";

const FIXTURES = new URL("../fixtures/sevenzip/", import.meta.url);

function fixture(name: string): Blob {
  return new Blob([readFileSync(new URL(name, FIXTURES))]);
}

async function unpack(archive: Blob, options: SevenZipOptions = {}): Promise<Map<string, Uint8Array>> {
  const out = new Map<string, Uint8Array>();
  for await (const file of sevenZipFiles(archive, options)) {
    assert.ok(!out.has(file.path), `duplicate ${file.path}`);
    out.set(file.path.normalize("NFC"), new Uint8Array(await file.data.arrayBuffer()));
  }
  return out;
}

function assertSameFiles(actual: Map<string, Uint8Array>, expected: Map<string, Uint8Array>): void {
  assert.deepEqual([...actual.keys()].sort(), [...expected.keys()].map((k) => k.normalize("NFC")).sort());
  for (const [path, data] of expected) {
    const got = actual.get(path.normalize("NFC"));
    assert.ok(got, `missing ${path}`);
    assert.equal(got.length, data.length, `${path} size`);
    assert.ok(Buffer.from(got).equals(Buffer.from(data)), `${path} contents differ`);
  }
}

for (const name of ["default.7z", "lzma.7z", "copy.7z", "plainheader.7z"]) {
  test(`${name}: every file byte-exact, empty file kept, directories skipped`, async () => {
    const files = await unpack(fixture(name));
    assertSameFiles(files, treeFiles());
    assert.equal(files.get("romfs/empty.dat")?.length, 0);
    for (const dir of [...emptyDirectories, "romfs", "romfs/Data", "exefs"]) assert.ok(!files.has(dir));
  });
}

test("tiny archive reads cross every buffer boundary", async () => {
  for (const name of ["default.7z", "lzma.7z", "copy.7z"]) {
    for (const readChunkBytes of [1, 7, 333]) {
      assertSameFiles(await unpack(fixture(name), { readChunkBytes }), treeFiles());
    }
  }
});

test("large file: LZMA2 over several chunks, LZMA over several steps", async () => {
  const expected = new Map([["big.bin", bigFile()]]);
  assertSameFiles(await unpack(fixture("big.7z")), expected);
  assertSameFiles(await unpack(fixture("big-lzma.7z")), expected);
  assertSameFiles(await unpack(fixture("big.7z"), { readChunkBytes: 1000 }), expected);
  assertSameFiles(await unpack(fixture("big-lzma.7z"), { readChunkBytes: 4093 }), expected);
});

test("BCJ x86 filter", async () => {
  const expected = new Map([["x86.bin", x86File()]]);
  assertSameFiles(await unpack(fixture("x86.7z")), expected);
  assertSameFiles(await unpack(fixture("x86.7z"), { readChunkBytes: 3 }), expected);
});

test("ARM64 filter (stream from xz --arm64)", async () => {
  const expected = new Map([["arm64.bin", arm64File()]]);
  assertSameFiles(await unpack(fixture("arm64.7z")), expected);
  assertSameFiles(await unpack(fixture("arm64.7z"), { readChunkBytes: 5 }), expected);
});

test("encrypted archives are rejected clearly", async () => {
  await assert.rejects(unpack(fixture("encrypted.7z")), /encrypted 7z archives are not supported/);
});

test("LZMA2 uncompressed chunks, with and without a dictionary reset", async () => {
  const a = new TextEncoder().encode("first chunk / ");
  const b = new TextEncoder().encode("second chunk");
  const packed = Uint8Array.from([0x01, 0, a.length - 1, ...a, 0x02, 0, b.length - 1, ...b, 0x00]);
  const size = a.length + b.length;
  const archive = new Blob([build7z(packed, [{ id: [0x21], props: [0] }], [size], [
    { name: "dir/a.txt", size: 5 },
    { name: "dir/b.txt", size: size - 5 },
  ])]);
  const files = await unpack(archive);
  assert.deepEqual([...files.keys()], ["dir/a.txt", "dir/b.txt"]);
  assert.equal(new TextDecoder().decode(files.get("dir/a.txt")), "first");
  assert.equal(new TextDecoder().decode(files.get("dir/b.txt")), " chunk / second chunk");
});

test("unsupported coders and non-7z input are clear errors", async () => {
  const deflate = new Blob([build7z(new Uint8Array(4), [{ id: [0x04, 0x01, 0x08] }], [4], [{ name: "x", size: 4 }])]);
  await assert.rejects(unpack(deflate), /unsupported 7z compression \(Deflate\)/);
  await assert.rejects(unpack(new Blob([new Uint8Array(100)])), /not a 7z archive/);
});

test("corrupted data fails its CRC", async () => {
  const bytes = new Uint8Array(readFileSync(new URL("copy.7z", FIXTURES)));
  bytes[40] = (bytes[40] ?? 0) ^ 0xff;
  await assert.rejects(unpack(new Blob([bytes])), /CRC mismatch/);
});

test("the Mods panel picks the reader by extension", async () => {
  const { archiveFiles, ARCHIVE_PATTERN } = await import("../../workers/mod-store.ts");
  assert.ok(ARCHIVE_PATTERN.test("Mod.7Z") && ARCHIVE_PATTERN.test("mod.zip") && !ARCHIVE_PATTERN.test("mod.rar"));
  const paths: string[] = [];
  for await (const file of archiveFiles("default.7z", fixture("default.7z"))) paths.push(file.path);
  assert.ok(paths.includes("romfs/Data/config.txt"));
});
