import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { devices, expect, test, type Page } from "@playwright/test";

import type { VolandE2EBootMilestone } from "../src/e2e-hooks";

/**
 * A phone browser (Android Chrome: memory64 and WebGPU): the shell boots,
 * fits the screen without sideways scrolling, every tab is reachable by
 * touch and a program runs. No touch controls are part of this - a phone
 * plays with a paired controller (Gamepad API) or a keyboard.
 */

declare global {
  interface Window {
    __VOLAND_E2E__?: VolandE2EBootMilestone;
  }
}

const CORE_JS = fileURLToPath(new URL("../public/core/switch_core.js", import.meta.url));
const TABS = ["games", "saves", "mods", "controls", "settings"] as const;
/* Rounding of fractional layout widths. */
const OVERFLOW_SLACK_PX = 1;

const { defaultBrowserType: _browser, ...pixel } = devices["Pixel 7"];
test.use({ ...pixel });

async function sidewaysOverflow(page: Page): Promise<number> {
  return page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
}

test("on a phone the shell boots with its full memory and fits the screen", async ({ page }) => {
  const pageErrors: string[] = [];
  page.on("pageerror", (err) => pageErrors.push(err.message));
  await page.goto("/");
  await page.waitForFunction(() => window.__VOLAND_E2E__ !== undefined, { timeout: 15_000 });
  const milestone = await page.evaluate(() => window.__VOLAND_E2E__);
  expect(milestone?.crossOriginIsolated).toBe(true);
  expect(milestone?.allocatedMemoryBytes).toBeGreaterThan(0);
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  expect(await sidewaysOverflow(page)).toBeLessThanOrEqual(OVERFLOW_SLACK_PX);
  expect(pageErrors).toEqual([]);
});

test("on a phone every tab opens by touch and none scrolls sideways", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  for (const tab of TABS) {
    await page.getByTestId(`tab-${tab}`).tap();
    await expect(page.getByTestId(`tab-${tab}`)).toHaveAttribute("aria-selected", "true");
    expect(await sidewaysOverflow(page), `tab ${tab}`).toBeLessThanOrEqual(OVERFLOW_SLACK_PX);
  }
});

test("on a phone a program runs and its screen fits", async ({ page }) => {
  test.skip(!existsSync(CORE_JS), "core not staged: run `cmake --build --preset web` first");
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("run-demo").tap();
  await expect(page.getByTestId("run-state")).toHaveAttribute("data-state", "exited", { timeout: 20_000 });
  await expect(page.getByTestId("guest-console")).toContainText("Hello from Voland!");
  const box = await page.getByTestId("screen").boundingBox();
  const width = page.viewportSize()?.width ?? 0;
  expect(box, "the game screen is laid out").not.toBeNull();
  expect((box?.x ?? 0) + (box?.width ?? 0)).toBeLessThanOrEqual(width + OVERFLOW_SLACK_PX);
  expect(await sidewaysOverflow(page)).toBeLessThanOrEqual(OVERFLOW_SLACK_PX);
});
