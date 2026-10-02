import { expect, test } from "@playwright/test";

/**
 * The controls legend is generated from the keyboard profile, and the
 * player bar offers fullscreen once something is loaded (the demo NRO).
 */
test("controls legend lists the keyboard mapping; the player bar appears", async ({ page }) => {
  await page.goto("/");
  const legend = page.getByTestId("controls");
  await expect(legend).toBeVisible({ timeout: 20_000 });
  /* The legend's content is in the DOM even while collapsed. */
  await expect(legend).toContainText("A");
  await expect(legend).toContainText("Z");
  await expect(legend).toContainText("W S A D");
  await page.getByTestId("run-demo").click();
  await expect(page.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
  await expect(page.getByTestId("fullscreen")).toBeVisible();
});
