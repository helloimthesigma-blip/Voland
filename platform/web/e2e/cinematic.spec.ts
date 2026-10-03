import { existsSync } from "node:fs";

import { expect, test } from "@playwright/test";

import { decodePng } from "./png";

/**
 * A game's opening cinematic in the browser (opt-in, DESIGN §13 video
 * decode): set VOLAND_CINEMATIC_NCA to a decrypted program NCA you own
 * whose New Game starts with a video. A (the Z key) is pressed every few
 * seconds - title, profile, New Game - until the video worker reports a
 * stream; from then on nothing is pressed (A would skip the video), and
 * the screen must show the video: neither black nor frozen. Run with
 * playwright.video.config.ts (project chromium-gpu: the real GPU).
 *   VOLAND_CINEMATIC_SHOT=prefix keeps screenshots (prefix-N.png).
 */
const NCA = process.env["VOLAND_CINEMATIC_NCA"] ?? "";
const SHOT = process.env["VOLAND_CINEMATIC_SHOT"];
const BOOT_MS = Number(process.env["VOLAND_CINEMATIC_BOOT_MS"] ?? "900000");
const WATCH_MS = Number(process.env["VOLAND_CINEMATIC_WATCH_MS"] ?? "120000");
const PRESS_EVERY_MS = 6000;
const HOLD_MS = 400;
const SHOT_EVERY_MS = 5000;
const SAMPLE_GRID = 16;

test.skip(NCA === "" || !existsSync(NCA), "set VOLAND_CINEMATIC_NCA to a decrypted NCA with an opening video");
test.setTimeout(BOOT_MS + WATCH_MS + 60_000);

test("the opening cinematic plays", async ({ page }) => {
  let videoStream = "";
  let decodedFrames = false;
  page.on("console", (message) => {
    const text = message.text();
    if (/video stream \d+:/.test(text)) videoStream = text;
    if (/first video frame decoded/.test(text)) decodedFrames = true;
    if (/video|VideoDecoder|\[video\]|WebGPU|crash/.test(text)) console.log(text);
  });
  await page.goto("/");
  await expect(page.getByTestId("load-panel")).toBeVisible({ timeout: 20_000 });
  await page.getByTestId("load-input").setInputFiles(NCA);
  await expect(page.getByTestId("load-success")).toBeVisible();

  /* Menus: A until the video starts. */
  const started = Date.now();
  while (!videoStream && Date.now() - started < BOOT_MS) {
    await page.waitForTimeout(PRESS_EVERY_MS);
    const state = await page.getByTestId("run-state").getAttribute("data-state");
    expect(state === "crashed" || state === "deadlock", `run state ${state ?? ""}`).toBe(false);
    if (videoStream) break;
    await page.keyboard.down("KeyZ");
    await page.waitForTimeout(HOLD_MS);
    await page.keyboard.up("KeyZ");
  }
  expect(videoStream, "the game started a video").not.toBe("");
  console.log(`video started after ${Math.round((Date.now() - started) / 1000)}s: ${videoStream}`);

  /* Watch: the screen must leave black and keep changing. */
  const screen = page.getByTestId("screen");
  const fingerprints: string[] = [];
  let brightest = 0;
  for (let waited = 0, n = 0; waited < WATCH_MS; waited += SHOT_EVERY_MS, n++) {
    await page.waitForTimeout(SHOT_EVERY_MS);
    const box = await screen.boundingBox();
    if (!box) continue;
    const png = await page.screenshot({ clip: box, ...(SHOT ? { path: `${SHOT}-${n}.png` } : {}) });
    const shot = decodePng(png);
    const samples: number[] = [];
    for (let i = 0; i < SAMPLE_GRID * SAMPLE_GRID; i++) {
      const x = Math.floor((((i % SAMPLE_GRID) + 0.5) / SAMPLE_GRID) * (shot.width - 1));
      const y = Math.floor(((Math.floor(i / SAMPLE_GRID) + 0.5) / SAMPLE_GRID) * (shot.height - 1));
      const p = shot.pixel(x, y);
      samples.push(Math.round((p[0] + p[1] + p[2]) / 3));
    }
    const mean = samples.reduce((a, b) => a + b, 0) / samples.length;
    brightest = Math.max(brightest, mean);
    fingerprints.push(samples.map((v) => v >> 4).join(","));
    console.log(`t+${(waited + SHOT_EVERY_MS) / 1000}s mean luma ${mean.toFixed(1)}`);
  }
  expect(decodedFrames, "the video worker decoded frames").toBe(true);
  expect(brightest, "the video is not black").toBeGreaterThan(20);
  expect(new Set(fingerprints).size, "the video moves").toBeGreaterThan(3);
});
