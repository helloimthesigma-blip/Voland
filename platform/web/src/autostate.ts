/**
 * Automatic save states: while a game runs, the whole machine is saved by
 * itself every few minutes and when the tab is hidden, one state per game
 * (the worker drops the older one). The library's "Continue" and the
 * crash panel's "Go back" load it. A state freezes the game for a moment,
 * so the interval is a setting (AutoStateSetting.tsx); 0 turns it off.
 */
import { getGuestConsole, subscribeGuestConsole } from "./guest-console.ts";
import { saveState } from "./savestates.ts";

const STORAGE_KEY = "voland.autoStateMinutes";
export const AUTO_STATE_CHOICES: readonly number[] = [0, 5, 10, 20];
export const DEFAULT_AUTO_STATE_MINUTES = 10;
const MS_PER_MINUTE = 60_000;
/** How often the timer looks whether a state is due. */
const CHECK_MS = 15_000;
/** A hidden tab saves only if the last state is at least this old. */
const HIDDEN_MIN_AGE_MS = 60_000;
/** A game's first automatic state comes this long after it starts (the
 * boot itself is not worth keeping). */
const FIRST_STATE_MS = 2 * MS_PER_MINUTE;

/** ?autostate=0 (tools/perf.mjs): none this session, whatever the setting. */
const DISABLED_BY_URL = typeof location !== "undefined" && new URLSearchParams(location.search).get("autostate") === "0";

export function autoStateMinutes(): number {
  if (DISABLED_BY_URL) return 0;
  try {
    const value = Number(localStorage.getItem(STORAGE_KEY) ?? String(DEFAULT_AUTO_STATE_MINUTES));
    return AUTO_STATE_CHOICES.includes(value) ? value : DEFAULT_AUTO_STATE_MINUTES;
  } catch {
    return DEFAULT_AUTO_STATE_MINUTES;
  }
}

export function setAutoStateMinutes(minutes: number): void {
  try {
    localStorage.setItem(STORAGE_KEY, String(minutes));
  } catch {
    /* not remembered: this session's default */
  }
}

/** When the next automatic state is due, given when the game started
 * running and when the last state was taken (null: none yet). Pure. */
export function nextAutoStateAt(startedAt: number, lastAt: number | null, minutes: number): number {
  if (minutes <= 0) return Number.POSITIVE_INFINITY;
  return lastAt === null ? startedAt + Math.min(FIRST_STATE_MS, minutes * MS_PER_MINUTE) : lastAt + minutes * MS_PER_MINUTE;
}

let startedAt = 0;
let lastAt: number | null = null;
let saving = false;
const listeners = new Set<(at: number) => void>();

/** Called with the time of each automatic state taken. */
export function subscribeAutoStates(fn: (at: number) => void): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

async function take(): Promise<void> {
  if (saving || getGuestConsole().runState !== "running") return;
  saving = true;
  try {
    const result = await saveState(true);
    lastAt = Date.now();
    if (result.ok) listeners.forEach((fn) => fn(lastAt ?? 0));
  } finally {
    saving = false;
  }
}

/** Starts the timer (main.ts, once). */
export function startAutoStates(): void {
  let wasRunning = false;
  subscribeGuestConsole((state) => {
    const running = state.runState === "running";
    /* A new game (the console resets to idle on each load). */
    if (state.runState === "idle") {
      wasRunning = false;
      lastAt = null;
    }
    if (running && !wasRunning && lastAt === null) startedAt = Date.now();
    wasRunning = running || (wasRunning && state.runState === "paused");
  });
  window.setInterval(() => {
    if (Date.now() >= nextAutoStateAt(startedAt, lastAt, autoStateMinutes())) void take();
  }, CHECK_MS);
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState !== "hidden" || autoStateMinutes() === 0) return;
    if (Date.now() - (lastAt ?? startedAt) >= HIDDEN_MIN_AGE_MS) void take();
  });
}
