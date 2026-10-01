import { expect, test } from "@playwright/test";

import { decodePng } from "./png";

/**
 * First pixels (§6, §25 Phase 3): the core publishes its test card into a
 * framebuffer slot; the GPU worker uploads it and draws it with WebGPU
 * (SwiftShader here); the canvas sits over the shell's screen box. The
 * screenshot's colours must be the card's bars and ramp.
 */
const CLOSE = 24; // scaling + colour-space slack per channel

function near(actual: readonly number[], expected: readonly number[]): boolean {
  return actual.every((v, i) => Math.abs(v - (expected[i] ?? 0)) <= CLOSE);
}

test("the GPU worker presents the core's test card", async ({ page }) => {
  await page.goto("/");
  const screen = page.getByTestId("screen");
  await expect(screen).toBeVisible({ timeout: 20_000 });
  await page.waitForTimeout(1000); // let the first frame land
  const box = await screen.boundingBox();
  expect(box).not.toBeNull();
  if (!box) return;
  const shot = decodePng(await page.screenshot({ clip: box }));
  const at = (fx: number, fy: number) =>
    shot.pixel(Math.floor(fx * (shot.width - 1)), Math.floor(fy * (shot.height - 1)));

  // Bars (top 2/3): white, yellow, cyan, green, magenta, red, blue, black.
  const bars = [[235, 235, 235], [235, 235, 16], [16, 235, 235], [16, 235, 16],
                [235, 16, 235], [235, 16, 16], [16, 16, 235], [16, 16, 16]];
  bars.forEach((colour, i) => {
    expect(near(at((i + 0.5) / 8, 0.3), colour), `bar ${i}: got ${at((i + 0.5) / 8, 0.3).join(",")}`).toBe(true);
  });
  // Grey ramp: dark on the left, bright on the right.
  const left = at(0.02, 0.75), right = at(0.98, 0.75);
  expect((left[0] ?? 0) < 40 && (right[0] ?? 0) > 215).toBe(true);
});
