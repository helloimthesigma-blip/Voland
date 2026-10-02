/**
 * The emulated SD card's persistence in the browser (§15: OPFS for
 * emulator-managed data). Files the user adds are mirrored under an
 * "sdmc" directory in the origin-private file system and restored into
 * the core's SD card when the worker starts, so a homebrew library
 * survives reloads. What the guest itself writes is mirrored too: the
 * CPU worker diffs the core's SD manifest every few seconds
 * (diffManifests) and stores or removes the files that changed.
 *
 * Guest save data travels the same way: the core names a save's files
 * "save:SS:<attribute hex>/path" (space SS, the SaveDataAttribute bytes),
 * stored here under "saves/SS-<attribute hex>/path" so no OPFS name needs
 * a colon, and restored under the core's name.
 */
const SD_DIRECTORY = "sdmc";
const SAVES_DIRECTORY = "saves";
const SAVE_PREFIX = "save:";
const SAVE_HEADER = /^save:([0-9a-f]{2}):([0-9a-f]+)(\/.*)?$/i;
const SAVE_DIRECTORY_NAME = /^([0-9a-f]{2})-([0-9a-f]+)$/i;

async function topDirectory(name: string): Promise<FileSystemDirectoryHandle | null> {
  if (typeof navigator === "undefined" || !navigator.storage?.getDirectory) return null;
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle(name, { create: true });
}

function segments(path: string): string[] {
  return path.split("/").filter((part) => part.length > 0 && part !== "." && part !== "..");
}

export interface StoredLocation {
  readonly directory: string;
  readonly parts: readonly string[];
}

/** Where a core path lives in OPFS: SD paths under "sdmc", save paths
 * under "saves/SS-<hex>". Null for a malformed save path. Pure. */
export function storedLocation(path: string): StoredLocation | null {
  if (!path.startsWith(SAVE_PREFIX)) return { directory: SD_DIRECTORY, parts: segments(path) };
  const match = SAVE_HEADER.exec(path);
  if (!match) return null;
  return { directory: SAVES_DIRECTORY, parts: [`${match[1]}-${match[2]}`, ...segments(match[3] ?? "")] };
}

/** The core path of a file found at `parts` under the "saves" directory. Pure. */
export function savePathFromStored(parts: readonly string[]): string | null {
  const match = parts.length >= 2 ? SAVE_DIRECTORY_NAME.exec(parts[0] ?? "") : null;
  if (!match) return null;
  return `${SAVE_PREFIX}${match[1]}:${match[2]}/${parts.slice(1).join("/")}`;
}

/** Stores `bytes` at core path `path` (e.g. "/switch/app.nro"). */
export async function persistSdFile(path: string, bytes: Uint8Array): Promise<boolean> {
  const location = storedLocation(path);
  const root = location ? await topDirectory(location.directory) : null;
  if (!root || !location) return false;
  const parts = [...location.parts];
  const name = parts.pop();
  if (!name) return false;
  let dir = root;
  for (const part of parts) dir = await dir.getDirectoryHandle(part, { create: true });
  const file = await dir.getFileHandle(name, { create: true });
  const writable = await file.createWritable();
  await writable.write(new Uint8Array(bytes)); // a copy: the source may view shared memory
  await writable.close();
  return true;
}

/** Calls `restore` for every stored file (SD files and saves); returns
 * how many it accepted. */
export async function restoreSdFiles(restore: (path: string, bytes: Uint8Array) => boolean): Promise<number> {
  let restored = 0;
  const walk = async (dir: FileSystemDirectoryHandle, parts: readonly string[], toPath: (parts: readonly string[]) => string | null): Promise<void> => {
    for await (const [name, handle] of dir.entries()) {
      const next = [...parts, name];
      if (handle.kind === "directory") {
        await walk(handle as FileSystemDirectoryHandle, next, toPath);
      } else {
        const path = toPath(next);
        if (!path) continue;
        const file = await (handle as FileSystemFileHandle).getFile();
        if (restore(path, new Uint8Array(await file.arrayBuffer()))) restored++;
      }
    }
  };
  const sd = await topDirectory(SD_DIRECTORY);
  if (sd) await walk(sd, [], (parts) => `/${parts.join("/")}`);
  const saves = await topDirectory(SAVES_DIRECTORY);
  if (saves) await walk(saves, [], savePathFromStored);
  return restored;
}

/** Removes every stored SD file. */
export async function clearSdFiles(): Promise<void> {
  if (typeof navigator === "undefined" || !navigator.storage?.getDirectory) return;
  const root = await navigator.storage.getDirectory();
  await root.removeEntry(SD_DIRECTORY, { recursive: true }).catch(() => undefined);
}

/** Removes one stored file (missing files are fine). */
export async function removeSdFile(path: string): Promise<void> {
  const location = storedLocation(path);
  const root = location ? await topDirectory(location.directory) : null;
  if (!root || !location) return;
  const parts = [...location.parts];
  const name = parts.pop();
  if (!name) return;
  try {
    let dir = root;
    for (const part of parts) dir = await dir.getDirectoryHandle(part);
    await dir.removeEntry(name);
  } catch {
    /* already gone */
  }
}

/** Parses the core's manifest: path -> "version size". */
export function parseManifest(text: string): ReadonlyMap<string, string> {
  const entries = new Map<string, string>();
  for (const line of text.split("\n")) {
    const first = line.indexOf(" ");
    const second = first < 0 ? -1 : line.indexOf(" ", first + 1);
    if (second < 0) continue;
    entries.set(line.slice(second + 1), line.slice(0, second));
  }
  return entries;
}

export interface ManifestDiff {
  readonly changed: readonly string[];
  readonly removed: readonly string[];
}

/** What to store and what to remove to go from `before` to `after`. */
export function diffManifests(before: ReadonlyMap<string, string>, after: ReadonlyMap<string, string>): ManifestDiff {
  const changed: string[] = [];
  const removed: string[] = [];
  for (const [path, key] of after) if (before.get(path) !== key) changed.push(path);
  for (const path of before.keys()) if (!after.has(path)) removed.push(path);
  return { changed, removed };
}
