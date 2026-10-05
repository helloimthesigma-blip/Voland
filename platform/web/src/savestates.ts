/**
 * Save states (workers/savestate.ts) from the main thread: lifecycle
 * messages to the CPU worker, one reply per request, answered in order.
 */
import type { CPUToMainMessage, MainToCPUMessage, SavestateInfo } from "@bindings/protocol";

export interface StateResult {
  readonly ok: boolean;
  readonly error: string;
  readonly info: SavestateInfo | null;
}

let worker: Worker | null = null;
const pendingSaves: ((result: StateResult) => void)[] = [];
const pendingLoads: ((result: StateResult) => void)[] = [];
const pendingLists: ((states: readonly SavestateInfo[]) => void)[] = [];
const pendingDeletes: (() => void)[] = [];

export function registerSavestateWorker(cpuWorker: Worker): void {
  worker = cpuWorker;
}

/** The CPU worker's replies; true if `msg` was one. */
export function handleSavestateMessage(msg: CPUToMainMessage): boolean {
  switch (msg.type) {
    case "state-saved":
      pendingSaves.shift()?.({ ok: msg.info !== null, error: msg.error, info: msg.info });
      return true;
    case "state-loaded":
      pendingLoads.shift()?.({ ok: msg.ok, error: msg.error, info: null });
      return true;
    case "states-listed":
      pendingLists.shift()?.(msg.states);
      return true;
    case "state-deleted":
      pendingDeletes.shift()?.();
      return true;
    default:
      return false;
  }
}

function request<T>(queue: ((value: T) => void)[], message: MainToCPUMessage, fallback: T): Promise<T> {
  const target = worker;
  if (!target) return Promise.resolve(fallback);
  return new Promise((resolve) => {
    queue.push(resolve);
    target.postMessage(message);
  });
}

const NO_WORKER: StateResult = { ok: false, error: "the emulator is not running", info: null };

export function saveState(): Promise<StateResult> {
  return request(pendingSaves, { type: "save-state" }, NO_WORKER);
}

export function loadState(id: string): Promise<StateResult> {
  return request(pendingLoads, { type: "load-state", id }, NO_WORKER);
}

export function listStates(): Promise<readonly SavestateInfo[]> {
  return request(pendingLists, { type: "list-states" }, []);
}

export function deleteState(id: string): Promise<void> {
  return request(pendingDeletes, { type: "delete-state", id }, undefined);
}
