import { createServer } from "node:http";
import type { AddressInfo } from "node:net";

import { expect, test } from "@playwright/test";

/**
 * "Open in about:blank" (src/blank-launch.ts, DESIGN.md §16): the blank
 * tab Voland opens is cross-origin isolated, the framed Voland in it boots
 * and runs the demo, and the original tab steps aside. Another site's
 * iframe runs Voland too: Document-Isolation-Policy isolates the framed
 * document though its embedder is not isolated.
 */
test("Voland runs framed in an about:blank tab it opens", async ({ page, context }) => {
  await page.goto("/");
  const popupOpened = context.waitForEvent("page");
  await page.getByTestId("open-blank").click({ timeout: 20_000 });
  const tab = await popupOpened;
  await expect.poll(() => page.url()).toBe("about:blank");
  expect(tab.url()).toBe("about:blank");
  expect(await tab.evaluate(() => crossOriginIsolated)).toBe(true);
  const frame = tab.frameLocator("iframe");
  await expect(frame.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await expect(frame.getByTestId("open-blank")).toHaveCount(0); /* no launcher inside the launcher */
  await frame.getByTestId("run-demo").click();
  await expect(frame.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
  const inner = tab.frames().find((f) => f !== tab.mainFrame());
  expect(await inner?.evaluate(() => crossOriginIsolated)).toBe(true);
});

test("another site's iframe runs Voland", async ({ page, baseURL }) => {
  /* A page on a different origin (127.0.0.1 vs localhost), not isolated itself. */
  const embedder = createServer((_req, res) => {
    res.writeHead(200, { "content-type": "text/html" });
    res.end(`<!doctype html><iframe src="${baseURL}/" allow="fullscreen; gamepad; autoplay"
             style="width:1200px;height:800px;border:0"></iframe>`);
  });
  await new Promise<void>((resolve) => embedder.listen(0, "127.0.0.1", resolve));
  try {
    const { port } = embedder.address() as AddressInfo;
    await page.setViewportSize({ width: 1240, height: 840 });
    await page.goto(`http://127.0.0.1:${port}/`);
    expect(await page.evaluate(() => crossOriginIsolated)).toBe(false);
    const frame = page.frameLocator("iframe");
    await expect(frame.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
    await frame.getByTestId("run-demo").click();
    await expect(frame.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
    const inner = page.frames().find((f) => f !== page.mainFrame());
    expect(await inner?.evaluate(() => crossOriginIsolated)).toBe(true);
  } finally {
    embedder.close();
  }
});
