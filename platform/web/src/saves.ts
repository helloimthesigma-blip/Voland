/**
 * Game-save backup (§15): asks the CPU worker for a tar of every stored
 * save, or hands it one to import (workers/save-store.ts). Lifecycle-only
 * messages, one reply per request, answered in order.
 */
import type { CPUToMainMessage, MainToCPUMessage, StoredSave } from "@bindings/protocol";

export interface SavesExport {
  readonly tar: ArrayBuffer;
  readonly count: number;
}
export interface SavesImport {
  readonly imported: number;
  readonly rejected: number;
}

let worker: Worker | null = null;
const pendingExports: ((result: SavesExport) => void)[] = [];
const pendingImports: ((result: SavesImport) => void)[] = [];
const pendingLists: ((saves: readonly StoredSave[]) => void)[] = [];
const pendingPuts: ((ok: boolean) => void)[] = [];
const pendingDeletes: ((ok: boolean) => void)[] = [];

export function registerSavesWorker(cpuWorker: Worker): void {
  worker = cpuWorker;
}

/** The CPU worker's replies; true if `msg` was one. */
export function handleSavesMessage(msg: CPUToMainMessage): boolean {
  if (msg.type === "saves-exported") {
    pendingExports.shift()?.({ tar: msg.tar, count: msg.count });
    return true;
  }
  if (msg.type === "saves-imported") {
    pendingImports.shift()?.({ imported: msg.imported, rejected: msg.rejected });
    return true;
  }
  if (msg.type === "saves-listed") {
    pendingLists.shift()?.(msg.saves);
    return true;
  }
  if (msg.type === "save-put") {
    pendingPuts.shift()?.(msg.ok);
    return true;
  }
  if (msg.type === "save-deleted") {
    pendingDeletes.shift()?.(msg.ok);
    return true;
  }
  return false;
}

export function exportSaves(): Promise<SavesExport> {
  const target = worker;
  if (!target) return Promise.resolve({ tar: new ArrayBuffer(0), count: 0 });
  return new Promise((resolve) => {
    pendingExports.push(resolve);
    target.postMessage({ type: "export-saves" } satisfies MainToCPUMessage);
  });
}

export async function importSaves(file: File): Promise<SavesImport> {
  const target = worker;
  if (!target) return { imported: 0, rejected: 0 };
  const tar = await file.arrayBuffer();
  return new Promise((resolve) => {
    pendingImports.push(resolve);
    target.postMessage({ type: "import-saves", tar } satisfies MainToCPUMessage, [tar]);
  });
}

/** "voland-saves-2026-10-04.tar". Pure. */
export function exportFileName(now: Date): string {
  return `voland-saves-${now.toISOString().slice(0, 10)}.tar`;
}

/** Every stored save (the save browser). */
export function listSaves(): Promise<readonly StoredSave[]> {
  const target = worker;
  if (!target) return Promise.resolve([]);
  return new Promise((resolve) => {
    pendingLists.push(resolve);
    target.postMessage({ type: "list-saves" } satisfies MainToCPUMessage);
  });
}

/** Replaces save `name` with `archive` (loaded into the core and stored);
 * false if it was refused (in use by the running game, or damaged). */
export function putSave(name: string, archive: Uint8Array): Promise<boolean> {
  const target = worker;
  if (!target) return Promise.resolve(false);
  const copy = archive.slice().buffer as ArrayBuffer;
  return new Promise((resolve) => {
    pendingPuts.push(resolve);
    target.postMessage({ type: "put-save", name, archive: copy } satisfies MainToCPUMessage, [copy]);
  });
}

export function deleteSave(name: string): Promise<boolean> {
  const target = worker;
  if (!target) return Promise.resolve(false);
  return new Promise((resolve) => {
    pendingDeletes.push(resolve);
    target.postMessage({ type: "delete-save", name } satisfies MainToCPUMessage);
  });
}

/* Game names for the save browser: a save only knows its program id, so
 * the file name each title was loaded from is remembered per browser. */
const TITLE_NAMES_KEY = "voland.titleNames.v1";

export function titleNames(): Readonly<Record<string, string>> {
  try {
    const parsed: unknown = JSON.parse(globalThis.localStorage?.getItem(TITLE_NAMES_KEY) ?? "{}");
    if (typeof parsed !== "object" || parsed === null) return {};
    return Object.fromEntries(Object.entries(parsed).filter(([, v]) => typeof v === "string")) as Record<string, string>;
  } catch {
    return {};
  }
}

/** Remembers that `titleId` ("010013C00E930000") was loaded from `fileName`. */
export function rememberTitleName(titleId: string, fileName: string): void {
  const name = fileName.replace(/\.(nca|nro|nsp|xci)$/i, "");
  try {
    globalThis.localStorage?.setItem(TITLE_NAMES_KEY, JSON.stringify({ ...titleNames(), [titleId]: name }));
  } catch {
    /* storage blocked */
  }
}
