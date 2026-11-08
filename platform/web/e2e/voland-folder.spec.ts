import { existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

/**
 * The Voland folder ("Your files"): one folder with games/, firmware/ and
 * mods/ is read once and everything in it is found - the game goes in the
 * library and launches, firmware/'s Mii model is kept, the mod is
 * installed for the game it is named after, and misplaced or encrypted
 * NCAs are pointed out. Driven through the folder input (the kept-handle
 * picker needs a real dialog). Every byte here is synthetic.
 */
const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
const FIXTURES = new URL("../../../build/native-noop/fixtures/", import.meta.url);
const PROGRAM_NCA = fileURLToPath(new URL("synthetic_program.nca", FIXTURES));
const CONTROL_NCA = fileURLToPath(new URL("synthetic_control.nca", FIXTURES));
const MII_NCA = fileURLToPath(new URL("synthetic_mii_model.nca", FIXTURES));
test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

function volandFolder(files: Readonly<Record<string, Buffer>>): string {
  const root = join(mkdtempSync(join(tmpdir(), "voland-folder-")), "Voland");
  for (const [path, bytes] of Object.entries(files)) {
    mkdirSync(dirname(join(root, path)), { recursive: true });
    writeFileSync(join(root, path), bytes);
  }
  return root;
}

test("one folder brings the game, its system files and its mods", async ({ page }) => {
  test.skip(![PROGRAM_NCA, CONTROL_NCA, MII_NCA].every(existsSync), "fixtures not built: run ctest --preset native-noop");
  await page.goto("/");
  await expect(page.getByTestId("files-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("voland-folder-input").setInputFiles(volandFolder({
    "games/Fixture/fedcba9876543210fedcba9876543210.nca": readFileSync(PROGRAM_NCA),
    "games/Fixture/0123456789abcdef0123456789abcdef.nca": readFileSync(CONTROL_NCA),
    "games/Fixture/00112233445566778899aabbccddeeff.nca": Buffer.alloc(0x400, 0x5a),
    "firmware/0a1b2c3d4e5f60718293a4b5c6d7e8f9.nca": readFileSync(MII_NCA),
    "mods/Fixture/Test Mod/romfs/hello.txt": Buffer.from("modded"),
    "lost.nca": Buffer.alloc(0x400),
  }));
  await expect(page.getByTestId("voland-folder-ready")).toBeVisible({ timeout: 20_000 });
  await expect(page.getByTestId("files-games")).toContainText("1 game(s)");
  await expect(page.getByTestId("files-games")).toContainText("Fixture");
  await expect(page.getByTestId("files-encrypted")).toContainText("games/Fixture/00112233445566778899aabbccddeeff.nca");
  await expect(page.getByTestId("files-stray")).toContainText("lost.nca");
  await expect(page.getByTestId("files-mods")).toContainText("installed Fixture/Test Mod", { timeout: 20_000 });
  await expect(page.getByTestId("system-file-0100000000000802")).not.toContainText("not added");

  const tile = page.getByTestId("library-game");
  await expect(tile).toHaveCount(1);
  await tile.getByRole("button").first().click();
  await expect(page.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
  await expect(page.getByTestId("library-game")).toContainText("Voland Fixture Quest");
  await expect(page.getByTestId("mods-panel")).toContainText("Test Mod");
  await expect(page.getByTestId("mods-panel")).toContainText("from your Voland folder");
});
