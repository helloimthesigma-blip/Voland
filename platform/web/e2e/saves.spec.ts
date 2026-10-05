import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

import { writeSaveArchive } from "../bindings/save-archive.ts";
import { tarNameOfSave, writeTar } from "../workers/save-store.ts";

/**
 * The save browser (§15): a restored backup shows up per game with its
 * files; a file added through the browser is stored (it is there after a
 * reload) and can be deleted again.
 */
const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

/* A Player save of program 010013C00E930000 for user 1. */
const NAME = `01-${"0000930ec0130001"}${"0100444e414c4f560100000000000000"}${"0000000000000000"}01${"00".repeat(31)}`;

test("save browser lists, edits and keeps a game's save files", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  const archive = writeSaveArchive(new Map([
    ["/user1.dat", new TextEncoder().encode('{"playerData":{}}')],
    ["/Restore_Points1/restoreData1.dat", new Uint8Array(40)],
  ]));
  const tar = writeTar(new Map([[tarNameOfSave(NAME), archive]]));
  const saves = page.getByTestId("saves");
  await saves.locator("summary").click();
  await page.getByTestId("saves-import-input").setInputFiles({ name: "backup.tar", mimeType: "application/x-tar", buffer: Buffer.from(tar) });
  /* The CPU worker may still be starting (a cold dev server compiles it). */
  await expect(page.getByTestId("saves-note")).toContainText("Imported 1 save", { timeout: 30_000 });

  const entry = page.locator(`[data-save="${NAME}"]`);
  await expect(entry).toContainText("010013C00E930000");
  await expect(entry).toContainText("2 files");
  await entry.locator(".voland-save-head").click();
  const browser = page.getByTestId("save-browser");
  await expect(browser.locator('[data-path="/user1.dat"]')).toBeVisible();
  await expect(browser.locator('[data-path="/Restore_Points1/restoreData1.dat"]')).toBeVisible();

  await page.getByTestId("save-add-input").setInputFiles({ name: "user2.dat", mimeType: "application/octet-stream", buffer: Buffer.from("{}") });
  await expect(page.getByTestId("saves-note")).toContainText("Added 1 file");
  await expect(browser.locator('[data-path="/user2.dat"]')).toContainText("updated");
  await expect(page.getByTestId("save-status")).toContainText("✓ Added 1 file");

  await page.reload();
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("saves").locator("summary").click();
  await expect(page.locator(`[data-save="${NAME}"]`)).toContainText("3 files");
  await page.locator(`[data-save="${NAME}"] .voland-save-head`).click();
  await page.locator('[data-path="/user2.dat"] [data-testid="save-delete-path"]').click();
  await expect(page.getByTestId("saves-note")).toContainText("Deleted /user2.dat");

  page.once("dialog", (dialog) => void dialog.accept());
  await page.getByTestId("save-delete").click();
  await expect(page.getByTestId("saves-note")).toContainText("Save deleted");
  await expect(page.locator(`[data-save="${NAME}"]`)).toHaveCount(0);
});
