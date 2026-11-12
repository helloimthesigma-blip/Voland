import { expect, test } from "@playwright/test";

/**
 * "How to use Voland" (src/ui/GettingStarted.tsx): open for a first-time
 * visitor, with the dumping guide link; closed once stays closed after a
 * reload.
 */
test("the how-to is open on a first visit and stays closed once closed", async ({ page }) => {
  await page.goto("/");
  const help = page.getByTestId("getting-started");
  await expect(help).toBeVisible({ timeout: 20_000 });
  await expect(help).toHaveAttribute("open", "");
  await expect(help.getByRole("link", { name: "How to dump and decrypt" })).toBeVisible();
  await help.locator("summary").click();
  await expect(help).not.toHaveAttribute("open", "");
  await page.reload();
  await expect(page.getByTestId("getting-started")).toBeVisible({ timeout: 20_000 });
  await expect(page.getByTestId("getting-started")).not.toHaveAttribute("open", "");
});
