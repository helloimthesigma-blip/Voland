/**
 * What the running guest printed (svcOutputDebugString) and whether it is
 * still running. Fed by main.ts from CPU-worker lifecycle messages; read
 * by the UI. State is replaced, never mutated (§3).
 */
import type { RunState } from "@bindings/load";

export interface GuestConsoleState {
  readonly lines: readonly string[];
  readonly runState: RunState | "idle";
  readonly detail: string;
}

const MAX_LINES = 1000;

let state: GuestConsoleState = { lines: [], runState: "idle", detail: "" };
const subscribers = new Set<(next: GuestConsoleState) => void>();

function publish(next: GuestConsoleState): void {
  state = next;
  subscribers.forEach((fn) => fn(state));
}

export function getGuestConsole(): GuestConsoleState {
  return state;
}

export function resetGuestConsole(): void {
  publish({ lines: [], runState: "idle", detail: "" });
}

export function appendGuestOutput(text: string): void {
  const lines = [...state.lines, ...text.split("\n")];
  publish({ ...state, lines: lines.length > MAX_LINES ? lines.slice(lines.length - MAX_LINES) : lines });
}

export function setGuestRunState(runState: RunState, detail: string): void {
  publish({ ...state, runState, detail });
}

export function subscribeGuestConsole(fn: (next: GuestConsoleState) => void): () => void {
  subscribers.add(fn);
  return () => subscribers.delete(fn);
}
