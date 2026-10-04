/**
 * Game saves in browser storage (§15): one OPFS file per save,
 * "saves-v2/<SS>-<attribute hex>.vsave", holding the core's committed
 * archive (core/hle/fs/save_archive.h). The core snapshots a save when
 * the game commits it - the moment a Switch makes save data durable - so
 * what is stored is always a whole, committed save, never one caught
 * mid-write. Each file is replaced atomically (createWritable writes a
 * temporary copy and swaps it in on close): a tab closed mid-write keeps
 * the previous commit. Export/import wraps the files in a plain tar.
 *
 * Saves from before this format ("saves/SS-<hex>/path", one OPFS file per
 * save file) are still restored by sd-persistence.ts; the next commit
 * rewrites them here.
 */
export const SAVES_V2_DIRECTORY = "saves-v2";
export const SAVE_ARCHIVE_SUFFIX = ".vsave";
const SAVE_NAME = /^[0-9a-f]{2}-[0-9a-f]{128}$/;

/** A committed-archive name ("SS-<128 hex>") is valid. Pure. */
export function isSaveName(name: string): boolean {
  return SAVE_NAME.test(name);
}

async function savesDirectory(): Promise<FileSystemDirectoryHandle | null> {
  if (typeof navigator === "undefined" || !navigator.storage?.getDirectory) return null;
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle(SAVES_V2_DIRECTORY, { create: true });
}

/** Stores one save archive (atomic replace). False without OPFS. */
export async function storeSaveArchive(name: string, bytes: Uint8Array): Promise<boolean> {
  if (!isSaveName(name)) return false;
  const dir = await savesDirectory();
  if (!dir) return false;
  const file = await dir.getFileHandle(name + SAVE_ARCHIVE_SUFFIX, { create: true });
  const writable = await file.createWritable(); /* a swap file until close() */
  await writable.write(new Uint8Array(bytes)); /* a copy: the source may view shared memory */
  await writable.close();
  return true;
}

/** Every stored save archive, by name. */
export async function loadSaveArchives(): Promise<ReadonlyMap<string, Uint8Array>> {
  const archives = new Map<string, Uint8Array>();
  const dir = await savesDirectory();
  if (!dir) return archives;
  for await (const [entry, handle] of dir.entries()) {
    if (handle.kind !== "file" || !entry.endsWith(SAVE_ARCHIVE_SUFFIX)) continue;
    const name = entry.slice(0, -SAVE_ARCHIVE_SUFFIX.length);
    if (!isSaveName(name)) continue; /* also skips the browser's own temporary swap files */
    const file = await (handle as FileSystemFileHandle).getFile();
    archives.set(name, new Uint8Array(await file.arrayBuffer()));
  }
  return archives;
}

/* ---- tar (ustar), for export/import -------------------------------- */

const TAR_BLOCK = 512;
const TAR_NAME_BYTES = 100;
const TAR_SIZE_OFFSET = 124;
const TAR_SIZE_BYTES = 12;
const TAR_MODE = "0000644";
const TAR_CHECKSUM_OFFSET = 148;
const TAR_TYPE_OFFSET = 156;
const TAR_MAGIC_OFFSET = 257;
const TAR_PREFIX_OFFSET = 345;
const TAR_PREFIX_BYTES = 155;

function putText(block: Uint8Array, offset: number, text: string): void {
  for (let i = 0; i < text.length; i++) block[offset + i] = text.charCodeAt(i) & 0x7f;
}

function octal(value: number, digits: number): string {
  return value.toString(8).padStart(digits, "0");
}

/** A ustar archive of `files` (name -> bytes); names must fit 100 bytes. Pure. */
export function writeTar(files: ReadonlyMap<string, Uint8Array>): Uint8Array {
  let total = TAR_BLOCK * 2; /* end-of-archive marker */
  for (const bytes of files.values()) total += TAR_BLOCK + Math.ceil(bytes.byteLength / TAR_BLOCK) * TAR_BLOCK;
  const out = new Uint8Array(total);
  let at = 0;
  for (const [name, bytes] of files) {
    /* Long paths: ustar's prefix field holds the directory part. */
    const slash = name.lastIndexOf("/");
    const base = name.length < TAR_NAME_BYTES || slash < 0 ? name : name.slice(slash + 1);
    const prefix = base === name ? "" : name.slice(0, slash);
    if (base.length >= TAR_NAME_BYTES || prefix.length >= TAR_PREFIX_BYTES) continue;
    const header = out.subarray(at, at + TAR_BLOCK);
    putText(header, 0, base);
    putText(header, TAR_PREFIX_OFFSET, prefix);
    putText(header, 100, `${TAR_MODE}\0`);
    putText(header, 108, "0000000\0");
    putText(header, 116, "0000000\0");
    putText(header, TAR_SIZE_OFFSET, `${octal(bytes.byteLength, 11)}\0`);
    putText(header, 136, `${octal(0, 11)}\0`);
    putText(header, TAR_CHECKSUM_OFFSET, "        ");
    putText(header, TAR_TYPE_OFFSET, "0");
    putText(header, TAR_MAGIC_OFFSET, "ustar\x0000");
    let sum = 0;
    for (const b of header) sum += b;
    putText(header, TAR_CHECKSUM_OFFSET, `${octal(sum, 6)}\0 `);
    out.set(bytes, at + TAR_BLOCK);
    at += TAR_BLOCK + Math.ceil(bytes.byteLength / TAR_BLOCK) * TAR_BLOCK;
  }
  return out;
}

/** The regular files in a tar (name -> bytes); unreadable entries are skipped. Pure. */
export function readTar(tar: Uint8Array): ReadonlyMap<string, Uint8Array> {
  const files = new Map<string, Uint8Array>();
  let at = 0;
  while (at + TAR_BLOCK <= tar.byteLength) {
    const header = tar.subarray(at, at + TAR_BLOCK);
    if (header.every((b) => b === 0)) break;
    const field = (offset: number, bytes: number): string => {
      const raw = header.subarray(offset, offset + bytes);
      const end = raw.indexOf(0);
      return String.fromCharCode(...raw.subarray(0, end < 0 ? raw.length : end));
    };
    const base = field(0, TAR_NAME_BYTES);
    const prefix = field(TAR_PREFIX_OFFSET, TAR_PREFIX_BYTES);
    const name = prefix ? `${prefix}/${base}` : base;
    const size = Number.parseInt(field(TAR_SIZE_OFFSET, TAR_SIZE_BYTES).trim() || "0", 8);
    const type = String.fromCharCode(header[TAR_TYPE_OFFSET] ?? 0);
    if (!Number.isFinite(size) || size < 0 || at + TAR_BLOCK + size > tar.byteLength) break;
    if ((type === "0" || type === "\0") && name) files.set(name, tar.slice(at + TAR_BLOCK, at + TAR_BLOCK + size));
    at += TAR_BLOCK + Math.ceil(size / TAR_BLOCK) * TAR_BLOCK;
  }
  return files;
}

const TAR_SAVES_DIRECTORY = "voland-saves";
const SAVE_NAME_SPLIT = 67; /* "SS-" + 64 hex: the rest fits ustar's 100-byte name */

/** The tar path of a save archive (split so ustar can hold it), and back. Pure. */
export function tarNameOfSave(name: string): string {
  return `${TAR_SAVES_DIRECTORY}/${name.slice(0, SAVE_NAME_SPLIT)}/${name.slice(SAVE_NAME_SPLIT)}${SAVE_ARCHIVE_SUFFIX}`;
}
export function saveNameOfTar(entry: string): string | null {
  if (!entry.endsWith(SAVE_ARCHIVE_SUFFIX)) return null;
  const parts = entry.slice(0, -SAVE_ARCHIVE_SUFFIX.length).split("/").filter((p) => p && p !== TAR_SAVES_DIRECTORY);
  const name = parts.join("");
  return isSaveName(name) ? name : null;
}
