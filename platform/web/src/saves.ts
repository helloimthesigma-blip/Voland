/**
 * Game-save backup (§15): asks the CPU worker for a tar of every stored
 * save, or hands it one to import (workers/save-store.ts). Lifecycle-only
 * messages, one reply per request, answered in order.
 */
import type { CPUToMainMessage, MainToCPUMessage } from "@bindings/protocol";

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
