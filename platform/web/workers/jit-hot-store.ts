/**
 * The JIT's hot list across sessions (docs/JIT.md "Hot code across
 * sessions"): the region entries a title compiled, as byte offsets into
 * its code, kept in OPFS per title and code key (emulator_jit_hot_key_ffi:
 * the modules' build ids, so an update or an exefs mod starts a new list).
 * The next session imports them and the JIT compiles each the first time
 * it is reached instead of interpreting it up to the hot threshold.
 *
 * File: u32 magic, u32 version, u32 count, then `count` u32 offsets
 * (little-endian).
 */

export const JIT_HOT_VERSION = 1;
const MAGIC = 0x484a4f56; /* "VOJH" */
const HEADER_WORDS = 3;
const CACHE_DIRECTORY = "jit-hot";
/* The core's table holds 98,304 (jit.c JIT_HOT_MAX). */
export const JIT_HOT_MAX_OFFSETS = 98_304;

/** The file bytes for `offsets`. Pure. */
export function serializeHotList(offsets: Uint32Array): Uint8Array<ArrayBuffer> {
  const words = new Uint32Array(HEADER_WORDS + offsets.length);
  const view = new DataView(words.buffer);
  view.setUint32(0, MAGIC, true);
  view.setUint32(4, JIT_HOT_VERSION, true);
  view.setUint32(8, offsets.length, true);
  for (let i = 0; i < offsets.length; i++) view.setUint32((HEADER_WORDS + i) * 4, offsets[i]!, true);
  return new Uint8Array(words.buffer);
}

/** The offsets in a stored file; damaged or other-version files hold none. Pure. */
export function parseHotList(bytes: ArrayBuffer): Uint32Array {
  if (bytes.byteLength < HEADER_WORDS * 4 || bytes.byteLength % 4 !== 0) return new Uint32Array(0);
  const view = new DataView(bytes);
  const count = view.getUint32(8, true);
  if (view.getUint32(0, true) !== MAGIC || view.getUint32(4, true) !== JIT_HOT_VERSION ||
      count > JIT_HOT_MAX_OFFSETS || bytes.byteLength !== (HEADER_WORDS + count) * 4) return new Uint32Array(0);
  const offsets = new Uint32Array(count);
  for (let i = 0; i < count; i++) offsets[i] = view.getUint32((HEADER_WORDS + i) * 4, true);
  return offsets;
}

/** The file name for a title's list under one code key, or null. Pure. */
export function hotListName(titleId: string, key: bigint): string | null {
  if (!/^[0-9A-F]{16}$/i.test(titleId) || key === 0n) return null;
  return `${titleId.toUpperCase()}-${key.toString(16).padStart(16, "0")}.bin`;
}

async function directory(): Promise<FileSystemDirectoryHandle | null> {
  if (typeof navigator === "undefined" || !navigator.storage?.getDirectory) return null;
  try {
    const root = await navigator.storage.getDirectory();
    return await root.getDirectoryHandle(CACHE_DIRECTORY, { create: true });
  } catch {
    return null;
  }
}

/** The stored offsets of `name` (none without OPFS or a file). */
export async function readHotList(name: string): Promise<Uint32Array> {
  const dir = await directory();
  if (!dir) return new Uint32Array(0);
  try {
    const file = await (await dir.getFileHandle(name)).getFile();
    return parseHotList(await file.arrayBuffer());
  } catch {
    return new Uint32Array(0);
  }
}

/** Replaces the stored list of `name`; false when it could not be written. */
export async function writeHotList(name: string, offsets: Uint32Array): Promise<boolean> {
  const dir = await directory();
  if (!dir) return false;
  try {
    const writable = await (await dir.getFileHandle(name, { create: true })).createWritable();
    await writable.write(serializeHotList(offsets));
    await writable.close();
    return true;
  } catch {
    return false;
  }
}
