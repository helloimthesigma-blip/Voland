import { expect, test } from "@playwright/test";

/**
 * The controls panel lists the bindings and remaps them (kept across a
 * reload); the player bar's fullscreen shows the game canvas itself, not
 * the empty box it is pinned over (which used to go fullscreen black).
 */
test("controls panel remaps a key and keeps it across a reload", async ({ page }) => {
  await page.goto("/");
  const panel = page.getByTestId("controls");
  await expect(panel).toBeVisible({ timeout: 20_000 });
  await panel.locator("summary").click();
  await expect(page.getByTestId("bind-key-A")).toHaveText("Z");
  await expect(page.getByTestId("bind-key-LStickUp")).toHaveText("W");
  await expect(page.getByTestId("bind-pad-A")).toHaveText("Right face");

  await page.getByTestId("bind-key-A").click();
  await expect(page.getByTestId("bind-key-A")).toHaveText("press a key…");
  await page.keyboard.press("Space");
  await expect(page.getByTestId("bind-key-A")).toHaveText("Space");
  /* X moves from B to A: B is left unbound. */
  await page.getByTestId("bind-key-A").click();
  await page.keyboard.press("x");
  await expect(page.getByTestId("bind-key-A")).toHaveText("X");
  await expect(page.getByTestId("bind-key-B")).toHaveText("—");

  await page.reload();
  await page.getByTestId("controls").locator("summary").click();
  await expect(page.getByTestId("bind-key-A")).toHaveText("X");
  await page.getByTestId("bindings-reset").click();
  await expect(page.getByTestId("bind-key-A")).toHaveText("Z");
  await expect(page.getByTestId("bind-key-B")).toHaveText("X");
});

test("fullscreen shows the game canvas", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("run-demo").click();
  await expect(page.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("fullscreen").click();
  await expect.poll(() => page.evaluate(() => document.fullscreenElement?.id ?? null)).toBe("game");
  const box = await page.evaluate(() => {
    const canvas = document.getElementById("game");
    const rect = canvas?.getBoundingClientRect();
    const style = canvas ? getComputedStyle(canvas) : null;
    return { width: rect?.width ?? 0, height: rect?.height ?? 0, viewW: innerWidth, viewH: innerHeight,
             clip: style?.clipPath ?? "", visibility: style?.visibility ?? "" };
  });
  expect(box.width).toBe(box.viewW);
  expect(box.height).toBe(box.viewH);
  expect(box.clip).toBe("none");
  expect(box.visibility).toBe("visible");
});
