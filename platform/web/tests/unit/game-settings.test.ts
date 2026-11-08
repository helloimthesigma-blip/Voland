/**
 * Per-game settings (src/game-settings.ts): a game's own value, the
 * default, and going back to it.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

const items = new Map<string, string>();
(globalThis as { localStorage?: unknown }).localStorage = {
  getItem: (k: string): string | null => items.get(k) ?? null,
  setItem: (k: string, v: string): void => { items.set(k, v); },
  removeItem: (k: string): void => { items.delete(k); },
};

const { hasOwnSetting, rememberSetting, settingFor } = await import("../../src/game-settings.ts");

test("a game's own value wins over the default, which wins over the fallback", () => {
  assert.equal(settingFor("01", "frameSkip", 0), 0);
  rememberSetting(null, "frameSkip", 1);
  assert.equal(items.get("voland.frameSkip"), "1"); /* the pre-per-game key */
  assert.equal(settingFor("01", "frameSkip", 0), 1);
  rememberSetting("01", "frameSkip", 3);
  assert.equal(settingFor("01", "frameSkip", 0), 3);
  assert.equal(settingFor("02", "frameSkip", 0), 1);
  assert.equal(hasOwnSetting("01", "frameSkip"), true);
  assert.equal(hasOwnSetting("01", "hostCores"), false);
});

test("dropping a game's own value goes back to the default", () => {
  rememberSetting("01", "hostCores", 0);
  rememberSetting("01", "frameSkip", undefined);
  assert.equal(settingFor("01", "frameSkip", 0), 1);
  assert.equal(settingFor("01", "hostCores", -1), 0);
});
