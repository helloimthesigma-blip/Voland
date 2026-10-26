/**
 * §19 mods in the browser (workers/mod-store.ts): priority order, safe
 * names and paths, and the zip reader (stored and deflate entries).
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { orderedNames, safeModName, safeParts, zipEntries, zipFiles } from "../../workers/mod-store.ts";

test("saved order first, the rest by name", () => {
  assert.deepEqual(orderedNames(["b", "a", "c"], ["c", "gone"]), ["c", "a", "b"]);
  assert.deepEqual(orderedNames([], ["x"]), []);
});

test("mod names and archive paths cannot escape their folder", () => {
  assert.equal(safeModName("60 FPS.zip"), "60 FPS");
  assert.equal(safeModName("HD Pack.7z"), "HD Pack");
  assert.equal(safeModName("a/b:c"), "a_b_c");
  assert.equal(safeModName(".."), "mod");
  assert.deepEqual(safeParts("../../romfs/./a\\b.bin"), ["romfs", "a", "b.bin"]);
});

async function deflateRaw(bytes: Uint8Array): Promise<Uint8Array> {
  const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream("deflate-raw"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

/** A minimal zip writer: one local header + data per entry, then the
 * central directory and end record (CRCs left 0; the reader ignores them). */
function buildZip(entries: readonly { name: string; method: number; data: Uint8Array; size: number }[]): Uint8Array {
  const enc = new TextEncoder();
  const parts: Uint8Array[] = [];
  const central: Uint8Array[] = [];
  let offset = 0;
  for (const e of entries) {
    const name = enc.encode(e.name);
    const local = new DataView(new ArrayBuffer(30));
    local.setUint32(0, 0x04034b50, true);
    local.setUint16(8, e.method, true);
    local.setUint32(18, e.data.length, true);
    local.setUint32(22, e.size, true);
    local.setUint16(26, name.length, true);
    parts.push(new Uint8Array(local.buffer), name, e.data);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014b50, true);
    c.setUint16(10, e.method, true);
    c.setUint32(20, e.data.length, true);
    c.setUint32(24, e.size, true);
    c.setUint16(28, name.length, true);
    c.setUint32(42, offset, true);
    central.push(new Uint8Array(c.buffer), name);
    offset += 30 + name.length + e.data.length;
  }
  const centralSize = central.reduce((n, p) => n + p.length, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true);
  end.setUint16(8, entries.length, true);
  end.setUint16(10, entries.length, true);
  end.setUint32(12, centralSize, true);
  end.setUint32(16, offset, true);
  const all = [...parts, ...central, new Uint8Array(end.buffer)];
  const out = new Uint8Array(all.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of all) { out.set(p, at); at += p.length; }
  return out;
}

test("zip entries: stored and deflate, directories skipped", async () => {
  const text = new TextEncoder().encode("hello hello hello hello mod");
  const zip = new Blob([buildZip([
    { name: "MyMod/", method: 0, data: new Uint8Array(0), size: 0 },
    { name: "MyMod/romfs/a.txt", method: 0, data: text, size: text.length },
    { name: "MyMod/exefs/0123456789ABCDEF.ips", method: 8, data: await deflateRaw(text), size: text.length },
  ])]);
  const entries = await zipEntries(zip);
  assert.deepEqual(entries.map((e) => e.path), ["MyMod/romfs/a.txt", "MyMod/exefs/0123456789ABCDEF.ips"]);
  const decoded: string[] = [];
  for await (const file of zipFiles(zip)) decoded.push(await file.data.text());
  assert.deepEqual(decoded, ["hello hello hello hello mod", "hello hello hello hello mod"]);
});

test("not a zip is an error", async () => {
  await assert.rejects(zipEntries(new Blob([new Uint8Array(100)])), /not a zip/);
});
