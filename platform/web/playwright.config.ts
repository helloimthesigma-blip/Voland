import { defineConfig, devices } from "@playwright/test";

/**
 * Boot e2e (CLAUDE.md's `npm run e2e`): headless Chromium loads the built
 * app, asserts crossOriginIsolated is active, and asserts the full
 * ~5.25GiB shared memory allocation succeeds (§4/§16). Runs against a
 * production build under `vite preview`, which serves the same COOP/COEP/
 * CSP headers configured in vite.config.ts - a real build is what
 * actually ships. The same specs also run against the dev server
 * (`chromium-dev`), because that is what contributors open: Vite's dev
 * server transforms module requests in ways a production build does not
 * (it once refused to serve the staged core from public/), and only a
 * dev-server run catches that class of bug.
 */
const WEBGPU_ARGS = ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader", "--enable-features=Vulkan"];

export default defineConfig({
  testDir: "./e2e",
  timeout: 30_000,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 0,
  reporter: "list",
  use: {
    trace: "retain-on-failure",
  },
  webServer: [
    {
      command: "npm run build && npm run preview",
      url: "http://localhost:5174",
      reuseExistingServer: !process.env.CI,
      timeout: 60_000,
    },
    {
      command: "npm run dev",
      url: "http://localhost:5173",
      reuseExistingServer: !process.env.CI,
      timeout: 60_000,
    },
  ],
  /* Full Chromium in new-headless mode (the headless shell does not
   * composite WebGPU canvases into screenshots) with SwiftShader as the
   * WebGPU adapter, so
   * rendered pixels are checkable (and deterministic) with no GPU. */
  projects: [
    { name: "chromium",     use: { ...devices["Desktop Chrome"], baseURL: "http://localhost:5174", launchOptions: { args: WEBGPU_ARGS }, channel: "chromium" } },
    { name: "chromium-dev", use: { ...devices["Desktop Chrome"], baseURL: "http://localhost:5173", launchOptions: { args: WEBGPU_ARGS }, channel: "chromium" } },
  ],
});
