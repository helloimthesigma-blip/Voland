/**
 * ui-shot.mjs OUT.png [WIDTH HEIGHT] [--demo] - a screenshot of the shell
 * once it is mounted, optionally with the built-in demo running (npm run
 * build first; serves dist with vite preview).
 */
import { chromium } from "@playwright/test";
import { spawn } from "node:child_process";
const demo = process.argv.includes("--demo");
const [out, w = "1440", h = "900"] = process.argv.slice(2).filter((a) => a !== "--demo");
const server = spawn("npx", ["vite", "preview", "--port", "5191", "--strictPort"], { stdio: "ignore" });
await new Promise((r) => setTimeout(r, 2500));
const browser = await chromium.launch({ channel: "chromium", args: ["--enable-unsafe-webgpu", "--use-angle=metal"] });
try {
  const page = await browser.newPage({ viewport: { width: Number(w), height: Number(h) } });
  await page.goto("http://localhost:5191/");
  await page.waitForSelector(".voland-shell", { timeout: 60_000 });
  if (demo) {
    await page.getByTestId("run-demo").click();
    await page.getByTestId("load-success").waitFor({ timeout: 30_000 });
  }
  await page.waitForTimeout(1500);
  await page.screenshot({ path: out, fullPage: false });
  const full = out.replace(/\.png$/, "-full.png");
  await page.evaluate(() => { const h = document.querySelector(".voland-hero"); if (h) h.style.overflow = "visible"; });
  await page.locator(".voland-hero").screenshot({ path: full });
} finally {
  await browser.close();
  server.kill();
}
