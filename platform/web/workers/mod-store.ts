/**
 * Mods in the browser (docs/DESIGN.md §19): installed mods live in the
 * origin-private file system under "mods/<title id>/<mod name>/...",
 * exactly as the user unpacked them (any of the usual layouts: romfs/,
 * exefs/, cheats/, atmosphere/contents/<id>/..., *.ips, *.pchtxt - the
 * core sorts out which file does what). Per-title order and enable flags
 * are a small JSON file next to them. Voland ships no mods and no mod
 * sources; this is the mechanism only.
 *
 * Used from both the page (install, list, reorder) and the CPU worker
 * (collect the enabled mods' files before a load). Zip archives are read
 * here too, with the browser's own DecompressionStream - no library.
 */
const MODS_DIRECTORY = "mods";
const CONFIG_FILE = ".voland-mods.json";

export interface ModInfo {
  readonly name: string;
  readonly enabled: boolean;
  readonly files: number;
  readonly bytes: number;
  readonly source: ModSource | null;
}

/** Where a mod installed from a repository came from (§19 provenance). */
export interface ModSource {
  readonly repo: string;
  readonly modId: string;
  readonly version: string;
}

interface ModConfig {
  readonly order: readonly string[];
  readonly disabled: readonly string[];
  readonly sources?: Readonly<Record<string, ModSource>>;
}

const EMPTY_CONFIG: ModConfig = { order: [], disabled: [] };

async function modsRoot(create: boolean): Promise<FileSystemDirectoryHandle | null> {
  if (typeof navigator === "undefined" || !navigator.storage?.getDirectory) return null;
  try {
    const root = await navigator.storage.getDirectory();
    return await root.getDirectoryHandle(MODS_DIRECTORY, { create });
  } catch {
    return null;
  }
}

async function titleDirectory(titleId: string, create: boolean): Promise<FileSystemDirectoryHandle | null> {
  const root = await modsRoot(create);
  if (!root) return null;
  try {
    return await root.getDirectoryHandle(titleId.toLowerCase(), { create });
  } catch {
    return null;
  }
}

function isConfig(value: unknown): value is ModConfig {
  if (typeof value !== "object" || value === null) return false;
  const v = value as Record<string, unknown>;
  return Array.isArray(v["order"]) && Array.isArray(v["disabled"]);
}

async function readConfig(dir: FileSystemDirectoryHandle): Promise<ModConfig> {
  try {
    const file = await (await dir.getFileHandle(CONFIG_FILE)).getFile();
    const parsed: unknown = JSON.parse(await file.text());
    return isConfig(parsed) ? parsed : EMPTY_CONFIG;
  } catch {
    return EMPTY_CONFIG;
  }
}

async function writeConfig(dir: FileSystemDirectoryHandle, config: ModConfig): Promise<void> {
  const handle = await dir.getFileHandle(CONFIG_FILE, { create: true });
  const writable = await handle.createWritable();
  await writable.write(JSON.stringify(config));
  await writable.close();
}

async function modNames(dir: FileSystemDirectoryHandle): Promise<string[]> {
  const names: string[] = [];
  for await (const [name, handle] of dir.entries()) if (handle.kind === "directory") names.push(name);
  return names;
}

/** Mod names in priority order: the saved order first, then the rest by
 * name. Pure. */
export function orderedNames(names: readonly string[], order: readonly string[]): string[] {
  const present = new Set(names);
  const first = order.filter((n) => present.has(n));
  const rest = names.filter((n) => !first.includes(n)).sort((a, b) => a.localeCompare(b));
  return [...first, ...rest];
}

async function walkFiles(dir: FileSystemDirectoryHandle, prefix: string,
                         visit: (path: string, handle: FileSystemFileHandle) => Promise<void>): Promise<void> {
  const entries: [string, FileSystemHandle][] = [];
  for await (const entry of dir.entries()) entries.push(entry);
  entries.sort((a, b) => a[0].localeCompare(b[0]));
  for (const [name, handle] of entries) {
    const path = prefix ? `${prefix}/${name}` : name;
    if (handle.kind === "directory") await walkFiles(handle as FileSystemDirectoryHandle, path, visit);
    else await visit(path, handle as FileSystemFileHandle);
  }
}

/** The title's installed mods, highest priority first. */
export async function listMods(titleId: string): Promise<readonly ModInfo[]> {
  const dir = await titleDirectory(titleId, false);
  if (!dir) return [];
  const config = await readConfig(dir);
  const result: ModInfo[] = [];
  for (const name of orderedNames(await modNames(dir), config.order)) {
    let files = 0;
    let bytes = 0;
    await walkFiles(await dir.getDirectoryHandle(name), "", async (_path, handle) => {
      files++;
      bytes += (await handle.getFile()).size;
    });
    result.push({ name, enabled: !config.disabled.includes(name), files, bytes, source: config.sources?.[name] ?? null });
  }
  return result;
}

export async function setModEnabled(titleId: string, name: string, enabled: boolean): Promise<void> {
  const dir = await titleDirectory(titleId, true);
  if (!dir) return;
  const config = await readConfig(dir);
  const disabled = config.disabled.filter((n) => n !== name);
  await writeConfig(dir, { ...config, disabled: enabled ? disabled : [...disabled, name] });
}

/** Moves `name` one place up (delta -1) or down (+1) in priority. */
export async function moveMod(titleId: string, name: string, delta: number): Promise<void> {
  const dir = await titleDirectory(titleId, true);
  if (!dir) return;
  const config = await readConfig(dir);
  const order = orderedNames(await modNames(dir), config.order);
  const at = order.indexOf(name);
  const to = at + delta;
  if (at < 0 || to < 0 || to >= order.length) return;
  const next = order.slice();
  next.splice(at, 1);
  next.splice(to, 0, name);
  await writeConfig(dir, { ...config, order: next });
}

export async function removeMod(titleId: string, name: string): Promise<void> {
  const dir = await titleDirectory(titleId, false);
  if (!dir) return;
  await dir.removeEntry(name, { recursive: true }).catch(() => undefined);
  const config = await readConfig(dir);
  const sources = { ...config.sources };
  delete sources[name];
  await writeConfig(dir, { order: config.order.filter((n) => n !== name), disabled: config.disabled.filter((n) => n !== name), sources });
}

/** Records that mod `name` came from a repository entry. */
export async function recordModSource(titleId: string, name: string, source: ModSource): Promise<void> {
  const dir = await titleDirectory(titleId, true);
  if (!dir) return;
  const config = await readConfig(dir);
  await writeConfig(dir, { ...config, sources: { ...config.sources, [name]: source } });
}

/** A folder or archive name made safe as one OPFS component. Pure. */
export function safeModName(name: string): string {
  const cleaned = name.replace(/\.(zip)$/i, "").replace(/[\/\\:*?"<>|\u0000-\u001f]/g, "_").trim();
  return cleaned.length > 0 && cleaned !== "." && cleaned !== ".." ? cleaned.slice(0, 120) : "mod";
}

/** Path components of an archive or folder entry, dropping empty, "." and
 * ".." parts (a hostile archive cannot climb out of its mod). Pure. */
export function safeParts(path: string): string[] {
  return path.split(/[\/\\]/).filter((p) => p.length > 0 && p !== "." && p !== "..");
}

export interface ModFileSource {
  readonly path: string;
  readonly data: Blob;
}

/** Installs (or replaces) mod `name` for the title from `files`. Returns
 * how many files were written. */
export async function installMod(titleId: string, name: string, files: AsyncIterable<ModFileSource> | Iterable<ModFileSource>,
                                 onProgress?: (written: number) => void): Promise<number> {
  const dir = await titleDirectory(titleId, true);
  if (!dir) return 0;
  const modName = safeModName(name);
  await dir.removeEntry(modName, { recursive: true }).catch(() => undefined);
  const mod = await dir.getDirectoryHandle(modName, { create: true });
  let written = 0;
  for await (const file of files) {
    const parts = safeParts(file.path);
    const leaf = parts.pop();
    if (!leaf) continue;
    let at = mod;
    for (const part of parts) at = await at.getDirectoryHandle(part, { create: true });
    const handle = await at.getFileHandle(leaf, { create: true });
    const writable = await handle.createWritable();
    await file.data.stream().pipeTo(writable);
    written++;
    onProgress?.(written);
  }
  return written;
}

export interface EnabledModFile {
  readonly mod: string;
  readonly path: string;
  readonly file: File;
}

/** Every file of the title's enabled mods, highest-priority mod first
 * (the CPU worker lists them into the core before a load). */
export async function enabledModFiles(titleId: string): Promise<readonly EnabledModFile[]> {
  const dir = await titleDirectory(titleId, false);
  if (!dir) return [];
  const config = await readConfig(dir);
  const result: EnabledModFile[] = [];
  for (const name of orderedNames(await modNames(dir), config.order)) {
    if (config.disabled.includes(name)) continue;
    await walkFiles(await dir.getDirectoryHandle(name), "", async (path, handle) => {
      result.push({ mod: name, path, file: await handle.getFile() });
    });
  }
  return result;
}

/* ------------------------------------------------------------------ */
/* Zip archives (stored and deflate entries, zip64 sizes).             */
/* ------------------------------------------------------------------ */

const EOCD_SIGNATURE = 0x06054b50;
const ZIP64_LOCATOR_SIGNATURE = 0x07064b50;
const ZIP64_EOCD_SIGNATURE = 0x06064b50;
const CENTRAL_SIGNATURE = 0x02014b50;
const LOCAL_SIGNATURE = 0x04034b50;
const EOCD_MIN_BYTES = 22;
const EOCD_SEARCH_BYTES = EOCD_MIN_BYTES + 0xffff;
const METHOD_STORED = 0;
const METHOD_DEFLATE = 8;

async function bytesOf(blob: Blob, start: number, end: number): Promise<DataView> {
  return new DataView(await blob.slice(start, end).arrayBuffer());
}

function u64(view: DataView, at: number): number {
  return Number(view.getBigUint64(at, true));
}

export interface ZipEntry {
  readonly path: string;
  readonly method: number;
  readonly compressedSize: number;
  readonly size: number;
  readonly localHeaderOffset: number;
}

/** The archive's file entries (directories left out). Throws on a file
 * that is not a zip or uses features this reader does not have. */
export async function zipEntries(zip: Blob): Promise<readonly ZipEntry[]> {
  const tailStart = Math.max(0, zip.size - EOCD_SEARCH_BYTES);
  const tail = await bytesOf(zip, tailStart, zip.size);
  let eocd = -1;
  for (let i = tail.byteLength - EOCD_MIN_BYTES; i >= 0; i--) {
    if (tail.getUint32(i, true) === EOCD_SIGNATURE) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error("not a zip archive");
  let count = tail.getUint16(eocd + 10, true);
  let directorySize = tail.getUint32(eocd + 12, true);
  let directoryOffset = tail.getUint32(eocd + 16, true);
  if (eocd >= 20 && tail.getUint32(eocd - 20, true) === ZIP64_LOCATOR_SIGNATURE) {
    const at = u64(tail, eocd - 20 + 8);
    const z = await bytesOf(zip, at, at + 56);
    if (z.getUint32(0, true) !== ZIP64_EOCD_SIGNATURE) throw new Error("damaged zip64 archive");
    count = u64(z, 32);
    directorySize = u64(z, 40);
    directoryOffset = u64(z, 48);
  }
  const dir = await bytesOf(zip, directoryOffset, directoryOffset + directorySize);
  const decoder = new TextDecoder();
  const entries: ZipEntry[] = [];
  let p = 0;
  for (let i = 0; i < count; i++) {
    if (p + 46 > dir.byteLength || dir.getUint32(p, true) !== CENTRAL_SIGNATURE) throw new Error("damaged zip directory");
    const method = dir.getUint16(p + 10, true);
    let compressedSize = dir.getUint32(p + 20, true);
    let size = dir.getUint32(p + 24, true);
    const nameLength = dir.getUint16(p + 28, true);
    const extraLength = dir.getUint16(p + 30, true);
    const commentLength = dir.getUint16(p + 32, true);
    let localHeaderOffset = dir.getUint32(p + 42, true);
    const path = decoder.decode(new Uint8Array(dir.buffer, dir.byteOffset + p + 46, nameLength));
    /* Zip64 extra field: the 0xFFFFFFFF fields, in order. */
    let e = p + 46 + nameLength;
    const extraEnd = e + extraLength;
    while (e + 4 <= extraEnd) {
      const id = dir.getUint16(e, true);
      const length = dir.getUint16(e + 2, true);
      if (id === 0x0001) {
        let f = e + 4;
        if (size === 0xffffffff) { size = u64(dir, f); f += 8; }
        if (compressedSize === 0xffffffff) { compressedSize = u64(dir, f); f += 8; }
        if (localHeaderOffset === 0xffffffff) { localHeaderOffset = u64(dir, f); }
      }
      e += 4 + length;
    }
    p = extraEnd + commentLength;
    if (path.endsWith("/")) continue;
    entries.push({ path, method, compressedSize, size, localHeaderOffset });
  }
  return entries;
}

/** One entry's contents as a Blob (decompressed). */
export async function zipEntryData(zip: Blob, entry: ZipEntry): Promise<Blob> {
  const local = await bytesOf(zip, entry.localHeaderOffset, entry.localHeaderOffset + 30);
  if (local.getUint32(0, true) !== LOCAL_SIGNATURE) throw new Error(`damaged zip entry ${entry.path}`);
  const start = entry.localHeaderOffset + 30 + local.getUint16(26, true) + local.getUint16(28, true);
  const raw = zip.slice(start, start + entry.compressedSize);
  if (entry.method === METHOD_STORED) return raw;
  if (entry.method !== METHOD_DEFLATE) throw new Error(`${entry.path}: unsupported zip compression (method ${entry.method}); re-pack it as a normal zip`);
  return new Response(raw.stream().pipeThrough(new DecompressionStream("deflate-raw"))).blob();
}

/** The archive's files as install sources. */
export async function* zipFiles(zip: Blob): AsyncGenerator<ModFileSource> {
  for (const entry of await zipEntries(zip)) yield { path: entry.path, data: await zipEntryData(zip, entry) };
}
