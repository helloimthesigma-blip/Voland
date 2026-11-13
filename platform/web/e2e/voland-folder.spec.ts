import { existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

import { writeSaveArchive } from "../bindings/save-archive.ts";
import { tarNameOfSave, writeTar } from "../workers/save-store.ts";
import { openTab } from "./tabs.ts";

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
  await expect(page.getByTestId("files-encrypted")).toContainText("lost.nca"); /* outside games/ it is still read */
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

/* A Player save of program 010013C00E930000 for user 1 (as saves.spec.ts). */
const SAVE_NAME = `01-${"0000930ec0130001"}${"0100444e414c4f560100000000000000"}${"0000000000000000"}01${"00".repeat(31)}`;

test("a kept folder is read again at load, backs saves up and follows its mods", async ({ page }) => {
  test.skip(![PROGRAM_NCA, CONTROL_NCA].every(existsSync), "fixtures not built: run ctest --preset native-noop");
  await page.goto("/");
  await expect(page.getByTestId("files-panel")).toBeVisible({ timeout: 20_000 });
  const tar = writeTar(new Map([[tarNameOfSave(SAVE_NAME), writeSaveArchive(new Map([["/user1.dat", new Uint8Array(8)]]))]]));
  await openTab(page, "saves");
  await page.getByTestId("saves-import-input").setInputFiles({ name: "backup.tar", mimeType: "application/x-tar", buffer: Buffer.from(tar) });
  await expect(page.getByTestId("saves-note")).toContainText("Imported 1 save", { timeout: 30_000 });

  await page.evaluate(async (files) => {
    const root = await (await navigator.storage.getDirectory()).getDirectoryHandle("kept-voland", { create: true });
    for (const [path, bytes] of Object.entries(files)) {
      const parts = path.split("/");
      const leaf = parts.pop() ?? "";
      let dir = root;
      for (const part of parts) dir = await dir.getDirectoryHandle(part, { create: true });
      const writable = await (await dir.getFileHandle(leaf, { create: true })).createWritable();
      await writable.write(new Uint8Array(bytes));
      await writable.close();
    }
    await new Promise<void>((resolve, reject) => {
      const open = indexedDB.open("voland-folder", 1);
      open.onupgradeneeded = (): void => { open.result.createObjectStore("handles"); };
      open.onsuccess = (): void => {
        const tx = open.result.transaction("handles", "readwrite");
        tx.objectStore("handles").put(root, "root");
        tx.oncomplete = (): void => resolve();
        tx.onerror = (): void => reject(tx.error);
      };
      open.onerror = (): void => reject(open.error);
    });
  }, {
    "games/Fixture/fedcba9876543210fedcba9876543210.nca": [...readFileSync(PROGRAM_NCA)],
    "games/Fixture/0123456789abcdef0123456789abcdef.nca": [...readFileSync(CONTROL_NCA)],
    "firmware/readme.txt": [...Buffer.from("x")],
    "mods/Fixture/Kept Mod/romfs/a.txt": [...Buffer.from("modded")],
  });

  await page.reload();
  await expect(page.getByTestId("voland-folder-ready")).toBeVisible({ timeout: 20_000 });
  await expect(page.getByTestId("files-games")).toContainText("Fixture");
  await expect(page.getByTestId("files-mods")).toContainText("installed Fixture/Kept Mod", { timeout: 20_000 });
  await expect(page.getByTestId("library-game")).toHaveCount(1);
  await expect(page.getByTestId("library-game")).not.toContainText("asks for");

  await page.getByTestId("files-save-backup").check();
  await expect(page.getByTestId("files-saves")).toContainText("1 save(s) backed up to saves/voland-saves-", { timeout: 20_000 });
  const backups = await page.evaluate(async () => {
    const dir = await (await (await navigator.storage.getDirectory()).getDirectoryHandle("kept-voland")).getDirectoryHandle("saves");
    const out: [string, number][] = [];
    for await (const [name, handle] of dir.entries()) out.push([name, (await (handle as FileSystemFileHandle).getFile()).size]);
    return out;
  });
  expect(backups).toHaveLength(1);
  expect(backups[0]?.[0]).toMatch(/^voland-saves-\d{4}-\d{2}-\d{2}\.tar$/);
  expect(backups[0]?.[1]).toBeGreaterThan(0);

  await page.evaluate(async () => {
    const root = await (await navigator.storage.getDirectory()).getDirectoryHandle("kept-voland");
    await (await root.getDirectoryHandle("mods")).removeEntry("Fixture", { recursive: true });
  });
  await page.getByTestId("voland-folder-rescan").click();
  await expect(page.getByTestId("files-mods")).toContainText("removed Fixture/Kept Mod", { timeout: 20_000 });
});

test("Tidy up moves loose games and firmware into their folders", async ({ page }) => {
  test.skip(![PROGRAM_NCA, CONTROL_NCA, MII_NCA].every(existsSync), "fixtures not built: run ctest --preset native-noop");
  await page.goto("/");
  await expect(page.getByTestId("files-panel")).toBeVisible({ timeout: 20_000 });
  await page.evaluate(async (files) => {
    const root = await (await navigator.storage.getDirectory()).getDirectoryHandle("tidy-voland", { create: true });
    for (const [path, bytes] of Object.entries(files)) {
      const parts = path.split("/");
      const leaf = parts.pop() ?? "";
      let dir = root;
      for (const part of parts) dir = await dir.getDirectoryHandle(part, { create: true });
      const writable = await (await dir.getFileHandle(leaf, { create: true })).createWritable();
      await writable.write(new Uint8Array(bytes));
      await writable.close();
    }
    await new Promise<void>((resolve, reject) => {
      const open = indexedDB.open("voland-folder", 1);
      open.onupgradeneeded = (): void => { open.result.createObjectStore("handles"); };
      open.onsuccess = (): void => {
        const tx = open.result.transaction("handles", "readwrite");
        tx.objectStore("handles").put(root, "root");
        tx.oncomplete = (): void => resolve();
        tx.onerror = (): void => reject(tx.error);
      };
      open.onerror = (): void => reject(open.error);
    });
  }, {
    "dumps/Quest/fedcba9876543210fedcba9876543210.nca": [...readFileSync(PROGRAM_NCA)],
    "dumps/Quest/0123456789abcdef0123456789abcdef.nca": [...readFileSync(CONTROL_NCA)],
    "0a1b2c3d4e5f60718293a4b5c6d7e8f9.nca": [...readFileSync(MII_NCA)],
  });
  await page.reload();
  await expect(page.getByTestId("files-games")).toContainText("Quest", { timeout: 20_000 });
  await page.getByTestId("voland-folder-tidy").click();
  await expect(page.getByTestId("files-note")).toContainText("Moved 3 file(s)", { timeout: 20_000 });
  const layout = await page.evaluate(async () => {
    const out: string[] = [];
    const walk = async (dir: FileSystemDirectoryHandle, prefix: string): Promise<void> => {
      for await (const [name, handle] of dir.entries()) {
        if (handle.kind === "file") out.push(prefix + name);
        else await walk(handle as FileSystemDirectoryHandle, `${prefix}${name}/`);
      }
    };
    await walk(await (await navigator.storage.getDirectory()).getDirectoryHandle("tidy-voland"), "");
    return out.sort();
  });
  expect(layout).toEqual([
    "firmware/0a1b2c3d4e5f60718293a4b5c6d7e8f9.nca",
    "games/Quest/0123456789abcdef0123456789abcdef.nca",
    "games/Quest/fedcba9876543210fedcba9876543210.nca",
  ]);
  await expect(page.getByTestId("voland-folder-tidy")).toHaveCount(0);
  await expect(page.getByTestId("library-game")).toHaveCount(1);
});
