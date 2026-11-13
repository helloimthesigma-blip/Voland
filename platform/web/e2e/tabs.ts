import type { Page } from "@playwright/test";

/** The shell shows one tab at a time under the game; open the one a test works in. */
export async function openTab(page: Page, id: "games" | "saves" | "mods" | "controls" | "settings"): Promise<void> {
  await page.getByTestId(`tab-${id}`).click({ timeout: 20_000 });
}

/** Settings, with the Advanced section (SD card, system files, event log, about:blank) unfolded. */
export async function openAdvanced(page: Page): Promise<void> {
  await openTab(page, "settings");
  const advanced = page.getByTestId("advanced");
  if (await advanced.getAttribute("open") === null) await advanced.locator("summary").first().click();
}
