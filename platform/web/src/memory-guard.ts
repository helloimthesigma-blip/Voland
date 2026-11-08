/**
 * A warning before a heavy game loads: the browser can freeze when the
 * machine runs out of memory (TOTK needed ~8 GB of Chrome's). Each game's
 * guest RAM use is remembered when it runs (the CPU worker reports it
 * every 30 s), so the next launch can say how much it needed; a game never
 * run here is judged by its file size. Per browser; storage may be
 * unavailable, which only loses the warning.
 */
const STORAGE_KEY = "voland.memoryUse.v1";
const GIB = 1024 ** 3;
/** Guest RAM a game needs before it counts as heavy (the browser needs
 * graphics memory and its own on top). */
export const HEAVY_GUEST_RAM_BYTES = 2.5 * GIB;
/** A game file this big (never run here) is probably heavy too. */
export const HEAVY_FILE_BYTES = 8 * GIB;
/** navigator.deviceMemory below this (GB; Chrome reports at most 8)
 * makes the warning stronger. */
const LOW_DEVICE_MEMORY_GB = 8;

function stored(): Record<string, number> {
  try {
    const parsed: unknown = JSON.parse(globalThis.localStorage?.getItem(STORAGE_KEY) ?? "{}");
    if (typeof parsed !== "object" || parsed === null) return {};
    return Object.fromEntries(Object.entries(parsed).filter(([, v]) => typeof v === "number")) as Record<string, number>;
  } catch {
    return {};
  }
}

/** Remembers the most guest RAM `titleId` has used. */
export function recordMemoryUse(titleId: string, guestRamBytes: number): void {
  const all = stored();
  if ((all[titleId] ?? 0) >= guestRamBytes) return;
  try {
    globalThis.localStorage?.setItem(STORAGE_KEY, JSON.stringify({ ...all, [titleId]: guestRamBytes }));
  } catch {
    /* storage blocked */
  }
}

export function memoryUseOf(titleId: string): number | null {
  return stored()[titleId] ?? null;
}

/** What to tell the player before loading, or null for a light game.
 * `usedBytes`: the game's remembered guest RAM use (null: never run
 * here); `deviceMemoryGb`: navigator.deviceMemory if the browser has it.
 * Pure. */
export function memoryWarning(usedBytes: number | null, fileBytes: number, deviceMemoryGb: number | null): string | null {
  const heavy = usedBytes !== null ? usedBytes >= HEAVY_GUEST_RAM_BYTES : fileBytes >= HEAVY_FILE_BYTES;
  if (!heavy) return null;
  const what = usedBytes !== null
    ? `Last time it used ${(usedBytes / GIB).toFixed(1)} GB for the game alone, and the browser needs more on top.`
    : `It is a big game (${(fileBytes / GIB).toFixed(0)} GB), so it will likely need a lot of memory.`;
  const low = deviceMemoryGb !== null && deviceMemoryGb < LOW_DEVICE_MEMORY_GB
    ? ` This device reports only ${deviceMemoryGb} GB of memory, so it may not run at all.` : "";
  return `Heavy game. ${what}${low} Close other tabs and apps first; if memory runs out, the browser can freeze.`;
}
