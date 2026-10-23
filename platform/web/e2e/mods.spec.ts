import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

/**
 * The Mods panel (§19): a zip installs as a mod for a game, can be turned
 * off, reordered and deleted; a repository added by URL offers its mods
 * for the game and installs one after checking its SHA-256.
 */
const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

const TITLE = "0100abcd00000000";

/** A zip of stored (uncompressed) files. CRCs are 0: the reader does not check them. */
function zip(files: Readonly<Record<string, string>>): Buffer {
  const parts: Buffer[] = [];
  const central: Buffer[] = [];
  let offset = 0;
  for (const [name, text] of Object.entries(files)) {
    const n = Buffer.from(name);
    const data = Buffer.from(text);
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt32LE(data.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(n.length, 26);
    parts.push(local, n, data);
    const c = Buffer.alloc(46);
    c.writeUInt32LE(0x02014b50, 0);
    c.writeUInt32LE(data.length, 20);
    c.writeUInt32LE(data.length, 24);
    c.writeUInt16LE(n.length, 28);
    c.writeUInt32LE(offset, 42);
    central.push(c, n);
    offset += 30 + n.length + data.length;
  }
  const dir = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(Object.keys(files).length, 8);
  end.writeUInt16LE(Object.keys(files).length, 10);
  end.writeUInt32LE(dir.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...parts, dir, end]);
}

test.beforeEach(async ({ page }) => {
  await page.addInitScript((title) => {
    localStorage.setItem("voland.titleNames.v1", JSON.stringify({ [title.toUpperCase()]: "Test Game" }));
  }, TITLE);
});

test("a zip installs as a mod that can be disabled, reordered and deleted", async ({ page }) => {
  await page.goto("/");
  const panel = page.getByTestId("mods-panel");
  await expect(panel).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("mods-add-zip").setInputFiles([
    { name: "Bigger Text.zip", mimeType: "application/zip", buffer: zip({ "romfs/ui/font.bin": "font" }) },
    { name: "Hard Mode.zip", mimeType: "application/zip", buffer: zip({ "exefs/0123456789abcdef.ips": "PATCHEOF" }) },
  ]);
  const rows = panel.getByTestId("mod-row");
  await expect(rows).toHaveCount(2);
  await expect(rows.nth(0)).toContainText("Bigger Text");
  await rows.nth(1).getByTitle("Higher priority").click();
  await expect(rows.nth(0)).toContainText("Hard Mode");
  await rows.nth(0).locator('input[type="checkbox"]').uncheck();
  /* The flag is written to OPFS asynchronously; wait for it before reloading. */
  await expect.poll(() => page.evaluate(async (title) => {
    const root = await navigator.storage.getDirectory();
    const dir = await (await root.getDirectoryHandle("mods")).getDirectoryHandle(title);
    return (await (await dir.getFileHandle(".voland-mods.json")).getFile()).text();
  }, TITLE)).toContain('"disabled":["Hard Mode"]');
  await page.reload();
  await expect(page.getByTestId("mods-panel").getByTestId("mod-row").nth(0)).toContainText("Hard Mode");
  await expect(page.getByTestId("mods-panel").getByTestId("mod-row").nth(0).locator('input[type="checkbox"]')).not.toBeChecked();
  page.once("dialog", (dialog) => void dialog.accept());
  await page.getByTestId("mods-panel").getByTestId("mod-row").nth(0).getByTitle("Delete this mod from the browser").click();
  await expect(page.getByTestId("mods-panel").getByTestId("mod-row")).toHaveCount(1);
});

test("a repository offers a game's mods and installs one after its hash checks", async ({ page }) => {
  const archive = zip({ "romfs/data/level.bin": "modded level" });
  const manifest = {
    schemaVersion: 2,
    name: "Test Repo",
    entries: [{
      titleId: TITLE, modId: "levels", name: "New Levels", version: "1.2.0", type: "romfs", targetGameVersions: ["1.0.0"],
      artifactUrl: "https://repo.example/levels.zip", sha256: createHash("sha256").update(archive).digest("hex"),
      sizeBytes: archive.length,
    }],
  };
  const cors = { "access-control-allow-origin": "*", "cross-origin-resource-policy": "cross-origin" };
  await page.route("https://repo.example/repository.json", (route) =>
    route.fulfill({ status: 200, headers: { ...cors, "content-type": "application/json" }, body: JSON.stringify(manifest) }));
  await page.route("https://repo.example/levels.zip", (route) =>
    route.fulfill({ status: 200, headers: { ...cors, "content-type": "application/zip" }, body: archive }));

  await page.goto("/");
  const panel = page.getByTestId("mods-panel");
  await expect(panel).toBeVisible({ timeout: 20_000 });
  await panel.getByTestId("mod-repos").locator("summary").click();
  await panel.getByTestId("repo-url").fill("https://repo.example");
  page.once("dialog", (dialog) => void dialog.accept());
  await panel.getByRole("button", { name: "Add", exact: true }).click();
  await expect(panel).toContainText("Added Test Repo: 1 mod(s)");
  await panel.getByRole("button", { name: "Install" }).click();
  await expect(panel).toContainText("Installed New Levels");
  await expect(panel.getByTestId("mod-row")).toContainText("1.2.0 from a repository");
});
