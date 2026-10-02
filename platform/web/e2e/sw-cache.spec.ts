import { expect, test } from "@playwright/test";

/**
 * A browser that visited before an update must still boot (§16). A
 * previous service-worker version cached the core's .wasm cache-first
 * under an unhashed URL; the next visit paired that stale module with the
 * new switch_core.js and the CPU worker failed at init. Seed such a stale
 * cache, reload under the current worker, and require a working core and
 * the old cache gone.
 */
test("a stale cached core from an older service worker does not break boot", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.evaluate(async () => {
    const stale = await caches.open("voland-wasm-v1");
    await stale.put("/core/switch_core.wasm", new Response(new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]), {
      headers: { "Content-Type": "application/wasm" },
    }));
    /* As after an update: the current worker installs and activates
     * with the old cache already present. */
    for (const registration of await navigator.serviceWorker.getRegistrations()) await registration.unregister();
  });
  await page.reload();
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("run-demo").click();
  await expect(page.getByTestId("load-success")).toBeVisible({ timeout: 20_000 });
  const caches_ = await page.evaluate(async () => caches.keys());
  expect(caches_).not.toContain("voland-wasm-v1");
});
