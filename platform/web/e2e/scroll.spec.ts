import { expect, test } from "@playwright/test";

/**
 * The shell scrolls when the window is too short for it (the load
 * result and controls sit below the screen), including with the wheel
 * over the game canvas, which floats above the scrolling column.
 */
test("the main column scrolls in a short window", async ({ page }) => {
  await page.setViewportSize({ width: 1100, height: 560 });
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  const hero = page.locator(".voland-hero");
  const overflow = await hero.evaluate((el) => el.scrollHeight - el.clientHeight);
  expect(overflow).toBeGreaterThan(0);
  const screen = await page.getByTestId("screen").boundingBox();
  expect(screen).not.toBeNull();
  if (!screen) return;
  await page.mouse.move(screen.x + screen.width / 2, screen.y + screen.height / 2);
  await page.mouse.wheel(0, 400);
  await expect.poll(() => hero.evaluate((el) => el.scrollTop)).toBeGreaterThan(0);
  /* Scrolled down, the controls below the screen are reachable. */
  await hero.evaluate((el) => el.scrollTo({ top: el.scrollHeight }));
  await expect(page.getByTestId("load-input")).toBeAttached();
  const legend = page.locator(".voland-facts");
  await expect(legend).toBeInViewport();
});
