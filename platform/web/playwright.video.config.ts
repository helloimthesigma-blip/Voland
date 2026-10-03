import { defineConfig, devices } from "@playwright/test";

/**
 * Video decode tests on their own ports, so they never reuse another
 * checkout's server on the shared 5173/5174.
 *   npx playwright test -c playwright.video.config.ts --project=chromium      (video worker, e2e/video.spec.ts)
 *   VOLAND_CINEMATIC_NCA=... npx playwright test -c playwright.video.config.ts --project=chromium-gpu
 *                                                                              (a game's cinematic, e2e/cinematic.spec.ts)
 */
const DEV_PORT = 5181;
const PREVIEW_PORT = 5182;
const HARDWARE_GPU_ARGS = ["--enable-unsafe-webgpu", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"];

export default defineConfig({
  testDir: "./e2e",
  timeout: 60_000,
  reporter: "list",
  webServer: [
    {
      command: `npx vite --port ${DEV_PORT} --strictPort`,
      url: `http://localhost:${DEV_PORT}/e2e/video-harness.html`,
      reuseExistingServer: false,
      timeout: 60_000,
    },
    {
      command: `npm run build && npx vite preview --port ${PREVIEW_PORT} --strictPort`,
      url: `http://localhost:${PREVIEW_PORT}`,
      reuseExistingServer: false,
      timeout: 180_000,
    },
  ],
  projects: [
    {
      name: "chromium",
      testMatch: /video\.spec\.ts/,
      use: { ...devices["Desktop Chrome"], baseURL: `http://localhost:${DEV_PORT}`, channel: "chromium" },
    },
    {
      name: "chromium-gpu",
      testMatch: /cinematic\.spec\.ts/,
      use: {
        ...devices["Desktop Chrome"],
        baseURL: `http://localhost:${PREVIEW_PORT}`,
        launchOptions: { args: HARDWARE_GPU_ARGS },
        channel: "chromium",
      },
    },
  ],
});
