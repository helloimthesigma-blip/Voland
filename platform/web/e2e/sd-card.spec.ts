import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

/**
 * The emulated SD card persists in the browser (§15, OPFS): a file added
 * through the shell is restored after a reload, and "Empty SD card"
 * removes it for good.
 */
const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

test("SD card files survive a reload until emptied", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("sd-clear").click();
  await expect(page.getByTestId("sd-result")).toContainText("emptied");

  const demo = readFileSync(new URL("../public/demo/hello.nro", import.meta.url));
  await page.getByTestId("sd-input").setInputFiles({ name: "hello.nro", mimeType: "application/octet-stream", buffer: demo });
  await expect(page.getByTestId("sd-result")).toContainText("/switch/hello.nro");

  await page.reload();
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await expect(page.locator("body")).toContainText("SD card: restored 1 file(s)");

  await page.getByTestId("sd-clear").click();
  await expect(page.getByTestId("sd-result")).toContainText("emptied");
  await page.reload();
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await expect(page.locator("body")).not.toContainText("SD card: restored");
});
