/**
 * Save archives as a file tree (core/hle/fs/save_archive.h): the save
 * browser reads a stored save, edits its files, and writes it back in the
 * same format the core restores. Pure functions; every edit returns a new
 * tree.
 *
 *   Archive (little-endian):
 *     u32 magic "VSAV", u32 version, u32 entry count, u32 reserved
 *     per entry: u32 kind (0 directory, 1 file), u32 path bytes, path
 *                (absolute, no NUL), u64 size (0 for directories), data
 *   Parents come before children.
 *
 * Save names are "SS-<SaveDataAttribute hex>" (save space, then the
 * 0x40-byte attribute: u64 program id, u128 user id, u64 system save id,
 * u8 SaveDataType, ...).
 */

const MAGIC = 0x56415356; /* "VSAV" */
const VERSION = 1;
const HEADER_BYTES = 16;
const KIND_DIRECTORY = 0;
const KIND_FILE = 1;
const ENTRY_FIXED_BYTES = 16; /* kind + path bytes + size */
/* Horizon's path limit (fs.h FS_MAX_PATH_BYTES), less the NUL. */
export const SAVE_PATH_MAX_BYTES = 0x300;

/** Absolute path ("/dir/file") -> file bytes, or null for a directory. */
export type SaveTree = ReadonlyMap<string, Uint8Array | null>;

const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });

/** The tree in `bytes`, or null if it is not a well-formed archive. Pure. */
export function parseSaveArchive(bytes: Uint8Array): SaveTree | null {
  if (bytes.byteLength < HEADER_BYTES) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(0, true) !== MAGIC || view.getUint32(4, true) !== VERSION) return null;
  const count = view.getUint32(8, true);
  const tree = new Map<string, Uint8Array | null>();
  let at = HEADER_BYTES;
  try {
    for (let i = 0; i < count; i++) {
      if (bytes.byteLength - at < 8) return null;
      const kind = view.getUint32(at, true);
      const pathBytes = view.getUint32(at + 4, true);
      at += 8;
      if (kind > KIND_FILE || pathBytes < 2 || bytes.byteLength - at < pathBytes + 8) return null;
      const path = decoder.decode(bytes.subarray(at, at + pathBytes));
      at += pathBytes;
      const size = Number(view.getBigUint64(at, true));
      at += 8;
      if (!path.startsWith("/") || path.includes("\0")) return null;
      if (kind === KIND_DIRECTORY) {
        if (size !== 0) return null;
        tree.set(path, null);
      } else {
        if (bytes.byteLength - at < size) return null;
        tree.set(path, bytes.slice(at, at + size));
        at += size;
      }
    }
  } catch {
    return null; /* a path that is not UTF-8 */
  }
  return at === bytes.byteLength ? tree : null;
}

function segments(path: string): string[] {
  return path.split("/").filter((part) => part.length > 0);
}

/** Depth-first order: a parent sorts before its children. */
function comparePaths(a: string, b: string): number {
  const x = segments(a), y = segments(b);
  for (let i = 0; i < Math.min(x.length, y.length); i++) {
    const p = x[i] ?? "", q = y[i] ?? "";
    if (p !== q) return p < q ? -1 : 1;
  }
  return x.length - y.length;
}

/** `tree` as archive bytes (missing parent directories are added). Pure. */
export function writeSaveArchive(tree: SaveTree): Uint8Array {
  const complete = new Map(tree);
  for (const path of tree.keys()) {
    const parts = segments(path);
    for (let i = 1; i < parts.length; i++) {
      const parent = `/${parts.slice(0, i).join("/")}`;
      if (!complete.has(parent)) complete.set(parent, null);
    }
  }
  const paths = [...complete.keys()].sort(comparePaths);
  const encoded = paths.map((path) => encoder.encode(path));
  let total = HEADER_BYTES;
  paths.forEach((path, i) => { total += ENTRY_FIXED_BYTES + (encoded[i]?.byteLength ?? 0) + (complete.get(path)?.byteLength ?? 0); });
  const out = new Uint8Array(total);
  const view = new DataView(out.buffer);
  view.setUint32(0, MAGIC, true);
  view.setUint32(4, VERSION, true);
  view.setUint32(8, paths.length, true);
  let at = HEADER_BYTES;
  paths.forEach((path, i) => {
    const data = complete.get(path) ?? null;
    const name = encoded[i] ?? new Uint8Array(0);
    view.setUint32(at, data ? KIND_FILE : KIND_DIRECTORY, true);
    view.setUint32(at + 4, name.byteLength, true);
    out.set(name, at + 8);
    at += 8 + name.byteLength;
    view.setBigUint64(at, BigInt(data?.byteLength ?? 0), true);
    at += 8;
    if (data) {
      out.set(data, at);
      at += data.byteLength;
    }
  });
  return out;
}

/** A clean absolute path ("a//b/" -> "/a/b"), or null if it is unusable
 * (".." parts, too long). Pure. */
export function normalizeSavePath(path: string): string | null {
  const parts = segments(path);
  if (parts.some((part) => part === "." || part === "..")) return null;
  const clean = `/${parts.join("/")}`;
  return encoder.encode(clean).byteLength <= SAVE_PATH_MAX_BYTES ? clean : null;
}

/** Adds or replaces the file at `path` (parents are created). Pure. */
export function putSaveFile(tree: SaveTree, path: string, bytes: Uint8Array): SaveTree {
  const clean = normalizeSavePath(path);
  if (!clean || clean === "/") return tree;
  const next = new Map(tree);
  /* A directory of that name, and everything in it, gives way. */
  for (const key of tree.keys()) if (key === clean || key.startsWith(`${clean}/`)) next.delete(key);
  next.set(clean, bytes);
  return next;
}

/** Adds an empty directory at `path`. Pure. */
export function addSaveDirectory(tree: SaveTree, path: string): SaveTree {
  const clean = normalizeSavePath(path);
  if (!clean || clean === "/" || tree.has(clean)) return tree;
  return new Map(tree).set(clean, null);
}

/** Removes `path` and, for a directory, everything under it. Pure. */
export function removeSavePath(tree: SaveTree, path: string): SaveTree {
  const clean = normalizeSavePath(path);
  if (!clean) return tree;
  if (clean === "/") return new Map();
  return new Map([...tree].filter(([key]) => key !== clean && !key.startsWith(`${clean}/`)));
}

/** Entries in display order (parents before children). Pure. */
export function sortedSaveEntries(tree: SaveTree): readonly (readonly [string, Uint8Array | null])[] {
  return [...tree].sort(([a], [b]) => comparePaths(a, b));
}

export interface SaveIdentity {
  /** Save space: 0 System, 1 User, 2 SdSystem, ... */
  readonly space: number;
  /** 16 hex digits, as titles are written ("010013C00E930000"). */
  readonly programId: string;
  readonly userId: string;
  readonly type: number;
  readonly typeLabel: string;
}

const SAVE_TYPE_LABELS: readonly string[] = ["System", "Player", "BCAT", "Device", "Temporary", "Cache", "System BCAT"];
const ATTRIBUTE_PROGRAM_ID = 0;
const ATTRIBUTE_USER_ID = 8;
const ATTRIBUTE_TYPE = 0x20;
const ID_BYTES = 8;
const USER_ID_BYTES = 16;

function hexBytes(hex: string, offset: number, count: number): number[] {
  return Array.from({ length: count }, (_, i) => Number.parseInt(hex.slice(2 * (offset + i), 2 * (offset + i) + 2), 16));
}

/** What a save name says about the save, or null if it is not one. Pure. */
export function describeSaveName(name: string): SaveIdentity | null {
  const match = /^([0-9a-f]{2})-([0-9a-f]{128})$/.exec(name);
  if (!match) return null;
  const attribute = match[2] ?? "";
  /* Little-endian ids, written most significant byte first. */
  const programId = hexBytes(attribute, ATTRIBUTE_PROGRAM_ID, ID_BYTES).reverse()
    .map((b) => b.toString(16).padStart(2, "0")).join("").toUpperCase();
  const userId = hexBytes(attribute, ATTRIBUTE_USER_ID, USER_ID_BYTES)
    .map((b) => b.toString(16).padStart(2, "0")).join("");
  const type = hexBytes(attribute, ATTRIBUTE_TYPE, 1)[0] ?? 0;
  return {
    space: Number.parseInt(match[1] ?? "0", 16),
    programId,
    userId,
    type,
    typeLabel: SAVE_TYPE_LABELS[type] ?? `Type ${type}`,
  };
}
