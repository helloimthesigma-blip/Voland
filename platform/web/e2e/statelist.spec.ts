import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

/**
 * Save states across a page reload: a state saved in one session is
 * listed - and loads - when the same game is loaded again in the next
 * (src/ui/SaveStates.tsx lists the running title's states).
 */
const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

test("a save state from the last session is listed and loads", async ({ page }) => {
  const demo = readFileSync(new URL("../public/demo/hello.nro", import.meta.url));
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("load-input").setInputFiles({ name: "hello.nro", mimeType: "application/octet-stream", buffer: demo });
  await page.getByTestId("state-save").click();
  await expect(page.getByTestId("state-status")).toContainText("State saved", { timeout: 30_000 });
  await expect(page.getByTestId("state-entry")).toHaveCount(1);

  await page.reload();
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("load-input").setInputFiles({ name: "hello.nro", mimeType: "application/octet-stream", buffer: demo });
  await expect(page.getByTestId("state-entry")).toHaveCount(1, { timeout: 20_000 });
  await page.getByTestId("state-load").first().click();
  await expect(page.getByTestId("state-status")).toContainText("Back at", { timeout: 30_000 });
});
