import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

/**
 * "System files" (§1.6): the user's own dump of a system archive is stored
 * where fsp-srv looks for it, listed as added, kept across a reload and
 * across "Empty SD card". Encrypted or unrecognised files are refused.
 */
const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

/* The smallest thing the core accepts as a RomFS image: a header whose size field is 0x50. */
function romfsImage(): Buffer {
  const bytes = Buffer.alloc(0x200);
  bytes.writeBigUInt64LE(0x50n, 0);
  return bytes;
}

test("a system file is stored, listed and kept", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  const mii = page.getByTestId("system-file-0100000000000802");
  await expect(mii).toBeVisible();

  await page.getByTestId("system-input").setInputFiles({ name: "junk.bin", mimeType: "application/octet-stream", buffer: Buffer.alloc(64) });
  await expect(page.getByTestId("system-result")).toContainText("Not added");

  await page.getByTestId("system-kind").selectOption("0100000000000802");
  await page.getByTestId("system-input").setInputFiles({ name: "miimodel.bin", mimeType: "application/octet-stream", buffer: romfsImage() });
  await expect(page.getByTestId("system-result")).toContainText("Added Mii model");
  await expect(mii).toContainText("added");
  await expect(mii).not.toContainText("not added");

  await page.getByTestId("sd-clear").click();
  await expect(page.getByTestId("sd-result")).toContainText("emptied");
  await page.reload();
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await expect(page.getByTestId("system-file-0100000000000802")).not.toContainText("not added");
});
