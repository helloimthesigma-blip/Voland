import { expect, test } from "@playwright/test";

/**
 * Frame skip (a speed setting, §13): the choice is offered, applied, and
 * remembered across a reload; a program still runs to completion with it
 * on (only rasterisation is skipped, never the guest's work).
 */
test("frame skip is offered, remembered, and programs still run", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  const select = page.getByTestId("frame-skip").locator("select");
  await expect(select).toHaveValue("0");
  await select.selectOption("2");
  await page.reload();
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await expect(page.getByTestId("frame-skip").locator("select")).toHaveValue("2");

  await page.getByTestId("run-demo").click();
  await expect(page.getByTestId("run-state")).toHaveAttribute("data-state", "exited", { timeout: 20_000 });
  await expect(page.getByTestId("guest-console")).toContainText("hello from a second guest thread");
});
