import { expect, test } from "@playwright/test";

/**
 * The WebCodecs video backend (DESIGN §13, workers/video.worker.ts) in a
 * real Chromium: e2e/video-harness.ts writes a 30-frame H.264 clip's
 * decode requests into a video region the way core/video/video_stream.c
 * does, and the worker must hand back the frames as NV12 in the slots,
 * each tagged with its request's sequence number. The synthesized SPS
 * declares 2 reorder frames (core/video/h264.h), so the decoder holds the
 * last 2 back until more arrive: 28 of 30 come out.
 * Run with playwright.video.config.ts (own dev-server port).
 */
const DECODED = 30 - 2;

interface VideoResult {
  decoded: number;
  sequences: number[];
  lumaMeans: number[];
  logs: string[];
  supported: boolean | null;
}

test("video worker decodes a synthesized-SPS H.264 stream into NV12 slots", async ({ page }) => {
  const console: string[] = [];
  page.on("console", (m) => console.push(m.text()));
  page.on("pageerror", (e) => console.push(`pageerror: ${e.message}`));
  await page.goto("/e2e/video-harness.html");
  let r: VideoResult | null = null;
  const deadline = Date.now() + 20_000;
  while (Date.now() < deadline) {
    r = await page.evaluate(() => (window as unknown as { __videoResult?: VideoResult }).__videoResult ?? null);
    if (r && (r.decoded >= DECODED || r.supported === false || r.logs.some((l) => l.includes("error")))) break;
    await page.waitForTimeout(200);
  }
  if (!r) throw new Error(`no harness result; console:\n${console.join("\n")}`);
  const why = [...r.logs, ...console, `decoded ${r.decoded}`].join("\n");
  expect(r.supported, why).toBe(true);
  /* Give a stray extra frame time to show up (it must not). */
  await page.waitForTimeout(500);
  r = await page.evaluate(() => (window as unknown as { __videoResult: VideoResult }).__videoResult);
  expect(r.decoded, why).toBe(DECODED);
  expect(new Set(r.sequences).size).toBe(DECODED);
  expect(r.sequences.every((s) => s >= 1 && s <= 30)).toBe(true);
  /* testsrc2 moves: the frames are not all the same, nor black. */
  expect(new Set(r.lumaMeans).size).toBeGreaterThan(1);
  expect(Math.max(...r.lumaMeans)).toBeGreaterThan(16);
});
