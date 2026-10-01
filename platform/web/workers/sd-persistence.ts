/**
 * The emulated SD card's persistence in the browser (§15: OPFS for
 * emulator-managed data). Files the user adds are mirrored under an
 * "sdmc" directory in the origin-private file system and restored into
 * the core's SD card when the worker starts, so a homebrew library
 * survives reloads. (What the guest itself writes to the SD card is not
 * mirrored yet - that is the save-management task, §25 Phase 6.)
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
