import { existsSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

/**
 * The heavy-game warning (src/memory-guard.ts): a game remembered as
 * having used a lot of memory says so when it is launched again; a light
 * one does not.
 */
const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

test("a game that needed a lot of memory warns before it loads again", async ({ page }) => {
  /* One file on disk: the library knows a game by its name, size and date. */
  const demo = join(mkdtempSync(join(tmpdir(), "voland-mem-")), "hello.nro");
  writeFileSync(demo, readFileSync(new URL("../public/demo/hello.nro", import.meta.url)));
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("load-input").setInputFiles(demo);
  await expect(page.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
  await expect(page.getByTestId("memory-warning")).toHaveCount(0);
  const titleId = await page.getByTestId("load-success").locator("code").first().innerText();

  await page.evaluate((id) => localStorage.setItem("voland.memoryUse.v1", JSON.stringify({ [id]: 3.5 * 1024 ** 3 })), titleId);
  await page.addInitScript(() => { delete (window as { showOpenFilePicker?: unknown }).showOpenFilePicker; });
  await page.reload();
  const chooser = page.waitForEvent("filechooser");
  await page.getByTestId("library-game").getByRole("button").first().click();
  await (await chooser).setFiles(demo);
  const warning = page.getByTestId("memory-warning");
  await expect(warning).toContainText("Heavy game. Last time it used 3.5 GB");
  await warning.getByRole("button", { name: "Dismiss" }).click();
  await expect(warning).toHaveCount(0);
});
