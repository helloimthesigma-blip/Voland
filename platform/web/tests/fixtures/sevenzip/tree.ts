/**
 * Deterministic contents of the 7z test fixtures (tests/unit/sevenzip.test.ts)
 * and a minimal 7z writer for hand-built archives.
 *
 * The committed .7z files were made from these contents with p7zip 17.05
 * and xz 5.8 (neither is needed to run the tests):
 *
 *   node --experimental-strip-types tests/fixtures/sevenzip/tree.ts write <tmp>
 *   cd <tmp>/tree && 7z a ../default.7z .           # LZMA2, solid, LZMA header
 *   7z a -m0=LZMA ../lzma.7z .
 *   7z a -mx0 ../copy.7z .
 *   7z a -mhc=off ../plainheader.7z .
 *   7z a -pSECRET -mhe=off ../encrypted.7z .
 *   cd <tmp>/big && 7z a ../big.7z . && 7z a -m0=LZMA ../big-lzma.7z .
 *   cd <tmp>/x86 && 7z a -mf=BCJ ../x86.7z .
 *   xz --format=raw --arm64 --lzma2=preset=6,dict=64KiB -c <tmp>/arm64/arm64.bin > <tmp>/arm64.raw
 *   node --experimental-strip-types tests/fixtures/sevenzip/tree.ts arm64 <tmp>/arm64.raw <tmp>/arm64.7z
 */
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { pathToFileURL } from "node:url";
import { crc32 } from "node:zlib";

/** A small deterministic generator (uint32 stream). */
export function prng(seed: number): () => number {
  let s = seed >>> 0 || 1;
  return () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5; s >>>= 0;
    return s;
  };
}

const text = (s: string): Uint8Array => new TextEncoder().encode(s);

/** The mod-like tree in default / lzma / copy / plainheader / encrypted.7z.
 * Empty directories are listed in `emptyDirectories`. */
export function treeFiles(): Map<string, Uint8Array> {
  const r = prng(1234);
  const files = new Map<string, Uint8Array>();
  files.set("readme.txt", text(Array.from({ length: 40 }, (_, i) => `Voland 7z fixture line ${i}\n`).join("")));
  files.set("romfs/Data/config.txt", text(Array.from({ length: 80 }, (_, i) => `key_${i} = ${r() % 1000}\n`).join("")));
  files.set("romfs/Data/sub/deep/value.bin", Uint8Array.from({ length: 3000 }, () => r() & 0xff));
  files.set("exefs/patch.ips", new Uint8Array([...text("PATCH"), ...Array.from({ length: 300 }, (_, i) => (i % 7 === 0 ? r() & 0xff : i & 0xff)), ...text("EOF")]));
  files.set("romfs/empty.dat", new Uint8Array(0));
  files.set("Über ✓ \u{1F3AE}.txt", text("unicode name\n"));
  return files;
}

export const emptyDirectories: readonly string[] = ["romfs/EmptyDir"];

/** big.bin: ~200 KB of varied word-built data (literals, matches, reps),
 * then repeats with sparse mutations past 2 MiB so LZMA2 needs more than
 * one chunk. Compresses to a few KB. */
export function bigFile(): Uint8Array {
  const r = prng(99);
  const words = Array.from({ length: 16 }, () => Uint8Array.from({ length: 4 + (r() % 21) }, () => 0x20 + (r() % 64)));
  const base: number[] = [];
  while (base.length < 200 * 1024) {
    const roll = r() % 64;
    if (roll === 0) base.push(r() & 0xff);
    else for (const b of words[r() % words.length]!) base.push(b);
  }
  const total = 2300 * 1024;
  const out = new Uint8Array(total);
  let at = 0;
  for (; at < base.length && at < total; at++) out[at] = base[at]!;
  while (at < total) {
    const n = Math.min(base.length, total - at);
    out.set(base.slice(0, n), at);
    for (let m = at + (r() % 4096); m < at + n; m += 2048 + (r() % 4096)) out[m] = r() & 0xff;
    at += n;
  }
  return out;
}

/** x86.bin: code-like bytes with E8/E9 calls to a few absolute targets
 * (what the BCJ filter rewrites). */
export function x86File(): Uint8Array {
  const r = prng(86);
  const targets = [0x1000, 0x2340, 0x8000, 0x10000, 0x4000, 0x6789];
  const filler = [0x55, 0x48, 0x89, 0xe5, 0x90, 0xc3, 0x31, 0xc0, 0x00, 0xff];
  const out: number[] = [];
  while (out.length < 16 * 1024) {
    const roll = r() % 8;
    if (roll < 3) {
      const rel = (targets[r() % targets.length]! - (out.length + 5)) | 0;
      out.push(roll === 0 ? 0xe9 : 0xe8, rel & 0xff, (rel >>> 8) & 0xff, (rel >>> 16) & 0xff, (rel >>> 24) & 0xff);
    } else if (roll === 3) {
      out.push(0xe8, r() & 0xff, filler[r() % filler.length]!);
    } else {
      out.push(filler[r() % filler.length]!);
    }
  }
  return Uint8Array.from(out);
}

/** arm64.bin: BL / ADRP instructions to a few absolute targets, plus NOPs. */
export function arm64File(): Uint8Array {
  const r = prng(64);
  const targets = [0x40000, 0x80000, 0x1000, 0x123450, 0x7ffff0, 0x20];
  const pages = [0x10, 0x200, 0x3, 0x7f, 0x1000, 0x40000];
  const count = 4096;
  const view = new DataView(new ArrayBuffer(count * 4));
  for (let i = 0; i < count; i++) {
    const pc = i * 4;
    const roll = r() % 4;
    let word = 0xd503201f;
    if (roll === 0) {
      word = (0x94000000 | (((targets[r() % targets.length]! - pc) >> 2) & 0x03ffffff)) >>> 0;
    } else if (roll === 1) {
      const imm = (pages[r() % pages.length]! - (pc >>> 12)) & 0x1fffff;
      word = (0x90000000 | ((imm & 3) << 29) | ((imm >>> 2) << 5) | (r() % 31)) >>> 0;
    } else if (roll === 2) {
      word = 0xaa0103e0;
    }
    view.setUint32(pc, word, true);
  }
  return new Uint8Array(view.buffer);
}

/* ------------------------------------------------------------------ */
/* A minimal 7z writer: one folder with a linear coder chain, files in */
/* one solid stream, plain (uncompressed) header.                       */
/* ------------------------------------------------------------------ */

export interface Coder {
  readonly id: readonly number[];
  readonly props?: readonly number[];
}

function number7z(value: number): number[] {
  if (value < 0x80) return [value];
  if (value < 0x4000) return [0x80 | (value >>> 8), value & 0xff];
  if (value < 0x200000) return [0xc0 | (value >>> 16), value & 0xff, (value >>> 8) & 0xff];
  const out = [0xff];
  let v = BigInt(value);
  for (let i = 0; i < 8; i++) { out.push(Number(v & 0xffn)); v >>= 8n; }
  return out;
}

function utf16z(s: string): number[] {
  const out: number[] = [];
  for (let i = 0; i < s.length; i++) { const c = s.charCodeAt(i); out.push(c & 0xff, c >>> 8); }
  out.push(0, 0);
  return out;
}

/** An archive whose single folder decodes `packed` through `coders`
 * (coders[0] produces the final output, as 7-Zip orders them) into the
 * concatenation of `files`. `unpackSizes` gives each coder's output size. */
export function build7z(packed: Uint8Array, coders: readonly Coder[], unpackSizes: readonly number[],
                        files: readonly { name: string; size: number }[]): Uint8Array {
  const h: number[] = [0x01, 0x04];
  h.push(0x06, ...number7z(0), ...number7z(1), 0x09, ...number7z(packed.length), 0x00);
  h.push(0x07, 0x0b, ...number7z(1), 0x00, ...number7z(coders.length));
  for (const c of coders) {
    const props = c.props ?? [];
    h.push(c.id.length | (props.length > 0 ? 0x20 : 0), ...c.id);
    if (props.length > 0) h.push(...number7z(props.length), ...props);
  }
  for (let i = 0; i + 1 < coders.length; i++) h.push(...number7z(i), ...number7z(i + 1));
  h.push(0x0c);
  for (const s of unpackSizes) h.push(...number7z(s));
  h.push(0x00);
  h.push(0x08, 0x0d, ...number7z(files.length));
  if (files.length > 1) {
    h.push(0x09);
    for (const f of files.slice(0, -1)) h.push(...number7z(f.size));
  }
  h.push(0x00, 0x00);
  const names = files.flatMap((f) => utf16z(f.name));
  h.push(0x05, ...number7z(files.length), 0x11, ...number7z(names.length + 1), 0x00, ...names, 0x00, 0x00);
  const header = Uint8Array.from(h);
  const start = new DataView(new ArrayBuffer(32));
  [0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c, 0x00, 0x04].forEach((b, i) => start.setUint8(i, b));
  start.setBigUint64(12, BigInt(packed.length), true);
  start.setBigUint64(20, BigInt(header.length), true);
  start.setUint32(28, crc32(header), true);
  start.setUint32(8, crc32(new Uint8Array(start.buffer, 12, 20)), true);
  const out = new Uint8Array(32 + packed.length + header.length);
  out.set(new Uint8Array(start.buffer), 0);
  out.set(packed, 32);
  out.set(header, 32 + packed.length);
  return out;
}

function writeTree(root: string, files: Map<string, Uint8Array>, dirs: readonly string[] = []): void {
  for (const [path, data] of files) {
    mkdirSync(dirname(join(root, path)), { recursive: true });
    writeFileSync(join(root, path), data);
  }
  for (const d of dirs) mkdirSync(join(root, d), { recursive: true });
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [command, a, b] = process.argv.slice(2);
  if (command === "write" && a) {
    writeTree(join(a, "tree"), treeFiles(), emptyDirectories);
    writeTree(join(a, "big"), new Map([["big.bin", bigFile()]]));
    writeTree(join(a, "x86"), new Map([["x86.bin", x86File()]]));
    writeTree(join(a, "arm64"), new Map([["arm64.bin", arm64File()]]));
  } else if (command === "arm64" && a && b) {
    const size = arm64File().length;
    /* LZMA2 property 8 = 64 KiB dictionary (matches the xz command). */
    writeFileSync(b, build7z(readFileSync(a), [{ id: [0x0a] }, { id: [0x21], props: [8] }], [size, size],
                             [{ name: "arm64.bin", size }]));
  } else {
    console.error("usage: tree.ts write <dir> | arm64 <raw> <out.7z>");
    process.exit(2);
  }
}
