import { existsSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

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

/* A dumped game folder: the Program NCA boots, the control NCA beside it
 * names the game and gives its icon, still-encrypted NCAs are left out.
 * Fixtures: tests/program_nca_fixture.c (every byte synthetic). */
const FIXTURES = new URL("../../../build/native-noop/fixtures/", import.meta.url);
const PROGRAM_NCA = fileURLToPath(new URL("synthetic_program.nca", FIXTURES));
const CONTROL_NCA = fileURLToPath(new URL("synthetic_control.nca", FIXTURES));
const ENCRYPTED_NCA = Buffer.alloc(0x400, 0x5a);

/* A folder on disk (a webkitdirectory input takes a directory path). */
function gameFolder(files: Readonly<Record<string, Buffer>>): string {
  const folder = mkdtempSync(join(tmpdir(), "voland-game-"));
  for (const [name, bytes] of Object.entries(files)) writeFileSync(join(folder, name), bytes);
  return folder;
}

test("a game folder loads with its own name and icon", async ({ page }) => {
  test.skip(!existsSync(PROGRAM_NCA) || !existsSync(CONTROL_NCA), "fixtures not built: run ctest --preset native-noop");
  await page.goto("/");
  await expect(page.getByTestId("library-empty")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("load-folder-input").setInputFiles(gameFolder({
    "0123456789abcdef0123456789abcdef.nca": readFileSync(CONTROL_NCA),
    "fedcba9876543210fedcba9876543210.nca": readFileSync(PROGRAM_NCA),
    "00112233445566778899aabbccddeeff.nca": ENCRYPTED_NCA,
  }));
  await expect(page.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
  const tile = page.getByTestId("library-game");
  await expect(tile).toHaveCount(1);
  await expect(tile).toContainText("Voland Fixture Quest");
  await expect(tile).toContainText("asks for the folder");
  await expect(page.getByTestId("library-icon")).toBeVisible();

  await page.reload();
  await expect(page.getByTestId("library-game")).toContainText("Voland Fixture Quest", { timeout: 20_000 });
  await expect(page.getByTestId("library-icon")).toBeVisible();
});

test("a folder with only encrypted NCAs says to decrypt it first", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("load-folder-input").setInputFiles(gameFolder({ "00112233445566778899aabbccddeeff.nca": ENCRYPTED_NCA }));
  const error = page.getByTestId("load-error");
  await expect(error).toBeVisible();
  await expect(error).toHaveAttribute("data-reason", "encrypted-input");
  await expect(error).toContainText("1 encrypted");
  await expect(page.getByTestId("library-empty")).toBeVisible();
});
