/**
 * Settings per game: a setting changed while a game runs is remembered
 * for that game and comes back each time it loads; changed with no game
 * running, it is the default for every game without its own. Today:
 * frame skip and guest CPU threads (FrameSkipSetting, ThreadsSetting).
 * Per browser; storage may be unavailable, which only forgets.
 */
export type GameSettingKey = "frameSkip" | "hostCores";

const STORAGE_KEY = "voland.gameSettings.v1";
/** The defaults' keys from before per-game settings (kept as they were). */
const DEFAULT_KEYS: Readonly<Record<GameSettingKey, string>> = { frameSkip: "voland.frameSkip", hostCores: "voland.hostCores" };

type PerGame = Readonly<Record<string, Readonly<Partial<Record<GameSettingKey, number>>>>>;

function readAll(): PerGame {
  try {
    const parsed: unknown = JSON.parse(globalThis.localStorage?.getItem(STORAGE_KEY) ?? "{}");
    return typeof parsed === "object" && parsed !== null ? parsed as PerGame : {};
  } catch {
    return {};
  }
}

function write(key: string, value: string | null): void {
  try {
    if (value === null) globalThis.localStorage?.removeItem(key);
    else globalThis.localStorage?.setItem(key, value);
  } catch {
    /* not remembered - still applied */
  }
}

/** A game's own value, the default's, then `fallback`. Pure but for storage. */
export function settingFor(titleId: string | null, key: GameSettingKey, fallback: number): number {
  const own = titleId ? readAll()[titleId]?.[key] : undefined;
  if (typeof own === "number") return own;
  try {
    const stored = globalThis.localStorage?.getItem(DEFAULT_KEYS[key]);
    return stored !== null && stored !== undefined && Number.isFinite(Number(stored)) ? Number(stored) : fallback;
  } catch {
    return fallback;
  }
}

/** Whether `titleId` has its own value for `key`. */
export function hasOwnSetting(titleId: string, key: GameSettingKey): boolean {
  return typeof readAll()[titleId]?.[key] === "number";
}

/** Remembers `value` for `titleId`, or as the default when null; a
 * game's value of undefined drops its own (back to the default). */
export function rememberSetting(titleId: string | null, key: GameSettingKey, value: number | undefined): void {
  if (!titleId) {
    if (value !== undefined) write(DEFAULT_KEYS[key], String(value));
    return;
  }
  const all = readAll();
  const own: Partial<Record<GameSettingKey, number>> = { ...all[titleId] };
  if (value === undefined) delete own[key];
  else own[key] = value;
  write(STORAGE_KEY, JSON.stringify({ ...all, [titleId]: own }));
}

/* The game whose settings apply now (LoadPanel sets it on each load). */
export interface CurrentGame {
  readonly titleId: string;
  readonly name: string;
}
let current: CurrentGame | null = null;
const listeners = new Set<(game: CurrentGame | null) => void>();

export function currentGame(): CurrentGame | null {
  return current;
}

export function setCurrentGame(game: CurrentGame | null): void {
  current = game;
  listeners.forEach((fn) => fn(game));
}

export function subscribeCurrentGame(fn: (game: CurrentGame | null) => void): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}
