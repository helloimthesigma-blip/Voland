/**
 * The player's control bindings for this browser (bindings.ts). A
 * per-viewer convenience kept in localStorage; when storage is blocked
 * (private windows, cleared site data) the defaults apply and changes
 * last for the session.
 */
import { type Bindings, DEFAULT_BINDINGS, sanitizeBindings } from "./bindings.ts";

const STORAGE_KEY = "voland.bindings.v1";

function load(): Bindings {
  try {
    const text = globalThis.localStorage?.getItem(STORAGE_KEY);
    return text ? sanitizeBindings(JSON.parse(text)) : DEFAULT_BINDINGS;
  } catch {
    return DEFAULT_BINDINGS;
  }
}

let current: Bindings = load();
const listeners = new Set<(bindings: Bindings) => void>();

export function getBindings(): Bindings {
  return current;
}

export function setBindings(next: Bindings): void {
  current = next;
  try {
    globalThis.localStorage?.setItem(STORAGE_KEY, JSON.stringify(next));
  } catch {
    /* storage blocked: this session only */
  }
  for (const listener of listeners) listener(next);
}

export function subscribeBindings(listener: (bindings: Bindings) => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}
