import { readFileSync } from "node:fs";

import { expect, test } from "@playwright/test";

/**
 * The game library remembers what was loaded: a tile appears after the
 * first load, survives a reload, launches the game again, and can be
 * removed. (The plain file input keeps no handle, so the tile says it
 * asks for the file; the File System Access picker path needs a real
 * dialog and is not driven here.)
 */
test("loaded games appear in the library and can be forgotten", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("library-empty")).toBeVisible({ timeout: 20_000 });
  const demo = readFileSync(new URL("../public/demo/hello.nro", import.meta.url));
  await page.getByTestId("load-input").setInputFiles({ name: "hello.nro", mimeType: "application/octet-stream", buffer: demo });
  await expect(page.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
  const tile = page.getByTestId("library-game");
  await expect(tile).toHaveCount(1);
  await expect(tile).toContainText("hello");
  await expect(tile).toContainText("played today");

  await page.reload();
  await expect(page.getByTestId("library-game")).toContainText("hello", { timeout: 20_000 });
  await page.getByTestId("library-game").hover();
  await page.getByTestId("library-forget").click();
  await expect(page.getByTestId("library-empty")).toBeVisible();
});
