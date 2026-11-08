/**
 * The Voland folder: one folder of the player's own, chosen once, that
 * holds everything Voland reads from disk so nothing has to be picked file
 * by file again:
 *
 *   games/      a decrypted game per subfolder (its NCAs: program, control,
 *               update, DLC), or loose .nca files (an update or DLC beside
 *               its game is matched to it by program id)
 *   firmware/   the decrypted firmware NCAs (only the system files games
 *               need are kept - LoadPanel's "System files")
 *   mods/<game>/<mod>   a mod folder, .zip / .7z or update / DLC .nca;
 *               <game> is the game's title id or its name in the library
 *
 * The folder is kept as a directory handle (IndexedDB) where the browser
 * can, else picked again with a folder input. Voland reads it in place and
 * only ever writes the three empty subfolders and a README when asked to
 * set one up. Like a game folder (game-folder.ts), still-encrypted NCAs
 * are counted and left out; Voland never decrypts (§1.6).
 */
import { type NcaInfo, classifyNcaHeader } from "./game-folder.ts";

export const GAMES_DIR = "games";
export const FIRMWARE_DIR = "firmware";
export const MODS_DIR = "mods";
export const README_NAME = "README.txt";
/** Marks a mod installed from the Voland folder (mod-store sources). */
export const FOLDER_SOURCE = "voland-folder";

/** Program ids (emulator.h): an update is its game's + UPDATE_ID_OFFSET,
 * DLC its game's + ADDON_BASE_OFFSET + index (1..ADDON_ID_MASK). */
const UPDATE_ID_OFFSET = 0x800n;
const ADDON_BASE_OFFSET = 0x1000n;
const ADDON_ID_MASK = 0xfffn;
/** Bytes of an NCA header classifyNcaHeader reads. */
const NCA_PROBE_BYTES = 0x218;
/** How deep the walk goes (mods keep their romfs/ trees). */
const MAX_DEPTH = 16;

/** One file in the folder: its path below the folder's root. */
export interface FolderFile {
  readonly path: readonly string[];
  readonly file: File;
  /** Absent for a folder picked with a plain folder input. */
  readonly handle?: FileSystemFileHandle;
}

/** A game found under games/. */
export interface FolderGame {
  /** The library key it is remembered under (library.ts gameIdOf /
   * folderIdOf), so a tile and the folder's game are one entry. */
  readonly id: string;
  readonly name: string;
  readonly programId: bigint;
  readonly program: FolderFile;
  /** Its other decrypted NCAs (control, update, DLC). */
  readonly extras: readonly FolderFile[];
  /** Its subfolder under games/ (absent for a loose .nca). */
  readonly dir: string | null;
  readonly hasUpdate: boolean;
  readonly dlc: number;
}

/** A mod under mods/<key>/. */
export interface FolderMod {
  readonly key: string;
  readonly name: string;
  /** The mod's files, paths relative to the mod (an archive or .nca is
   * one file named after itself). */
  readonly files: readonly FolderFile[];
  /** A .zip / .7z to unpack. */
  readonly archive: boolean;
  /** Changes when any of its files does: count, bytes, newest mtime. */
  readonly fingerprint: string;
}

export interface FolderScan {
  readonly games: readonly FolderGame[];
  readonly firmware: readonly FolderFile[];
  readonly firmwareFingerprint: string;
  readonly mods: readonly FolderMod[];
  /** Which of games/, firmware/, mods/ are missing. */
  readonly missingDirs: readonly string[];
  /** Still-encrypted NCAs under games/, by path. */
  readonly encrypted: readonly string[];
  /** Decrypted NCAs under games/ that belong to no game found there
   * (an update or DLC whose game is missing). */
  readonly orphans: readonly string[];
  /** .nca files outside games/, firmware/ and mods/ (likely misplaced). */
  readonly stray: readonly string[];
}

/** Whether `id` is the base game `base` itself, its update, or its DLC
 * (by program id; a control NCA carries its game's or its update's).
 * Pure. */
export function belongsTo(base: bigint, id: bigint): boolean {
  if (id === base || id === base + UPDATE_ID_OFFSET) return true;
  const first = base + ADDON_BASE_OFFSET;
  return id > first && id - first <= ADDON_ID_MASK;
}

/** Whether `id` is DLC of `base`. Pure. */
function isAddonOf(base: bigint, id: bigint): boolean {
  return id !== base && id !== base + UPDATE_ID_OFFSET && belongsTo(base, id);
}

function isNca(name: string): boolean {
  return /\.nca$/i.test(name);
}

function joined(path: readonly string[]): string {
  return path.join("/");
}

function fingerprintOf(files: readonly FolderFile[]): string {
  let bytes = 0;
  let newest = 0;
  for (const f of files) {
    bytes += f.file.size;
    newest = Math.max(newest, f.file.lastModified);
  }
  return `${files.length}:${bytes}:${newest}`;
}

/** The library key of a loose game file (library.ts gameIdOf). Pure. */
function looseId(file: File): string {
  return `${file.name}:${file.size}:${file.lastModified}`;
}

/** The library key of a game folder (library.ts folderIdOf). Pure. */
function folderId(dir: string, file: File): string {
  return `folder:${dir}/${looseId(file)}`;
}

interface Classified {
  readonly entry: FolderFile;
  readonly info: NcaInfo;
}

/** Groups classified NCAs into games: each base Program NCA is a game (in
 * a subfolder, only its largest), and every other decrypted NCA goes with
 * the game it belongs to. Pure. */
function groupGames(ncas: readonly Classified[], dir: string | null): { games: FolderGame[]; orphans: string[] } {
  const programs = ncas.filter((c) => c.info.kind === "program")
    .sort((a, b) => b.entry.file.size - a.entry.file.size);
  const chosen = dir === null ? programs : programs.slice(0, 1);
  const games: FolderGame[] = [];
  const claimed = new Set<Classified>(chosen);
  for (const program of chosen) {
    const base = program.info.programId;
    const extras = ncas.filter((c) => c !== program && c.info.kind !== "encrypted" &&
      (dir !== null || belongsTo(base, c.info.programId)));
    for (const extra of extras) claimed.add(extra);
    games.push({
      id: dir === null ? looseId(program.entry.file) : folderId(dir, program.entry.file),
      name: dir ?? program.entry.file.name.replace(/\.nca$/i, ""),
      programId: base,
      program: program.entry,
      extras: extras.map((c) => c.entry),
      dir,
      hasUpdate: extras.some((c) => c.info.kind === "update" && c.info.programId === base + UPDATE_ID_OFFSET),
      dlc: new Set(extras.filter((c) => isAddonOf(base, c.info.programId)).map((c) => c.info.programId)).size,
    });
  }
  const orphans = ncas.filter((c) => !claimed.has(c) && c.info.kind !== "encrypted").map((c) => joined(c.entry.path));
  return { games, orphans };
}

/** Sorts the folder's files (from walkFolder or a folder input) into
 * games, firmware and mods. Reads the header of each .nca under games/. */
export async function scanFolder(files: readonly FolderFile[], dirs: readonly string[]): Promise<FolderScan> {
  const top = new Set(dirs.filter((d) => !d.includes("/")).map((d) => d.toLowerCase()));
  for (const f of files) if (f.path.length > 1 && f.path[0]) top.add(f.path[0].toLowerCase());
  const under = (dir: string): FolderFile[] => files.filter((f) => f.path.length > 1 && f.path[0]?.toLowerCase() === dir);

  const gameNcas = await Promise.all(under(GAMES_DIR).filter((f) => isNca(f.file.name)).map(async (entry) => ({
    entry, info: classifyNcaHeader(new Uint8Array(await entry.file.slice(0, NCA_PROBE_BYTES).arrayBuffer())),
  })));
  const games: FolderGame[] = [];
  const orphans: string[] = [];
  const loose = gameNcas.filter((c) => c.entry.path.length === 2);
  const looseGroup = groupGames(loose, null);
  games.push(...looseGroup.games);
  orphans.push(...looseGroup.orphans);
  const subdirs = [...new Set(gameNcas.filter((c) => c.entry.path.length > 2).map((c) => c.entry.path[1] ?? ""))].sort();
  for (const dir of subdirs) {
    const group = groupGames(gameNcas.filter((c) => c.entry.path.length > 2 && c.entry.path[1] === dir), dir);
    games.push(...group.games);
    orphans.push(...group.orphans);
  }

  const firmware = under(FIRMWARE_DIR);
  const mods: FolderMod[] = [];
  const byMod = new Map<string, FolderFile[]>();
  for (const f of under(MODS_DIR)) {
    const [, key, name] = f.path;
    if (!key || !name) continue;
    const loneFile = f.path.length === 3;
    if (loneFile && !/\.(zip|7z|nca)$/i.test(name)) continue;
    const id = `${key}\u0000${name}`;
    const list = byMod.get(id) ?? [];
    list.push(loneFile ? { ...f, path: [name] } : { ...f, path: f.path.slice(3) });
    byMod.set(id, list);
  }
  for (const [id, list] of [...byMod.entries()].sort((a, b) => a[0].localeCompare(b[0]))) {
    const [key = "", name = ""] = id.split("\u0000");
    const archive = list.length === 1 && list[0]?.path.length === 1 && list[0]?.path[0] === name && /\.(zip|7z)$/i.test(name);
    mods.push({ key, name, files: list, archive, fingerprint: fingerprintOf(list) });
  }

  return {
    games: games.sort((a, b) => a.name.localeCompare(b.name)),
    firmware,
    firmwareFingerprint: fingerprintOf(firmware),
    mods,
    missingDirs: [GAMES_DIR, FIRMWARE_DIR, MODS_DIR].filter((d) => !top.has(d)),
    encrypted: gameNcas.filter((c) => c.info.kind === "encrypted").map((c) => joined(c.entry.path)),
    orphans,
    stray: files.filter((f) => isNca(f.file.name) &&
      ![GAMES_DIR, FIRMWARE_DIR, MODS_DIR].includes(f.path.length > 1 ? (f.path[0] ?? "").toLowerCase() : ""))
      .map((f) => joined(f.path)),
  };
}

/** Normalizes a game name for matching a mods/<name> folder. Pure. */
function nameKey(name: string): string {
  return name.toLowerCase().replace(/[^\p{L}\p{N}]+/gu, "");
}

/** The title id a mods/<key> folder is for: the key itself when it is a
 * 16-digit title id, else the known game whose name matches (ignoring
 * case, spaces and punctuation). Null if none does. Pure. */
export function modTitleId(key: string, known: readonly { readonly titleId: string; readonly names: readonly string[] }[]): string | null {
  if (/^[0-9a-f]{16}$/i.test(key)) return key.toUpperCase();
  const want = nameKey(key);
  if (want.length === 0) return null;
  return known.find((k) => k.names.some((n) => nameKey(n) === want))?.titleId ?? null;
}

/** "folder input" files (webkitRelativePath "Voland/games/x.nca") as
 * folder entries below the chosen folder. Pure. */
export function inputFiles(files: readonly File[]): { readonly name: string; readonly files: FolderFile[]; readonly dirs: string[] } {
  const rel = (f: File): string[] => ((f as File & { readonly webkitRelativePath?: string }).webkitRelativePath || f.name).split("/");
  const name = rel(files[0] ?? new File([], "Voland"))[0] ?? "Voland";
  const entries = files.map((file) => ({ path: rel(file).slice(1), file })).filter((f) => f.path.length > 0 && !hidden(f.path));
  const dirs = new Set<string>();
  for (const f of entries) for (let i = 1; i < f.path.length; i++) dirs.add(f.path.slice(0, i).join("/"));
  return { name, files: entries, dirs: [...dirs] };
}

/** macOS and Windows bookkeeping files and dot-folders. Pure. */
function hidden(path: readonly string[]): boolean {
  return path.some((p) => p.startsWith(".") || p === "Thumbs.db" || p === "desktop.ini");
}

/** Every file below `root` and every subfolder path (a kept folder). */
export async function walkFolder(root: FileSystemDirectoryHandle): Promise<{ files: FolderFile[]; dirs: string[] }> {
  const files: FolderFile[] = [];
  const dirs: string[] = [];
  const walk = async (dir: FileSystemDirectoryHandle, path: readonly string[]): Promise<void> => {
    const entries = (dir as unknown as { values: () => AsyncIterable<FileSystemHandle> }).values();
    for await (const entry of entries) {
      const at = [...path, entry.name];
      if (hidden(at)) continue;
      if (entry.kind === "file") {
        const handle = entry as FileSystemFileHandle;
        files.push({ path: at, file: await handle.getFile(), handle });
      } else if (at.length < MAX_DEPTH) {
        dirs.push(joined(at));
        await walk(entry as FileSystemDirectoryHandle, at);
      }
    }
  };
  await walk(root, []);
  return { files, dirs };
}

/** A game's files read again through their handles (they may have
 * changed since the scan; File objects go stale). */
export async function freshFiles(game: FolderGame): Promise<{ readonly file: File; readonly extras: readonly File[] }> {
  const fresh = (f: FolderFile): Promise<File> => f.handle ? f.handle.getFile() : Promise.resolve(f.file);
  return { file: await fresh(game.program), extras: await Promise.all(game.extras.map(fresh)) };
}

export const README_TEXT = `This is your Voland folder. Voland reads it in place; it never uploads or copies your games.

games/      One subfolder per game with its decrypted .nca files (the game,
            its control NCA for the name and icon, an update, DLC).
            Loose .nca files work too.
firmware/   Your console's decrypted firmware NCAs. Voland keeps only the
            system files some games need (the Mii model, NG word lists).
mods/       One subfolder per game, named after the game (as the library
            shows it) or its title id, e.g. mods/Celeste/MyMod/romfs/...
            A mod can be a folder, a .zip / .7z, or an update / DLC .nca.

Decrypt everything with your own tools first. Voland never decrypts and
never asks for keys.
`;

/* ---- keeping the folder (one IndexedDB entry: its handle) ---- */

const DB_NAME = "voland-folder";
const DB_VERSION = 1;
const STORE = "handles";
const ROOT_KEY = "root";

function openDb(): Promise<IDBDatabase | null> {
  if (typeof indexedDB === "undefined") return Promise.resolve(null);
  return new Promise((resolve) => {
    const request = indexedDB.open(DB_NAME, DB_VERSION);
    request.onupgradeneeded = (): void => {
      if (!request.result.objectStoreNames.contains(STORE)) request.result.createObjectStore(STORE);
    };
    request.onsuccess = (): void => resolve(request.result);
    request.onerror = (): void => resolve(null);
    request.onblocked = (): void => resolve(null);
  });
}

async function withStore<T>(mode: IDBTransactionMode, body: (store: IDBObjectStore) => IDBRequest<T>): Promise<T | null> {
  const db = await openDb();
  if (!db) return null;
  return new Promise((resolve) => {
    try {
      const request = body(db.transaction(STORE, mode).objectStore(STORE));
      request.onsuccess = (): void => resolve(request.result);
      request.onerror = (): void => resolve(null);
    } catch {
      resolve(null);
    }
  });
}

/** The kept Voland folder, if one was chosen in this browser. */
export async function keptFolder(): Promise<FileSystemDirectoryHandle | null> {
  const value = await withStore<unknown>("readonly", (store) => store.get(ROOT_KEY));
  return value instanceof Object && (value as FileSystemHandle).kind === "directory" ? value as FileSystemDirectoryHandle : null;
}

export async function keepFolder(handle: FileSystemDirectoryHandle | null): Promise<void> {
  if (handle) await withStore("readwrite", (store) => store.put(handle, ROOT_KEY));
  else await withStore("readwrite", (store) => store.delete(ROOT_KEY));
}

/* Permission methods of File System Access handles (not in TS's DOM lib). */
interface PermissionedHandle {
  queryPermission?: (options: { readonly mode: "read" | "readwrite" }) => Promise<PermissionState>;
  requestPermission?: (options: { readonly mode: "read" | "readwrite" }) => Promise<PermissionState>;
}

/** The folder's permission now, without asking. */
export async function folderPermission(handle: FileSystemDirectoryHandle, mode: "read" | "readwrite" = "read"): Promise<PermissionState> {
  return await (handle as unknown as PermissionedHandle).queryPermission?.({ mode }).catch(() => "denied" as const) ?? "granted";
}

/** Asks for permission (must run inside a click). */
export async function askFolderPermission(handle: FileSystemDirectoryHandle, mode: "read" | "readwrite" = "read"): Promise<boolean> {
  if (await folderPermission(handle, mode) === "granted") return true;
  const asked = await (handle as unknown as PermissionedHandle).requestPermission?.({ mode }).catch(() => "denied" as const);
  return (asked ?? "denied") === "granted";
}

/** Opens the system folder picker for the Voland folder. */
export async function pickVolandFolder(): Promise<FileSystemDirectoryHandle | null> {
  const picker = (window as unknown as { showDirectoryPicker?: (o: object) => Promise<FileSystemDirectoryHandle> }).showDirectoryPicker;
  if (!picker) return null;
  try {
    return await picker({ id: "voland-folder", mode: "read" });
  } catch {
    return null; /* cancelled */
  }
}

/** Creates the missing subfolders and the README (asks for write access;
 * must run inside a click). False if refused. */
export async function setUpFolder(handle: FileSystemDirectoryHandle): Promise<boolean> {
  if (!await askFolderPermission(handle, "readwrite")) return false;
  try {
    for (const dir of [GAMES_DIR, FIRMWARE_DIR, MODS_DIR]) await handle.getDirectoryHandle(dir, { create: true });
    const readme = await handle.getFileHandle(README_NAME, { create: true });
    if ((await readme.getFile()).size === 0) {
      const writable = await readme.createWritable();
      await writable.write(README_TEXT);
      await writable.close();
    }
    return true;
  } catch {
    return false;
  }
}
