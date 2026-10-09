/**
 * The game library (§15 "library"): games the player has loaded, kept in
 * IndexedDB as File System Access handles so a game launches again with
 * one click (and one permission prompt per browser session) instead of
 * re-picking a multi-gigabyte file. Browsers without
 * showOpenFilePicker keep the entry but ask for the file again when it is
 * launched. Only handles and labels are stored - never the game's bytes.
 */

/* Permission methods of File System Access handles (not in TS's DOM lib). */
interface PermissionedHandle {
  queryPermission?: (options: { readonly mode: "read" }) => Promise<PermissionState>;
  requestPermission?: (options: { readonly mode: "read" }) => Promise<PermissionState>;
}

export interface LibraryGame {
  /** Stable key: name + size + last-modified of the file it came from. */
  readonly id: string;
  readonly name: string;
  readonly fileName: string;
  readonly size: number;
  readonly titleId: string | null;
  readonly addedAt: number;
  readonly lastPlayedAt: number | null;
  /** Absent where the browser cannot keep file handles. */
  readonly handle: FileSystemFileHandle | null;
}

const DB_NAME = "voland-library";
const DB_VERSION = 1;
const STORE = "games";

function openDb(): Promise<IDBDatabase | null> {
  if (typeof indexedDB === "undefined") return Promise.resolve(null);
  return new Promise((resolve) => {
    const request = indexedDB.open(DB_NAME, DB_VERSION);
    request.onupgradeneeded = (): void => {
      if (!request.result.objectStoreNames.contains(STORE)) request.result.createObjectStore(STORE, { keyPath: "id" });
    };
    request.onsuccess = (): void => resolve(request.result);
    request.onerror = (): void => resolve(null);
    request.onblocked = (): void => resolve(null);
  });
}

function run<T>(mode: IDBTransactionMode, body: (store: IDBObjectStore) => IDBRequest<T>): Promise<T | null> {
  return openDb().then((db) => new Promise<T | null>((resolve) => {
    if (!db) {
      resolve(null);
      return;
    }
    try {
      const request = body(db.transaction(STORE, mode).objectStore(STORE));
      request.onsuccess = (): void => resolve(request.result);
      request.onerror = (): void => resolve(null);
    } catch {
      resolve(null);
    }
  }));
}

/** The key a file is remembered under. Pure. */
export function gameIdOf(file: { readonly name: string; readonly size: number; readonly lastModified: number }): string {
  return `${file.name}:${file.size}:${file.lastModified}`;
}

/** "silksong.nca" -> "silksong". Pure. */
export function displayNameOf(fileName: string): string {
  return fileName.replace(/\.(nca|nro|nsp|xci)$/i, "") || fileName;
}

function isGame(value: unknown): value is LibraryGame {
  if (typeof value !== "object" || value === null) return false;
  const v = value as Record<string, unknown>;
  return typeof v["id"] === "string" && typeof v["fileName"] === "string" && typeof v["size"] === "number";
}

/** Every remembered game, most recently played (or added) first. */
export async function listGames(): Promise<readonly LibraryGame[]> {
  const all = await run<unknown[]>("readonly", (store) => store.getAll());
  return (all ?? []).filter(isGame).sort((a, b) =>
    (b.lastPlayedAt ?? b.addedAt) - (a.lastPlayedAt ?? a.addedAt));
}

/** Remembers `file` (with its handle when the browser gave one); keeps an
 * existing entry's title id and play time. Returns the entry. */
export async function rememberGame(file: File, handle: FileSystemFileHandle | null): Promise<LibraryGame> {
  const id = gameIdOf(file);
  const existing = await run<unknown>("readonly", (store) => store.get(id));
  const before = isGame(existing) ? existing : null;
  const game: LibraryGame = {
    id,
    name: before?.name ?? displayNameOf(file.name),
    fileName: file.name,
    size: file.size,
    titleId: before?.titleId ?? null,
    addedAt: before?.addedAt ?? Date.now(),
    lastPlayedAt: before?.lastPlayedAt ?? null,
    handle: handle ?? before?.handle ?? null,
  };
  await run("readwrite", (store) => store.put(game));
  return game;
}

/** Records a launch (and the title id the core reported). */
export async function markPlayed(id: string, titleId: string | null): Promise<void> {
  const existing = await run<unknown>("readonly", (store) => store.get(id));
  if (!isGame(existing)) return;
  await run("readwrite", (store) => store.put({ ...existing, titleId: titleId ?? existing.titleId, lastPlayedAt: Date.now() }));
}

export async function forgetGame(id: string): Promise<void> {
  await run("readwrite", (store) => store.delete(id));
}

/** The game's file, asking for read permission when the browser needs to
 * (must run inside a click). Null if there is no handle, permission was
 * refused, or the file is gone/changed. */
export async function openGameFile(game: LibraryGame): Promise<File | null> {
  const handle = game.handle;
  if (!handle) return null;
  try {
    const options = { mode: "read" } as const;
    const permissioned = handle as unknown as PermissionedHandle;
    let permission = await permissioned.queryPermission?.(options) ?? "granted";
    if (permission !== "granted") permission = await permissioned.requestPermission?.(options) ?? "denied";
    if (permission !== "granted") return null;
    const file = await handle.getFile();
    return gameIdOf(file) === game.id ? file : null;
  } catch {
    return null;
  }
}

/** Whether this browser can keep file handles (showOpenFilePicker). Not in
 * a frame: browsers block that picker in iframes, so a plain file input
 * (as the firmware folder uses) is the path that opens there. */
export function canKeepFiles(): boolean {
  if (typeof window === "undefined" || !("showOpenFilePicker" in window)) return false;
  try {
    return window.self === window.top;
  } catch {
    return false; /* cross-origin parent */
  }
}

/** Opens the system file picker; the chosen file and its handle, or null. */
export async function pickGameFile(): Promise<{ readonly file: File; readonly handle: FileSystemFileHandle } | null> {
  const picker = (window as unknown as { showOpenFilePicker?: (o: object) => Promise<FileSystemFileHandle[]> }).showOpenFilePicker;
  if (!picker) return null;
  try {
    const [handle] = await picker({ multiple: false, id: "voland-games" });
    if (!handle) return null;
    return { file: await handle.getFile(), handle };
  } catch {
    return null; /* cancelled */
  }
}
