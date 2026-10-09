import { existsSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

/**
 * "System files" (§1.6): the user's own dumps of system archives are
 * stored where fsp-srv looks for them, listed as added, and kept across a
 * reload and across "Empty SD card". A whole decrypted firmware folder can
 * be given: only the archives games need are kept. Encrypted or
 * unrecognised files are refused. Every byte here is synthetic.
 */
const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
const MII_NCA = fileURLToPath(new URL("../../../build/native-noop/fixtures/synthetic_mii_model.nca", import.meta.url));
const PROGRAM_NCA = fileURLToPath(new URL("../../../build/native-noop/fixtures/synthetic_program.nca", import.meta.url));
test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

/* The smallest thing the core accepts as a RomFS image: a header whose size field is 0x50. */
function romfsImage(): Buffer {
  const bytes = Buffer.alloc(0x200);
  bytes.writeBigUInt64LE(0x50n, 0);
  return bytes;
}
const octets = "application/octet-stream";

test("a system file is stored, listed and kept", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  const mii = page.getByTestId("system-file-0100000000000802");
  await expect(mii).toBeVisible();

  await page.getByTestId("system-input").setInputFiles({ name: "junk.bin", mimeType: octets, buffer: Buffer.alloc(64) });
  await expect(page.getByTestId("system-result")).toContainText("Nothing added");

  await page.getByTestId("system-kind").selectOption("0100000000000802");
  await page.getByTestId("system-input").setInputFiles({ name: "miimodel.bin", mimeType: octets, buffer: romfsImage() });
  await expect(page.getByTestId("system-result")).toContainText("Added Mii model");
  await expect(mii).not.toContainText("not added");

  await page.getByTestId("sd-clear").click();
  await expect(page.getByTestId("sd-result")).toContainText("emptied");
  await page.reload();
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await expect(page.getByTestId("system-file-0100000000000802")).not.toContainText("not added");
});

test("a decrypted firmware folder keeps only what games need", async ({ page }) => {
  test.skip(!existsSync(MII_NCA) || !existsSync(PROGRAM_NCA), "fixtures not built: run ctest --preset native-noop");
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  const folder = mkdtempSync(join(tmpdir(), "voland-firmware-"));
  writeFileSync(join(folder, "0a1b2c3d4e5f60718293a4b5c6d7e8f9.nca"), readFileSync(MII_NCA));
  writeFileSync(join(folder, "11223344556677889900aabbccddeeff.nca"), readFileSync(PROGRAM_NCA));
  writeFileSync(join(folder, "ffeeddccbbaa00998877665544332211.cnmt.nca"), Buffer.alloc(0x4000));
  await page.getByTestId("firmware-input").setInputFiles(folder);
  await expect(page.getByTestId("system-result")).toContainText("Added Mii model (0100000000000802)");
  await expect(page.getByTestId("system-result")).toContainText("2 file(s) skipped");
  await expect(page.getByTestId("system-file-0100000000000802")).not.toContainText("not added");
});
