import { existsSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";
import { openTab } from "./tabs.ts";

/**
 * Settings per game (src/game-settings.ts): frame skip chosen while a game
 * runs is that game's; the page opens with the default and the game's own
 * value comes back when it loads; "Use the default" drops it.
 */
const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

test("a setting changed in a game is kept for that game", async ({ page }) => {
  const demo = join(mkdtempSync(join(tmpdir(), "voland-settings-")), "hello.nro");
  writeFileSync(demo, readFileSync(new URL("../public/demo/hello.nro", import.meta.url)));
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await openTab(page, "settings");
  const skip = page.getByTestId("frame-skip");
  await expect(skip.getByTestId("setting-scope")).toHaveText("For all games.");
  await page.getByTestId("load-input").setInputFiles(demo);
  await expect(page.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
  await expect(skip.getByTestId("setting-scope")).toContainText("a change here is kept for hello only");
  await skip.locator("select").selectOption("2");
  await expect(skip.getByTestId("setting-scope")).toContainText("For hello only.");

  await page.reload();
  await openTab(page, "settings");
  await expect(skip.locator("select")).toHaveValue("0", { timeout: 20_000 });
  await page.getByTestId("load-input").setInputFiles(demo);
  await expect(page.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
  await expect(skip.locator("select")).toHaveValue("2");
  await skip.getByRole("button", { name: "Use the default" }).click();
  await expect(skip.locator("select")).toHaveValue("0");
});
