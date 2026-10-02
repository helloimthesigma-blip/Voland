/**
 * The emulated SD card's persistence in the browser (§15: OPFS for
 * emulator-managed data). Files the user adds are mirrored under an
 * "sdmc" directory in the origin-private file system and restored into
 * the core's SD card when the worker starts, so a homebrew library
 * survives reloads. What the guest itself writes is mirrored too: the
 * CPU worker diffs the core's SD manifest every few seconds
 * (diffManifests) and stores or removes the files that changed.
 */
const SD_DIRECTORY = "sdmc";

async function sdRoot(): Promise<FileSystemDirectoryHandle | null> {
  if (typeof navigator === "undefined" || !navigator.storage?.getDirectory) return null;
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle(SD_DIRECTORY, { create: true });
}

function segments(path: string): string[] {
  return path.split("/").filter((part) => part.length > 0 && part !== "." && part !== "..");
}

/** Stores `bytes` at SD path `path` (e.g. "/switch/app.nro"). */
export async function persistSdFile(path: string, bytes: Uint8Array): Promise<boolean> {
  const root = await sdRoot();
  if (!root) return false;
  const parts = segments(path);
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

/** Calls `restore` for every stored file; returns how many it accepted. */
export async function restoreSdFiles(restore: (path: string, bytes: Uint8Array) => boolean): Promise<number> {
  const root = await sdRoot();
  if (!root) return 0;
  let restored = 0;
  const walk = async (dir: FileSystemDirectoryHandle, prefix: string): Promise<void> => {
    for await (const [name, handle] of dir.entries()) {
      const path = `${prefix}/${name}`;
      if (handle.kind === "directory") {
        await walk(handle as FileSystemDirectoryHandle, path);
      } else {
        const file = await (handle as FileSystemFileHandle).getFile();
        if (restore(path, new Uint8Array(await file.arrayBuffer()))) restored++;
      }
    }
  };
  await walk(root, "");
  return restored;
}

/** Removes every stored SD file. */
export async function clearSdFiles(): Promise<void> {
  if (typeof navigator === "undefined" || !navigator.storage?.getDirectory) return;
  const root = await navigator.storage.getDirectory();
  await root.removeEntry(SD_DIRECTORY, { recursive: true }).catch(() => undefined);
}

/** Removes one stored SD file (missing files are fine). */
export async function removeSdFile(path: string): Promise<void> {
  const root = await sdRoot();
  if (!root) return;
  const parts = segments(path);
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
