import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { expect, test } from "@playwright/test";

/**
 * Audio output (§14): loading a program (a user gesture) opens a 48kHz
 * AudioContext whose worklet drains the core's ring straight from the
 * shared WebAssembly memory. The worklet's reports prove it is running
 * on the audio thread against that ring.
 */
const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");

test("the audio worklet runs against the shared ring", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("run-demo").click();
  await page.mouse.click(5, 5); // a further gesture, in case the context started suspended
  await expect.poll(async () => page.evaluate(() => window.__VOLAND_AUDIO__?.blocks ?? 0), { timeout: 15_000 })
    .toBeGreaterThan(0);
  const report = await page.evaluate(() => window.__VOLAND_AUDIO__);
  expect(report?.ratio).toBeGreaterThan(0.98);
  expect(report?.ratio).toBeLessThan(1.02);
});
