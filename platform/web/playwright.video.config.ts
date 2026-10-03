import { defineConfig, devices } from "@playwright/test";

/**
 * The video worker test (e2e/video.spec.ts) on its own dev-server port,
 * so it never reuses another checkout's server on the shared 5173/5174.
 *   npx playwright test -c playwright.video.config.ts
 */
const PORT = 5181;

export default defineConfig({
  testDir: "./e2e",
  testMatch: /video\.spec\.ts/,
  timeout: 60_000,
  reporter: "list",
  webServer: {
    command: `npx vite --port ${PORT} --strictPort`,
    url: `http://localhost:${PORT}/e2e/video-harness.html`,
    reuseExistingServer: false,
    timeout: 60_000,
  },
  projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"], baseURL: `http://localhost:${PORT}`, channel: "chromium" } }],
});
