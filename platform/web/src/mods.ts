/**
 * Mods from the page (§19): the running game's applied mods and cheats as
 * the CPU worker reports them after a load, and cheat toggles back to it.
 * Installing, ordering and enabling mods is workers/mod-store.ts (OPFS),
 * which the page uses directly; changes apply at the next load.
 */
import type { CheatInfo, CPUToMainMessage } from "@bindings/protocol";

export interface RunningMods {
  readonly titleId: string;
  /** The main executable's build id (hex), for matching patches. */
  readonly buildId: string;
  readonly mods: readonly string[];
  readonly cheats: readonly CheatInfo[];
}

let worker: Worker | null = null;
let current: RunningMods | null = null;
const listeners = new Set<(state: RunningMods | null) => void>();

export function registerModsWorker(cpuWorker: Worker): void {
  worker = cpuWorker;
}

/** The CPU worker's "cheats" report; true if `msg` was one. */
export function handleModsMessage(msg: CPUToMainMessage): boolean {
  if (msg.type !== "cheats") return false;
  current = { titleId: msg.titleId, buildId: msg.buildId, mods: msg.mods, cheats: msg.cheats };
  for (const listener of listeners) listener(current);
  return true;
}

export function runningMods(): RunningMods | null {
  return current;
}

export function subscribeRunningMods(listener: (state: RunningMods | null) => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

export function setCheatEnabled(index: number, enabled: boolean): void {
  worker?.postMessage({ type: "set-cheat", index, enabled });
}
