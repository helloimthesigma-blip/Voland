import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

/**
 * Resuming and recovering (src/autostate.ts, LoadPanel's crash panel): a
 * game with a save state offers "Continue" on its library tile, which
 * after a page reload starts it and goes back to that state; a crashed
 * game says so, copies a crash report and goes back to its save state. (Automatic states come minutes
 * into a game; their timing is unit-tested in autostate.test.ts.)
 */
const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
const PROGRAM_NCA = fileURLToPath(new URL("../../../build/native-noop/fixtures/synthetic_program.nca", import.meta.url));
test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

test("a library tile continues from its save state after a reload", async ({ page }) => {
  const demo = readFileSync(new URL("../public/demo/hello.nro", import.meta.url));
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("load-input").setInputFiles({ name: "hello.nro", mimeType: "application/octet-stream", buffer: demo });
  await page.getByTestId("state-save").click();
  await expect(page.getByTestId("state-status")).toContainText("State saved", { timeout: 30_000 });

  /* As in browsers without the File System Access picker (which a test
   * cannot drive): the tile asks for its file through a plain input. */
  await page.addInitScript(() => { delete (window as { showOpenFilePicker?: unknown }).showOpenFilePicker; });
  await page.reload();
  const cont = page.getByTestId("library-continue");
  await expect(cont).toContainText("Continue", { timeout: 20_000 });
  const restored = page.waitForEvent("console", { predicate: (m) => m.text().includes("save state loaded"), timeout: 30_000 });
  /* The tile's file came from a plain input: the browser asks for it again. */
  const chooser = page.waitForEvent("filechooser");
  await cont.click();
  await (await chooser).setFiles({ name: "hello.nro", mimeType: "application/octet-stream", buffer: demo });
  await expect(page.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
  await restored;
});

test("a crash offers a crash report and a way back", async ({ page, context }) => {
  test.skip(!existsSync(PROGRAM_NCA), "fixtures not built: run ctest --preset native-noop");
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  /* The synthetic program's code is not real code: it faults at once. */
  await page.getByTestId("load-input").setInputFiles({ name: "program.nca", mimeType: "application/octet-stream", buffer: readFileSync(PROGRAM_NCA) });
  const panel = page.getByTestId("crash-recover");
  await expect(panel).toContainText("The game crashed.", { timeout: 20_000 });
  await expect(panel).toContainText("no save state");
  await page.getByTestId("crash-report").click();
  await expect(page.getByTestId("crash-report")).toHaveText("Report copied");
  expect(await page.evaluate(() => navigator.clipboard.readText())).toContain("Voland compatibility report");

  /* With a save state of it, the next crash offers to go back. */
  await page.getByTestId("state-save").click();
  await expect(page.getByTestId("state-status")).toContainText("State saved", { timeout: 30_000 });
  await page.reload();
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("load-input").setInputFiles({ name: "program.nca", mimeType: "application/octet-stream", buffer: readFileSync(PROGRAM_NCA) });
  await expect(panel).toContainText("You can go back to your save state", { timeout: 20_000 });
  const restored = page.waitForEvent("console", { predicate: (m) => m.text().includes("save state loaded"), timeout: 30_000 });
  await page.getByTestId("crash-go-back").click();
  await restored;
});
