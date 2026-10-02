import { existsSync, readFileSync } from "node:fs";

import { expect, test } from "@playwright/test";

import { decodePng } from "./png";

/**
 * Real homebrew in the browser (opt-in): set VOLAND_HOMEBREW_NRO to a
 * libnx NRO (or a decrypted NCA) you have, e.g. a released
 * nx-hbmenu.nro, and this loads it
 * through the shell, lets it run, and checks it drew something other
 * than the core's test card. No third-party binary lives in the
 * repository, so without the variable the test skips. Set
 * VOLAND_HOMEBREW_SHOT to keep the screenshot.
 */
const NRO = process.env["VOLAND_HOMEBREW_NRO"] ?? "";
const SHOT = process.env["VOLAND_HOMEBREW_SHOT"];
const RUN_MS = Number(process.env["VOLAND_HOMEBREW_RUN_MS"] ?? "60000");
const HOLD_MS = Number(process.env["VOLAND_HOMEBREW_HOLD_MS"] ?? "3000");

test.skip(NRO === "" || !existsSync(NRO), "set VOLAND_HOMEBREW_NRO to a homebrew NRO to run this");
test.setTimeout(RUN_MS + 60_000);

test("a real homebrew NRO boots and presents frames", async ({ page }) => {
  page.on("console", (message) => {
    if (message.text().includes("[gpu] reference renderer")) console.log(message.text());
  });
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  // By path: the browser reads the file itself, so multi-GB content (a
  // decrypted NCA) loads without passing through this process.
  await page.getByTestId("load-input").setInputFiles(NRO);
  await expect(page.getByTestId("load-success")).toBeVisible();

  const screen = page.getByTestId("screen");
  const deadline = Date.now() + RUN_MS;
  let distinct = 0;
  while (Date.now() < deadline) {
    await page.waitForTimeout(2000);
    const state = await page.getByTestId("run-state").getAttribute("data-state");
    expect(state === "crashed" || state === "deadlock", `run state ${state ?? ""}`).toBe(false);
    const box = await screen.boundingBox();
    if (!box) continue;
    const png = await page.screenshot({ clip: box });
    if (SHOT) await page.screenshot({ clip: box, path: SHOT });
    const shot = decodePng(png);
    // The test card's top-left bar is white; a guest frame replaces it.
    const colours = new Set<string>();
    for (let i = 0; i < 64; i++) {
      const p = shot.pixel(Math.floor(((i % 8) + 0.5) / 8 * (shot.width - 1)), Math.floor((Math.floor(i / 8) + 0.5) / 8 * (shot.height - 1)));
      colours.add(p.join(","));
    }
    const corner = shot.pixel(Math.floor(shot.width * 0.06), Math.floor(shot.height * 0.3));
    distinct = colours.size;
    if (!(corner[0] > 200 && corner[1] > 200 && corner[2] > 200) && distinct > 1) break;
  }
  expect(distinct, "the guest presented a non-uniform frame").toBeGreaterThan(1);
  /* Optional: report the frame rate after it settles (VOLAND_HOMEBREW_FPS_MS). */
  const fpsMs = Number(process.env["VOLAND_HOMEBREW_FPS_MS"] ?? "0");
  if (fpsMs > 0) {
    const samples: number[] = [];
    for (let waited = 0; waited < fpsMs; waited += 1000) {
      await page.waitForTimeout(1000);
      samples.push(await page.evaluate(() => window.__VOLAND_STATS__?.fps ?? 0));
    }
    console.log(`guest fps samples: ${samples.join(" ")}`);
  }
});

/* The homebrew-menu loop (opt-in, VOLAND_HOMEBREW_MENU=1 with a menu NRO
 * such as hbmenu): Voland's demo NRO goes onto the SD card, the menu lists
 * it, A (the Z key) launches it through the chain-loader, it runs and
 * exits, and the menu comes back. */
test("a homebrew menu launches an NRO from the SD card", async ({ page }) => {
  test.skip(process.env["VOLAND_HOMEBREW_MENU"] !== "1", "set VOLAND_HOMEBREW_MENU=1 with a menu NRO");
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  const demo = readFileSync(new URL("../public/demo/hello.nro", import.meta.url));
  await page.getByTestId("sd-input").setInputFiles({ name: "hello.nro", mimeType: "application/octet-stream", buffer: demo });
  await expect(page.getByTestId("sd-result")).toContainText("/switch/hello.nro");
  // By path: the browser reads the file itself, so multi-GB content (a
  // decrypted NCA) loads without passing through this process.
  await page.getByTestId("load-input").setInputFiles(NRO);
  await expect(page.getByTestId("load-success")).toBeVisible();
  await page.waitForTimeout(Math.min(RUN_MS, 20_000)); // let the menu scan /switch and draw
  if (SHOT) await page.getByTestId("screen").screenshot({ path: SHOT });
  /* A launches; the demo isn't a libnx build, so the menu asks to
   * confirm (an ABI warning) - A again. */
  const pressA = async (): Promise<void> => {
    /* Held long enough to span a menu frame: the menu redraws its whole
     * UI in software, slow on the interpreter. */
    await page.keyboard.down("KeyZ");
    await page.waitForTimeout(HOLD_MS);
    await page.keyboard.up("KeyZ");
  };
  await pressA();
  await page.waitForTimeout(3000);
  if (SHOT) await page.getByTestId("screen").screenshot({ path: SHOT.replace(/\.png$/, "-confirm.png") });
  await pressA();
  await expect(page.getByTestId("guest-console")).toContainText("Hello from Voland!", { timeout: RUN_MS });
  await expect(page.getByTestId("run-state")).toHaveAttribute("data-state", "running");
});
