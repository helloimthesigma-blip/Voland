import { createServer } from "node:http";
import type { AddressInfo } from "node:net";

import { expect, test } from "@playwright/test";

/**
 * "Open in about:blank" (src/blank-launch.ts, DESIGN.md §16): the blank
 * tab Voland opens is cross-origin isolated, the framed Voland in it boots
 * and runs the demo, and the original tab steps aside. Any other origin's
 * frame is still refused (frame-ancestors 'self').
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

test("another origin cannot frame Voland", async ({ page, baseURL }) => {
  /* A page on a different origin (127.0.0.1 vs localhost) that frames Voland. */
  const embedder = createServer((_req, res) => {
    res.writeHead(200, { "content-type": "text/html" });
    res.end(`<!doctype html><iframe src="${baseURL}/" style="width:800px;height:600px"></iframe>`);
  });
  await new Promise<void>((resolve) => embedder.listen(0, "127.0.0.1", resolve));
  try {
    const { port } = embedder.address() as AddressInfo;
    await page.goto(`http://127.0.0.1:${port}/`);
    await page.waitForTimeout(3_000);
    const inner = page.frames().find((f) => f !== page.mainFrame());
    /* Refused by frame-ancestors 'self': the browser's error page, not Voland. */
    expect(inner?.url().startsWith(`${baseURL}`)).toBe(false);
  } finally {
    embedder.close();
  }
});
