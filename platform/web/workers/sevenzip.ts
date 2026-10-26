/**
 * 7z archive reader for mod installs (§19), written from the public 7z
 * format description (7zFormat.txt) and the LZMA / LZMA2 specifications.
 *
 * Supported: plain and LZMA-compressed ("encoded") headers, solid folders
 * holding many files, empty files and directories, the Copy, LZMA and
 * LZMA2 coders, and the BCJ x86 and ARM64 branch filters. Encrypted
 * archives and other coders (PPMd, BZip2, Deflate, BCJ2, ...) are
 * rejected with a clear error.
 *
 * Folders are decoded sequentially; each file is emitted as a Blob built
 * from the decoder's output chunks, so a large solid folder is never held
 * as one array. Only the LZMA dictionary (capped at the folder size) and
 * the current file's chunks are in memory.
 */
import type { ModFileSource } from "./mod-store.ts";

const SIGNATURE = [0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c];
const SIGNATURE_HEADER_BYTES = 32;
const READ_CHUNK_BYTES = 1 << 20;
const STEP_OUTPUT_BYTES = 1 << 20;

/* Property ids (7zFormat.txt). */
const K_END = 0x00;
const K_HEADER = 0x01;
const K_ARCHIVE_PROPERTIES = 0x02;
const K_ADDITIONAL_STREAMS_INFO = 0x03;
const K_MAIN_STREAMS_INFO = 0x04;
const K_FILES_INFO = 0x05;
const K_PACK_INFO = 0x06;
const K_UNPACK_INFO = 0x07;
const K_SUBSTREAMS_INFO = 0x08;
const K_SIZE = 0x09;
const K_CRC = 0x0a;
const K_FOLDER = 0x0b;
const K_CODERS_UNPACK_SIZE = 0x0c;
const K_NUM_UNPACK_STREAM = 0x0d;
const K_EMPTY_STREAM = 0x0e;
const K_EMPTY_FILE = 0x0f;
const K_ANTI = 0x10;
const K_NAME = 0x11;
const K_ENCODED_HEADER = 0x17;

/* Coder ids, as hex strings of the id bytes. */
const METHOD_COPY = "00";
const METHOD_LZMA = "030101";
const METHOD_LZMA2 = "21";
const METHOD_BCJ_X86 = "03030103";
const METHOD_ARM64 = "0a";
const METHOD_AES = "06f10701";
const METHOD_NAMES: Readonly<Record<string, string>> = {
  "03": "Delta", "0303011b": "BCJ2", "03030205": "PPC", "03030401": "IA64", "03030501": "ARM",
  "03030701": "ARMT", "03030805": "SPARC", "0b": "RISCV", "030401": "PPMd", "040108": "Deflate",
  "040109": "Deflate64", "040202": "BZip2", "04f71101": "Zstandard",
};

const ENCRYPTED_MESSAGE = "encrypted 7z archives are not supported";

/* ------------------------------------------------------------------ */
/* CRC-32 (IEEE, reflected), for the header and per-file checks.       */
/* ------------------------------------------------------------------ */

const CRC_TABLE = ((): Uint32Array => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

/** Continues a CRC-32 (`crc` is the running value, start with 0). */
function crc32(crc: number, bytes: Uint8Array): number {
  let c = ~crc >>> 0;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]!) & 0xff]! ^ (c >>> 8);
  return ~c >>> 0;
}

/* ------------------------------------------------------------------ */
/* Header parsing.                                                      */
/* ------------------------------------------------------------------ */

class HeaderReader {
  private pos = 0;
  private readonly bytes: Uint8Array;
  constructor(bytes: Uint8Array) {
    this.bytes = bytes;
  }

  byte(): number {
    if (this.pos >= this.bytes.length) throw new Error("damaged 7z header (truncated)");
    return this.bytes[this.pos++]!;
  }

  /** 7z NUMBER: the count of leading 1 bits in the first byte is the
   * count of extra little-endian bytes; the rest of the first byte holds
   * the high bits. */
  number(): number {
    const first = this.byte();
    let mask = 0x80;
    let value = 0;
    for (let i = 0; i < 8; i++) {
      if ((first & mask) === 0) return value + (first & (mask - 1)) * 2 ** (8 * i);
      value += this.byte() * 2 ** (8 * i);
      mask >>>= 1;
    }
    if (value > Number.MAX_SAFE_INTEGER) throw new Error("damaged 7z header (number too large)");
    return value;
  }

  uint32(): number {
    const v = this.byte() | (this.byte() << 8) | (this.byte() << 16) | (this.byte() << 24);
    return v >>> 0;
  }

  take(n: number): Uint8Array {
    if (n < 0 || this.pos + n > this.bytes.length) throw new Error("damaged 7z header (truncated)");
    const out = this.bytes.subarray(this.pos, this.pos + n);
    this.pos += n;
    return out;
  }

  skip(n: number): void {
    this.take(n);
  }

  /** A bit vector of `n` items, most significant bit first. */
  bits(n: number): boolean[] {
    const out: boolean[] = [];
    let current = 0;
    for (let i = 0; i < n; i++) {
      if (i % 8 === 0) current = this.byte();
      out.push((current & (0x80 >>> (i % 8))) !== 0);
    }
    return out;
  }

  /** An "all defined" byte, then a bit vector when it is 0. */
  definedBits(n: number): boolean[] {
    return this.byte() !== 0 ? new Array<boolean>(n).fill(true) : this.bits(n);
  }

  digests(n: number): (number | null)[] {
    const defined = this.definedBits(n);
    return defined.map((d) => (d ? this.uint32() : null));
  }
}

interface CoderInfo {
  readonly method: string;
  readonly inStreams: number;
  readonly outStreams: number;
  readonly props: Uint8Array;
}

interface FolderInfo {
  readonly coders: readonly CoderInfo[];
  readonly bindPairs: readonly { readonly inIndex: number; readonly outIndex: number }[];
  readonly packedStreams: readonly number[];
  unpackSizes: number[];
  unpackCrc: number | null;
}

interface StreamsInfo {
  readonly packPos: number;
  readonly packSizes: readonly number[];
  readonly folders: readonly FolderInfo[];
  /** Per folder: the sizes and CRCs of its substreams (files). */
  readonly substreams: readonly { readonly sizes: readonly number[]; readonly crcs: readonly (number | null)[] }[];
}

interface FileEntry {
  readonly path: string;
  readonly hasStream: boolean;
  readonly isDirectory: boolean;
  readonly isAnti: boolean;
}

function methodName(method: string): string {
  return METHOD_NAMES[method] ?? `method ${method}`;
}

function readFolder(r: HeaderReader): FolderInfo {
  const coderCount = r.number();
  const coders: CoderInfo[] = [];
  let totalIn = 0;
  let totalOut = 0;
  for (let i = 0; i < coderCount; i++) {
    const flags = r.byte();
    if (flags & 0x80) throw new Error("unsupported 7z archive (alternative coder methods)");
    const id = r.take(flags & 0x0f);
    const method = Array.from(id, (b) => b.toString(16).padStart(2, "0")).join("");
    const complex = (flags & 0x10) !== 0;
    const inStreams = complex ? r.number() : 1;
    const outStreams = complex ? r.number() : 1;
    const props = flags & 0x20 ? r.take(r.number()) : new Uint8Array(0);
    coders.push({ method, inStreams, outStreams, props });
    totalIn += inStreams;
    totalOut += outStreams;
  }
  const bindPairs: { inIndex: number; outIndex: number }[] = [];
  for (let i = 0; i < totalOut - 1; i++) bindPairs.push({ inIndex: r.number(), outIndex: r.number() });
  const packedCount = totalIn - bindPairs.length;
  const packedStreams: number[] = [];
  if (packedCount === 1) {
    for (let i = 0; i < totalIn; i++) {
      if (!bindPairs.some((p) => p.inIndex === i)) { packedStreams.push(i); break; }
    }
  } else {
    for (let i = 0; i < packedCount; i++) packedStreams.push(r.number());
  }
  return { coders, bindPairs, packedStreams, unpackSizes: [], unpackCrc: null };
}

function readStreamsInfo(r: HeaderReader): StreamsInfo {
  let packPos = 0;
  let packSizes: number[] = [];
  let folders: FolderInfo[] = [];
  let substreams: { sizes: number[]; crcs: (number | null)[] }[] | null = null;
  for (;;) {
    const id = r.byte();
    if (id === K_END) break;
    if (id === K_PACK_INFO) {
      packPos = r.number();
      const count = r.number();
      for (;;) {
        const t = r.byte();
        if (t === K_END) break;
        if (t === K_SIZE) packSizes = Array.from({ length: count }, () => r.number());
        else if (t === K_CRC) r.digests(count);
        else r.skip(r.number());
      }
    } else if (id === K_UNPACK_INFO) {
      if (r.byte() !== K_FOLDER) throw new Error("damaged 7z header (folders)");
      const count = r.number();
      if (r.byte() !== 0) throw new Error("unsupported 7z archive (external folders)");
      folders = Array.from({ length: count }, () => readFolder(r));
      if (r.byte() !== K_CODERS_UNPACK_SIZE) throw new Error("damaged 7z header (unpack sizes)");
      for (const f of folders) {
        const outs = f.coders.reduce((n, c) => n + c.outStreams, 0);
        f.unpackSizes = Array.from({ length: outs }, () => r.number());
      }
      for (;;) {
        const t = r.byte();
        if (t === K_END) break;
        if (t === K_CRC) r.digests(folders.length).forEach((crc, i) => { folders[i]!.unpackCrc = crc; });
        else r.skip(r.number());
      }
    } else if (id === K_SUBSTREAMS_INFO) {
      const counts = folders.map(() => 1);
      let t = r.byte();
      if (t === K_NUM_UNPACK_STREAM) {
        for (let i = 0; i < folders.length; i++) counts[i] = r.number();
        t = r.byte();
      }
      const sizes: number[][] = folders.map((f, i) => {
        const total = folderSize(f);
        return counts[i] === 1 ? [total] : [];
      });
      const hasSizes = t === K_SIZE;
      for (let i = 0; i < folders.length; i++) {
        const n = counts[i]!;
        if (n <= 1) continue;
        const list: number[] = [];
        let sum = 0;
        if (hasSizes) {
          for (let k = 0; k < n - 1; k++) { const s = r.number(); list.push(s); sum += s; }
        }
        const total = folderSize(folders[i]!);
        if (sum > total) throw new Error("damaged 7z header (substream sizes)");
        list.push(total - sum);
        sizes[i] = list;
      }
      if (hasSizes) t = r.byte();
      const crcs: (number | null)[][] = folders.map((f, i) => (counts[i] === 1 && f.unpackCrc !== null ? [f.unpackCrc] : []));
      for (; t !== K_END; t = r.byte()) {
        if (t === K_CRC) {
          const unknown = folders.reduce((n, f, i) => n + (counts[i] === 1 && f.unpackCrc !== null ? 0 : counts[i]!), 0);
          const digests = r.digests(unknown);
          let d = 0;
          folders.forEach((f, i) => {
            if (counts[i] === 1 && f.unpackCrc !== null) return;
            crcs[i] = Array.from({ length: counts[i]! }, () => digests[d++] ?? null);
          });
        } else {
          r.skip(r.number());
        }
      }
      substreams = folders.map((_, i) => ({
        sizes: sizes[i] ?? [],
        crcs: crcs[i]!.length > 0 ? crcs[i]! : new Array<number | null>(counts[i]!).fill(null),
      }));
    } else {
      throw new Error(`damaged 7z header (unexpected property ${id})`);
    }
  }
  return {
    packPos, packSizes, folders,
    substreams: substreams ?? folders.map((f) => ({ sizes: [folderSize(f)], crcs: [f.unpackCrc] })),
  };
}

/** The folder's final output size: the output stream no bind pair uses. */
function folderSize(f: FolderInfo): number {
  for (let i = 0; i < f.unpackSizes.length; i++) {
    if (!f.bindPairs.some((p) => p.outIndex === i)) return f.unpackSizes[i]!;
  }
  throw new Error("damaged 7z header (folder has no output)");
}

function readFilesInfo(r: HeaderReader): FileEntry[] {
  const count = r.number();
  let emptyStream: boolean[] = new Array<boolean>(count).fill(false);
  let emptyFile: boolean[] = [];
  let anti: boolean[] = [];
  let names: string[] = [];
  for (;;) {
    const id = r.byte();
    if (id === K_END) break;
    const size = r.number();
    const data = new HeaderReader(r.take(size));
    const emptyCount = emptyStream.filter(Boolean).length;
    if (id === K_EMPTY_STREAM) emptyStream = data.bits(count);
    else if (id === K_EMPTY_FILE) emptyFile = data.bits(emptyCount);
    else if (id === K_ANTI) anti = data.bits(emptyCount);
    else if (id === K_NAME) {
      if (data.byte() !== 0) throw new Error("unsupported 7z archive (external file names)");
      const raw = data.take(size - 1);
      const text = new TextDecoder("utf-16le").decode(raw);
      names = text.split("\0").slice(0, count);
    }
    /* Times, attributes, kDummy padding and anything newer: skipped. */
  }
  const files: FileEntry[] = [];
  let e = 0;
  for (let i = 0; i < count; i++) {
    const path = (names[i] ?? "").replace(/\\/g, "/");
    if (!emptyStream[i]) {
      files.push({ path, hasStream: true, isDirectory: false, isAnti: false });
      continue;
    }
    files.push({ path, hasStream: false, isDirectory: !(emptyFile[e] ?? false), isAnti: anti[e] ?? false });
    e++;
  }
  return files;
}

/* ------------------------------------------------------------------ */
/* Byte streams.                                                        */
/* ------------------------------------------------------------------ */

type Chunk = Uint8Array<ArrayBuffer>;

interface ByteStream {
  /** The next output chunk, or null at the end. */
  read(): Promise<Chunk | null>;
}

/** A byte range of the archive, read in `chunkBytes` pieces. */
class PackedStream implements ByteStream {
  private pos: number;
  private readonly blob: Blob;
  private readonly end: number;
  private readonly chunkBytes: number;
  constructor(blob: Blob, start: number, end: number, chunkBytes: number) {
    this.blob = blob;
    this.end = end;
    this.chunkBytes = chunkBytes;
    if (end > blob.size) throw new Error("damaged 7z archive (truncated)");
    this.pos = start;
  }

  async read(): Promise<Chunk | null> {
    if (this.pos >= this.end) return null;
    const next = Math.min(this.end, this.pos + this.chunkBytes);
    const bytes = new Uint8Array(await this.blob.slice(this.pos, next).arrayBuffer());
    this.pos = next;
    return bytes;
  }
}

/** Buffered input for the LZMA decoders: keeps at least `need` bytes
 * past `pos` available unless the source has ended. */
class InputBuffer {
  buf: Chunk = new Uint8Array(0);
  pos = 0;
  eof = false;
  private readonly source: ByteStream;
  constructor(source: ByteStream) {
    this.source = source;
  }

  get available(): number {
    return this.buf.length - this.pos;
  }

  async fill(need: number): Promise<void> {
    while (this.available < need && !this.eof) {
      const chunk = await this.source.read();
      if (!chunk) { this.eof = true; break; }
      const merged = new Uint8Array(this.available + chunk.length);
      merged.set(this.buf.subarray(this.pos), 0);
      merged.set(chunk, this.available);
      this.buf = merged;
      this.pos = 0;
    }
  }
}

/* ------------------------------------------------------------------ */
/* LZMA.                                                                */
/* ------------------------------------------------------------------ */

const NUM_STATES = 12;
const POS_STATES_MAX = 16;
const LEN_PROBS = 2 + 2 * POS_STATES_MAX * 8 + 256;
const P_IS_MATCH = 0;
const P_IS_REP = P_IS_MATCH + NUM_STATES * POS_STATES_MAX;
const P_IS_REP_G0 = P_IS_REP + NUM_STATES;
const P_IS_REP_G1 = P_IS_REP_G0 + NUM_STATES;
const P_IS_REP_G2 = P_IS_REP_G1 + NUM_STATES;
const P_IS_REP0_LONG = P_IS_REP_G2 + NUM_STATES;
const P_POS_SLOT = P_IS_REP0_LONG + NUM_STATES * POS_STATES_MAX;
const P_SPEC_POS = P_POS_SLOT + 4 * 64;
const P_ALIGN = P_SPEC_POS + 115;
const P_LEN = P_ALIGN + 16;
const P_REP_LEN = P_LEN + LEN_PROBS;
const P_LITERAL = P_REP_LEN + LEN_PROBS;
const PROB_INIT = 1024;
const TOP = 0x1000000;
const MATCH_MIN_LEN = 2;
const END_POS_MODEL_INDEX = 14;
const LZMA_MARGIN = 64;
const LZMA_LOOKAHEAD = 1 << 16;

/** The sliding dictionary; newly written bytes are copied out in chunks. */
class Window {
  private readonly buf: Uint8Array;
  private pos = 0;
  private flushed = 0;
  /** Bytes since the last dictionary reset (positions and distances). */
  total = 0;
  /** Bytes ever written. */
  written = 0;
  readonly out: Chunk[] = [];

  constructor(size: number) {
    this.buf = new Uint8Array(Math.max(1, size));
  }

  reset(): void {
    this.total = 0;
  }

  /** The byte `back` positions behind (1 = the last byte). */
  get(back: number): number {
    let i = this.pos - back;
    if (i < 0) i += this.buf.length;
    return this.buf[i]!;
  }

  canReach(back: number): boolean {
    return back <= this.total && back <= this.buf.length;
  }

  put(b: number): void {
    this.buf[this.pos++] = b;
    this.total++;
    this.written++;
    if (this.pos === this.buf.length) this.wrap();
  }

  copy(back: number, len: number): void {
    const size = this.buf.length;
    let src = this.pos - back;
    if (src < 0) src += size;
    if (back >= len && src + len <= size && this.pos + len < size) {
      this.buf.copyWithin(this.pos, src, src + len);
      this.pos += len;
      this.total += len;
      this.written += len;
      return;
    }
    for (let i = 0; i < len; i++) {
      this.buf[this.pos++] = this.buf[src++]!;
      if (src === size) src = 0;
      this.total++;
      this.written++;
      if (this.pos === size) this.wrap();
    }
  }

  putBytes(bytes: Uint8Array): void {
    for (let i = 0; i < bytes.length;) {
      const n = Math.min(bytes.length - i, this.buf.length - this.pos);
      this.buf.set(bytes.subarray(i, i + n), this.pos);
      this.pos += n;
      this.total += n;
      this.written += n;
      i += n;
      if (this.pos === this.buf.length) this.wrap();
    }
  }

  private wrap(): void {
    this.flush();
    this.pos = 0;
    this.flushed = 0;
  }

  flush(): void {
    if (this.pos > this.flushed) this.out.push(this.buf.slice(this.flushed, this.pos));
    this.flushed = this.pos;
  }
}

/** The LZMA decoder proper: range decoder, probability model and state.
 * Input is a byte array the caller points at; reads past its end yield 0. */
class LzmaDecoder {
  private probs: Uint16Array = new Uint16Array(0);
  private lc = 0;
  private lp = 0;
  private pb = 0;
  private state = 0;
  private rep0 = 0;
  private rep1 = 0;
  private rep2 = 0;
  private rep3 = 0;
  private range = 0;
  private code = 0;
  input: Uint8Array = new Uint8Array(0);
  inPos = 0;
  ended = false;

  readonly window: Window;
  constructor(window: Window) {
    this.window = window;
  }

  setProperties(propsByte: number): void {
    if (propsByte >= 9 * 5 * 5) throw new Error("damaged 7z archive (bad LZMA properties)");
    this.lc = propsByte % 9;
    this.lp = Math.floor(propsByte / 9) % 5;
    this.pb = Math.floor(propsByte / 45);
    this.probs = new Uint16Array(P_LITERAL + (0x300 << (this.lc + this.lp)));
  }

  resetState(): void {
    this.probs.fill(PROB_INIT);
    this.state = 0;
    this.rep0 = this.rep1 = this.rep2 = this.rep3 = 0;
  }

  /** Starts the range decoder on the 5 bytes at `inPos`. */
  initRange(): void {
    if (this.nextByte() !== 0) throw new Error("damaged 7z archive (bad LZMA stream)");
    this.code = 0;
    for (let i = 0; i < 4; i++) this.code = (this.code << 8) | this.nextByte();
    this.range = -1;
    if (this.code === -1) throw new Error("damaged 7z archive (bad LZMA stream)");
  }

  private nextByte(): number {
    const b = this.inPos < this.input.length ? this.input[this.inPos]! : 0;
    this.inPos++;
    return b;
  }

  /* range and code are kept as int32 bit patterns (unsigned compares via
   * >>> 0) so the hot loop stays in small-integer arithmetic. */
  private bit(i: number): number {
    const p = this.probs[i]!;
    const bound = ((this.range >>> 11) * p) | 0;
    let b: number;
    if ((this.code >>> 0) < (bound >>> 0)) {
      this.range = bound;
      this.probs[i] = p + ((2048 - p) >>> 5);
      b = 0;
    } else {
      this.range = (this.range - bound) | 0;
      this.code = (this.code - bound) | 0;
      this.probs[i] = p - (p >>> 5);
      b = 1;
    }
    if ((this.range >>> 0) < TOP) {
      this.range <<= 8;
      this.code = (this.code << 8) | this.nextByte();
    }
    return b;
  }

  private direct(count: number): number {
    let result = 0;
    for (let i = 0; i < count; i++) {
      this.range = this.range >>> 1;
      let b = 0;
      if ((this.code >>> 0) >= this.range) { this.code = (this.code - this.range) | 0; b = 1; }
      result = result * 2 + b;
      if (this.range < TOP) {
        this.range <<= 8;
        this.code = (this.code << 8) | this.nextByte();
      }
    }
    return result;
  }

  private tree(base: number, bits: number): number {
    let m = 1;
    for (let i = 0; i < bits; i++) m = (m << 1) | this.bit(base + m);
    return m - (1 << bits);
  }

  private reverseTree(base: number, bits: number): number {
    let m = 1;
    let symbol = 0;
    for (let i = 0; i < bits; i++) {
      const b = this.bit(base + m);
      m = (m << 1) | b;
      symbol |= b << i;
    }
    return symbol;
  }

  private length(base: number, posState: number): number {
    if (this.bit(base) === 0) return this.tree(base + 2 + posState * 8, 3);
    if (this.bit(base + 1) === 0) return 8 + this.tree(base + 2 + POS_STATES_MAX * 8 + posState * 8, 3);
    return 16 + this.tree(base + 2 + 2 * POS_STATES_MAX * 8, 8);
  }

  private distance(len: number): number {
    const lenState = Math.min(len, 3);
    const slot = this.tree(P_POS_SLOT + lenState * 64, 6);
    if (slot < 4) return slot;
    const directBits = (slot >>> 1) - 1;
    let dist = (2 | (slot & 1)) * 2 ** directBits;
    if (slot < END_POS_MODEL_INDEX) return dist + this.reverseTree(P_SPEC_POS + dist - slot, directBits);
    dist += this.direct(directBits - 4) * 16;
    return dist + this.reverseTree(P_ALIGN, 4);
  }

  /** Decodes symbols until `window.written` reaches `stopAt` (checked
   * between symbols), the input position reaches `inLimit`, or the end
   * marker. Matches are cut at `hardEnd`. */
  run(stopAt: number, hardEnd: number, inLimit: number): void {
    const w = this.window;
    const pbMask = (1 << this.pb) - 1;
    const lpMask = (1 << this.lp) - 1;
    while (w.written < stopAt && this.inPos < inLimit && !this.ended) {
      const posState = w.total & pbMask;
      const s = this.state;
      if (this.bit(P_IS_MATCH + s * POS_STATES_MAX + posState) === 0) {
        const prev = w.total > 0 ? w.get(1) : 0;
        const base = P_LITERAL + 0x300 * (((w.total & lpMask) << this.lc) + (prev >>> (8 - this.lc)));
        let symbol = 1;
        if (s >= 7) {
          if (!w.canReach(this.rep0 + 1)) throw new Error("damaged 7z archive (bad LZMA distance)");
          let matchByte = w.get(this.rep0 + 1);
          while (symbol < 0x100) {
            const matchBit = (matchByte >>> 7) & 1;
            matchByte <<= 1;
            const b = this.bit(base + 0x100 + (matchBit << 8) + symbol);
            symbol = (symbol << 1) | b;
            if (matchBit !== b) break;
          }
        }
        while (symbol < 0x100) symbol = (symbol << 1) | this.bit(base + symbol);
        w.put(symbol & 0xff);
        this.state = s < 4 ? 0 : s < 10 ? s - 3 : s - 6;
        continue;
      }
      let len: number;
      if (this.bit(P_IS_REP + s) === 0) {
        len = this.length(P_LEN, posState);
        this.state = s < 7 ? 7 : 10;
        const dist = this.distance(len);
        if (dist === 0xffffffff) { this.ended = true; break; }
        this.rep3 = this.rep2;
        this.rep2 = this.rep1;
        this.rep1 = this.rep0;
        this.rep0 = dist;
      } else {
        if (this.bit(P_IS_REP_G0 + s) === 0) {
          if (this.bit(P_IS_REP0_LONG + s * POS_STATES_MAX + posState) === 0) {
            if (!w.canReach(this.rep0 + 1)) throw new Error("damaged 7z archive (bad LZMA distance)");
            this.state = s < 7 ? 9 : 11;
            w.put(w.get(this.rep0 + 1));
            continue;
          }
        } else {
          let dist: number;
          if (this.bit(P_IS_REP_G1 + s) === 0) {
            dist = this.rep1;
          } else {
            if (this.bit(P_IS_REP_G2 + s) === 0) {
              dist = this.rep2;
            } else {
              dist = this.rep3;
              this.rep3 = this.rep2;
            }
            this.rep2 = this.rep1;
          }
          this.rep1 = this.rep0;
          this.rep0 = dist;
        }
        len = this.length(P_REP_LEN, posState);
        this.state = s < 7 ? 8 : 11;
      }
      if (!w.canReach(this.rep0 + 1)) throw new Error("damaged 7z archive (bad LZMA distance)");
      const n = Math.min(len + MATCH_MIN_LEN, hardEnd - w.written);
      w.copy(this.rep0 + 1, n);
    }
  }
}

function dictionaryCap(dict: number, outSize: number): number {
  return Math.max(1, Math.min(dict, outSize));
}

/** A 7z LZMA coder: 5 property bytes (lc/lp/pb, dictionary size). */
class LzmaStream implements ByteStream {
  private readonly input: InputBuffer;
  private readonly decoder: LzmaDecoder;
  private started = false;

  private readonly outSize: number;
  constructor(source: ByteStream, props: Uint8Array, outSize: number) {
    this.outSize = outSize;
    if (props.length < 5) throw new Error("damaged 7z archive (LZMA properties)");
    const dict = (props[1]! | (props[2]! << 8) | (props[3]! << 16) | (props[4]! << 24)) >>> 0;
    this.input = new InputBuffer(source);
    this.decoder = new LzmaDecoder(new Window(dictionaryCap(dict, outSize)));
    this.decoder.setProperties(props[0]!);
    this.decoder.resetState();
  }

  async read(): Promise<Chunk | null> {
    const d = this.decoder;
    const w = d.window;
    while (w.out.length === 0) {
      if (w.written >= this.outSize || d.ended) return null;
      if (this.input.available < LZMA_LOOKAHEAD) await this.input.fill(LZMA_LOOKAHEAD);
      d.input = this.input.buf;
      d.inPos = this.input.pos;
      if (!this.started) {
        d.initRange();
        this.started = true;
      }
      const inLimit = this.input.eof ? Infinity : this.input.buf.length - LZMA_MARGIN;
      const before = w.written;
      d.run(Math.min(this.outSize, w.written + STEP_OUTPUT_BYTES), this.outSize, inLimit);
      this.input.pos = Math.min(d.inPos, this.input.buf.length);
      w.flush();
      if (w.written === before && this.input.eof && !d.ended) throw new Error("damaged 7z archive (LZMA data ends early)");
    }
    return w.out.shift() ?? null;
  }
}

/** LZMA2 dictionary size from its one property byte. */
function lzma2Dictionary(p: number): number {
  if (p > 40) throw new Error("damaged 7z archive (LZMA2 properties)");
  if (p === 40) return 0xffffffff;
  return (2 | (p & 1)) * 2 ** (Math.floor(p / 2) + 11);
}

/** A 7z LZMA2 coder: a sequence of LZMA and uncompressed chunks. */
class Lzma2Stream implements ByteStream {
  private readonly input: InputBuffer;
  private readonly decoder: LzmaDecoder;
  private done = false;
  private haveProps = false;

  private readonly outSize: number;
  constructor(source: ByteStream, props: Uint8Array, outSize: number) {
    this.outSize = outSize;
    this.input = new InputBuffer(source);
    this.decoder = new LzmaDecoder(new Window(dictionaryCap(lzma2Dictionary(props[0] ?? 0), outSize)));
  }

  async read(): Promise<Chunk | null> {
    const w = this.decoder.window;
    while (w.out.length === 0) {
      if (this.done) return null;
      await this.chunk();
      w.flush();
    }
    return w.out.shift() ?? null;
  }

  private async chunk(): Promise<void> {
    const input = this.input;
    const d = this.decoder;
    const w = d.window;
    await input.fill(6);
    if (input.available < 1) {
      this.done = true;
      return;
    }
    const b = input.buf;
    const control = b[input.pos]!;
    if (control === 0x00) {
      this.done = true;
      return;
    }
    if (control === 0x01 || control === 0x02) {
      if (input.available < 3) throw new Error("damaged 7z archive (LZMA2 data ends early)");
      const size = ((b[input.pos + 1]! << 8) | b[input.pos + 2]!) + 1;
      input.pos += 3;
      await input.fill(size);
      if (input.available < size) throw new Error("damaged 7z archive (LZMA2 data ends early)");
      if (control === 0x01) w.reset();
      w.putBytes(input.buf.subarray(input.pos, input.pos + size));
      input.pos += size;
      return;
    }
    if (control < 0x80) throw new Error("damaged 7z archive (bad LZMA2 chunk)");
    const reset = (control >>> 5) & 3;
    const headerBytes = reset >= 2 ? 6 : 5;
    if (input.available < headerBytes) throw new Error("damaged 7z archive (LZMA2 data ends early)");
    const unpacked = (control & 0x1f) * 65536 + ((b[input.pos + 1]! << 8) | b[input.pos + 2]!) + 1;
    const packed = ((b[input.pos + 3]! << 8) | b[input.pos + 4]!) + 1;
    if (reset === 3) w.reset();
    if (reset >= 2) {
      const props = b[input.pos + 5]!;
      if ((props % 9) + (Math.floor(props / 9) % 5) > 4) throw new Error("damaged 7z archive (LZMA2 properties)");
      d.setProperties(props);
      this.haveProps = true;
    }
    if (!this.haveProps) throw new Error("damaged 7z archive (LZMA2 chunk before properties)");
    if (reset >= 1) d.resetState();
    input.pos += headerBytes;
    await input.fill(packed);
    if (input.available < packed) throw new Error("damaged 7z archive (LZMA2 data ends early)");
    const end = input.pos + packed;
    d.input = input.buf.subarray(0, end);
    d.inPos = input.pos;
    d.initRange();
    const target = Math.min(w.written + unpacked, this.outSize);
    d.run(target, target, Infinity);
    if (w.written !== target) throw new Error("damaged 7z archive (bad LZMA2 chunk)");
    input.pos = end;
  }
}

/** The x86 BCJ filter (decoding): E8/E9 call and jump targets were
 * stored absolute; turn them back into relative ones. */
class X86Stream implements ByteStream {
  private carry: Chunk = new Uint8Array(0);
  private ip = 0;
  private prevMask = 0;
  private prevPos = -5;
  private done = false;

  private readonly source: ByteStream;
  constructor(source: ByteStream) {
    this.source = source;
  }

  async read(): Promise<Chunk | null> {
    for (;;) {
      if (this.done) return null;
      const chunk = await this.source.read();
      if (!chunk) {
        this.done = true;
        return this.carry.length > 0 ? this.carry : null;
      }
      const buf = new Uint8Array(this.carry.length + chunk.length);
      buf.set(this.carry, 0);
      buf.set(chunk, this.carry.length);
      const n = this.convert(buf);
      this.carry = buf.slice(n);
      if (n > 0) return buf.subarray(0, n);
    }
  }

  /** Converts in place; returns how many leading bytes are final. */
  private convert(buf: Uint8Array): number {
    const allowed = [true, true, true, false, true, false, false, false];
    const bitNumber = [0, 1, 2, 2, 3, 3, 3, 3];
    const msByte = (b: number): boolean => b === 0 || b === 0xff;
    if (buf.length < 5) return 0;
    const ip = this.ip;
    if (ip - this.prevPos > 5) this.prevPos = ip - 5;
    const limit = buf.length - 5;
    let i = 0;
    while (i <= limit) {
      const op = buf[i]!;
      if (op !== 0xe8 && op !== 0xe9) { i++; continue; }
      const offset = ip + i - this.prevPos;
      this.prevPos = ip + i;
      if (offset > 5) {
        this.prevMask = 0;
      } else {
        for (let k = 0; k < offset; k++) { this.prevMask &= 0x77; this.prevMask <<= 1; }
      }
      let b = buf[i + 4]!;
      if (msByte(b) && allowed[(this.prevMask >>> 1) & 7]! && (this.prevMask >>> 1) < 0x10) {
        let src = (buf[i + 1]! | (buf[i + 2]! << 8) | (buf[i + 3]! << 16) | (b << 24)) >>> 0;
        let dest: number;
        for (;;) {
          dest = (src - (ip + i + 5)) >>> 0;
          if (this.prevMask === 0) break;
          const index = bitNumber[this.prevMask >>> 1]! * 8;
          b = (dest >>> (24 - index)) & 0xff;
          if (!msByte(b)) break;
          src = (dest ^ (2 ** (32 - index) - 1)) >>> 0;
        }
        dest &= 0x01ffffff;
        if (dest & 0x01000000) dest |= 0xff000000;
        buf[i + 1] = dest & 0xff;
        buf[i + 2] = (dest >>> 8) & 0xff;
        buf[i + 3] = (dest >>> 16) & 0xff;
        buf[i + 4] = (dest >>> 24) & 0xff;
        i += 5;
        this.prevMask = 0;
      } else {
        i++;
        this.prevMask |= 1;
        if (msByte(b)) this.prevMask |= 0x10;
      }
    }
    this.ip = ip + i;
    return i;
  }
}

/** The ARM64 branch filter (decoding): BL and ADRP targets were stored
 * absolute; turn them back into PC-relative ones. */
class Arm64Stream implements ByteStream {
  private carry: Chunk = new Uint8Array(0);
  private pc: number;
  private done = false;

  private readonly source: ByteStream;
  constructor(source: ByteStream, props: Uint8Array) {
    this.source = source;
    this.pc = props.length >= 4 ? (props[0]! | (props[1]! << 8) | (props[2]! << 16) | (props[3]! << 24)) >>> 0 : 0;
  }

  async read(): Promise<Chunk | null> {
    for (;;) {
      if (this.done) return null;
      const chunk = await this.source.read();
      if (!chunk) {
        this.done = true;
        return this.carry.length > 0 ? this.carry : null;
      }
      const buf = new Uint8Array(this.carry.length + chunk.length);
      buf.set(this.carry, 0);
      buf.set(chunk, this.carry.length);
      const n = buf.length & ~3;
      const view = new DataView(buf.buffer, 0, n);
      for (let i = 0; i < n; i += 4) {
        const pc = (this.pc + i) >>> 0;
        let word = view.getUint32(i, true);
        if ((word >>> 26) === 0x25) {
          word = (0x94000000 | (((word & 0x03ffffff) - (pc >>> 2)) & 0x03ffffff)) >>> 0;
          view.setUint32(i, word, true);
        } else if (((word & 0x9f000000) >>> 0) === 0x90000000) {
          const src = ((word >>> 29) & 3) | ((word >>> 3) & 0x001ffffc);
          if (((src + 0x00020000) & 0x001c0000) !== 0) continue;
          const dest = (src - (pc >>> 12)) | 0;
          word = (word & 0x9000001f) | ((dest & 3) << 29) | ((dest & 0x0003fffc) << 3) | ((0 - (dest & 0x00020000)) & 0x00e00000);
          view.setUint32(i, word >>> 0, true);
        }
      }
      this.pc = (this.pc + n) >>> 0;
      this.carry = buf.slice(n);
      if (n > 0) return buf.subarray(0, n);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Folders.                                                             */
/* ------------------------------------------------------------------ */

function checkFolder(folder: FolderInfo): void {
  for (const c of folder.coders) {
    if (c.method === METHOD_AES) throw new Error(ENCRYPTED_MESSAGE);
  }
  for (const c of folder.coders) {
    const known = [METHOD_COPY, METHOD_LZMA, METHOD_LZMA2, METHOD_BCJ_X86, METHOD_ARM64].includes(c.method);
    if (!known || c.inStreams !== 1 || c.outStreams !== 1) {
      throw new Error(`unsupported 7z compression (${methodName(c.method)}); re-pack the archive with 7-Zip's default LZMA2, or as zip`);
    }
  }
  if (folder.packedStreams.length !== 1) throw new Error("unsupported 7z archive (multi-stream folder)");
}

/** Opens the folder's decoded output. `packStart` is the absolute offset
 * of its (single) packed stream. */
function openFolder(blob: Blob, folder: FolderInfo, packStart: number, packSize: number, chunkBytes: number): ByteStream {
  checkFolder(folder);
  /* Coder i has in-stream i and out-stream i (all coders are 1-in-1-out).
   * Start from the packed in-stream and walk to the unbound out-stream. */
  let stream: ByteStream = new PackedStream(blob, packStart, packStart + packSize, chunkBytes);
  let coder = folder.packedStreams[0]!;
  for (let steps = 0; steps < folder.coders.length; steps++) {
    const c = folder.coders[coder]!;
    const outSize = folder.unpackSizes[coder]!;
    if (c.method === METHOD_LZMA) stream = new LzmaStream(stream, c.props, outSize);
    else if (c.method === METHOD_LZMA2) stream = new Lzma2Stream(stream, c.props, outSize);
    else if (c.method === METHOD_BCJ_X86) stream = new X86Stream(stream);
    else if (c.method === METHOD_ARM64) stream = new Arm64Stream(stream, c.props);
    const next = folder.bindPairs.find((p) => p.outIndex === coder);
    if (!next) return stream;
    coder = next.inIndex;
  }
  throw new Error("damaged 7z header (coder loop)");
}

/** Hands out a folder's output as consecutive Blobs of given sizes. */
class FolderReader {
  private chunk: Chunk = new Uint8Array(0);
  private at = 0;
  private readonly stream: ByteStream;
  constructor(stream: ByteStream) {
    this.stream = stream;
  }

  async take(size: number, crc: number | null, path: string): Promise<Blob> {
    const parts: Chunk[] = [];
    let left = size;
    let running = 0;
    while (left > 0) {
      if (this.at >= this.chunk.length) {
        const next = await this.stream.read();
        if (!next) throw new Error(`damaged 7z archive (${path} is truncated)`);
        this.chunk = next;
        this.at = 0;
      }
      const n = Math.min(left, this.chunk.length - this.at);
      const part = this.chunk.subarray(this.at, this.at + n);
      if (crc !== null) running = crc32(running, part);
      parts.push(part);
      this.at += n;
      left -= n;
    }
    if (crc !== null && running !== crc) throw new Error(`damaged 7z archive (CRC mismatch in ${path})`);
    return new Blob(parts);
  }
}

/** The absolute offset of each packed stream. */
function packOffsets(info: StreamsInfo): number[] {
  const offsets: number[] = [];
  let at = SIGNATURE_HEADER_BYTES + info.packPos;
  for (const size of info.packSizes) { offsets.push(at); at += size; }
  return offsets;
}

/** The index of each folder's first packed stream. */
function folderPackIndex(info: StreamsInfo): number[] {
  const out: number[] = [];
  let index = 0;
  for (const f of info.folders) { out.push(index); index += f.packedStreams.length; }
  return out;
}

async function decodeEncodedHeader(blob: Blob, info: StreamsInfo, chunkBytes: number): Promise<Uint8Array> {
  const offsets = packOffsets(info);
  const first = folderPackIndex(info);
  const parts: Uint8Array[] = [];
  for (let i = 0; i < info.folders.length; i++) {
    const folder = info.folders[i]!;
    const p = first[i]!;
    const reader = new FolderReader(openFolder(blob, folder, offsets[p]!, info.packSizes[p]!, chunkBytes));
    const data = await reader.take(folderSize(folder), folder.unpackCrc, "header");
    parts.push(new Uint8Array(await data.arrayBuffer()));
  }
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
}

interface Archive {
  readonly streams: StreamsInfo | null;
  readonly files: readonly FileEntry[];
}

async function readArchive(blob: Blob, chunkBytes: number): Promise<Archive> {
  if (blob.size < SIGNATURE_HEADER_BYTES) throw new Error("not a 7z archive");
  const start = new DataView(await blob.slice(0, SIGNATURE_HEADER_BYTES).arrayBuffer());
  for (let i = 0; i < SIGNATURE.length; i++) {
    if (start.getUint8(i) !== SIGNATURE[i]) throw new Error("not a 7z archive");
  }
  if (crc32(0, new Uint8Array(start.buffer, 12, 20)) !== start.getUint32(8, true)) throw new Error("damaged 7z archive (start header CRC)");
  const nextOffset = Number(start.getBigUint64(12, true));
  const nextSize = Number(start.getBigUint64(20, true));
  const nextCrc = start.getUint32(28, true);
  if (nextSize === 0) return { streams: null, files: [] };
  const at = SIGNATURE_HEADER_BYTES + nextOffset;
  if (at + nextSize > blob.size) throw new Error("damaged 7z archive (truncated; is it a multi-volume part?)");
  let header: Uint8Array = new Uint8Array(await blob.slice(at, at + nextSize).arrayBuffer());
  if (crc32(0, header) !== nextCrc) throw new Error("damaged 7z archive (header CRC)");
  for (;;) {
    const r = new HeaderReader(header);
    const id = r.byte();
    if (id === K_ENCODED_HEADER) {
      header = await decodeEncodedHeader(blob, readStreamsInfo(r), chunkBytes);
      continue;
    }
    if (id !== K_HEADER) throw new Error("damaged 7z header");
    let streams: StreamsInfo | null = null;
    let files: FileEntry[] = [];
    for (;;) {
      const t = r.byte();
      if (t === K_END) break;
      if (t === K_ARCHIVE_PROPERTIES) {
        for (let p = r.byte(); p !== K_END; p = r.byte()) r.skip(r.number());
      } else if (t === K_ADDITIONAL_STREAMS_INFO) {
        readStreamsInfo(r);
      } else if (t === K_MAIN_STREAMS_INFO) {
        streams = readStreamsInfo(r);
      } else if (t === K_FILES_INFO) {
        files = readFilesInfo(r);
      } else {
        throw new Error(`damaged 7z header (unexpected property ${t})`);
      }
    }
    return { streams, files };
  }
}

export interface SevenZipOptions {
  /** Archive read size (tests use tiny values to cross every boundary). */
  readonly readChunkBytes?: number;
}

/** The archive's files as install sources, in archive order (directories
 * left out). Throws on encrypted archives and unsupported coders before
 * the first file. */
export async function* sevenZipFiles(archive: Blob, options: SevenZipOptions = {}): AsyncGenerator<ModFileSource> {
  const chunkBytes = Math.max(1, options.readChunkBytes ?? READ_CHUNK_BYTES);
  const { streams, files } = await readArchive(archive, chunkBytes);
  const folders = streams?.folders ?? [];
  folders.forEach(checkFolder);
  const offsets = streams ? packOffsets(streams) : [];
  const first = streams ? folderPackIndex(streams) : [];
  let folderIndex = -1;
  let substream = 0;
  let reader: FolderReader | null = null;
  for (const file of files) {
    if (file.isAnti || file.isDirectory) continue;
    if (!file.hasStream) {
      yield { path: file.path, data: new Blob([]) };
      continue;
    }
    if (!streams) throw new Error("damaged 7z archive (more files than streams)");
    if (!reader || substream >= (streams.substreams[folderIndex]?.sizes.length ?? 0)) {
      do {
        folderIndex++;
        if (folderIndex >= folders.length) throw new Error("damaged 7z archive (more files than streams)");
      } while ((streams.substreams[folderIndex]?.sizes.length ?? 0) === 0);
      substream = 0;
      const p = first[folderIndex]!;
      reader = new FolderReader(openFolder(archive, folders[folderIndex]!, offsets[p]!, streams.packSizes[p]!, chunkBytes));
    }
    const sub = streams.substreams[folderIndex]!;
    const data = await reader.take(sub.sizes[substream]!, sub.crcs[substream] ?? null, file.path);
    substream++;
    yield { path: file.path, data };
  }
}
