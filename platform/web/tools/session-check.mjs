#!/usr/bin/env node
/**
 * Two browser sessions of one game in one persistent profile, on the
 * host's real GPU - checks what only shows across sessions:
 *
 *   session 1: boot, play to --frames, Save state, play on, Load state
 *              (the game must keep presenting), wait for the shader cache
 *              to be written;
 *   session 2: boot again: the shader cache must pre-build pipelines;
 *              Load the session-1 state right away (a fresh page reaching
 *              the same memory layout) and keep playing.
 *
 *   node tools/session-check.mjs --game FILE [--frames N] [--port P]
 *                                [--profile DIR] [--no-build]
 *
 * The port is fixed for both sessions: OPFS is per origin.
 */
import { spawn } from "node:child_process";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "@playwright/test";

const WEB_DIR = fileURLToPath(new URL("..", import.meta.url));
const GPU_ARGS = ["--enable-unsafe-webgpu", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"];

function parseArgs(argv) {
  const opts = { game: null, frames: 900, port: 4797, profile: null, build: true };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => argv[++i];
    if (a === "--game") opts.game = next();
    else if (a === "--frames") opts.frames = Number(next());
    else if (a === "--port") opts.port = Number(next());
    else if (a === "--profile") opts.profile = next();
    else if (a === "--no-build") opts.build = false;
    else throw new Error(`unknown argument ${a}`);
  }
  if (!opts.game) throw new Error("--game FILE is required");
  return opts;
}

function run(cmd, args) {
  return new Promise((resolve, reject) => {
    const child = spawn(cmd, args, { cwd: WEB_DIR, stdio: "inherit" });
    child.on("exit", (code) => (code === 0 ? resolve() : reject(new Error(`${cmd} exited ${code}`))));
  });
}

async function waitForUrl(url, ms) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    try {
      if ((await fetch(url)).ok) return;
    } catch { /* not up yet */ }
    await new Promise((r) => setTimeout(r, 300));
  }
  throw new Error(`${url} did not come up`);
}

const presents = (page) => page.evaluate(() => window.__VOLAND_STATS__?.presents ?? 0);

async function waitPresents(page, target, label) {
  const started = Date.now();
  for (;;) {
    const n = await presents(page);
    const state = await page.getByTestId("run-state").getAttribute("data-state").catch(() => null);
    if (state === "crashed" || state === "deadlock") throw new Error(`${label}: the game ${state} at ${n} frames`);
    if (n >= target) return n;
    if (Date.now() - started > 30 * 60_000) throw new Error(`${label}: stuck at ${n} frames`);
    await new Promise((r) => setTimeout(r, 1000));
  }
}

async function session(opts, name, body) {
  const context = await chromium.launchPersistentContext(opts.profile, {
    channel: "chromium", headless: true, args: GPU_ARGS, viewport: { width: 1280, height: 800 },
  });
  const page = await context.newPage();
  const lines = [];
  page.on("console", (m) => {
    const text = m.text();
    if (/shader cache|save state|svcBreak|crash/i.test(text)) {
      lines.push(text);
      console.log(`  [${name}] ${text}`);
    }
  });
  try {
    await page.goto(`http://localhost:${opts.port}/`);
    await page.getByTestId("load-panel").waitFor({ timeout: 60_000 });
    await page.getByTestId("load-input").setInputFiles(opts.game);
    await page.getByTestId("load-success").waitFor({ timeout: 120_000 });
    return await body(page, lines);
  } finally {
    await context.close();
  }
}

async function clickAndWaitStatus(page, testId) {
  const status = page.getByTestId("state-status");
  await page.getByTestId(testId).first().click();
  await status.filter({ hasText: /^[✓✗]/ }).waitFor({ timeout: 10 * 60_000 });
  return status.textContent();
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  opts.profile ??= mkdtempSync(join(tmpdir(), "voland-session-"));
  if (opts.build) await run("npx", ["vite", "build", "--logLevel", "warn"]);
  const preview = spawn("npx", ["vite", "preview", "--port", String(opts.port), "--strictPort"], { cwd: WEB_DIR, stdio: "ignore" });
  const results = {};
  try {
    await waitForUrl(`http://localhost:${opts.port}/`, 60_000);
    console.log(`profile ${opts.profile}`);
    await session(opts, "1", async (page) => {
      const at = await waitPresents(page, opts.frames, "boot");
      results.save = await clickAndWaitStatus(page, "state-save");
      console.log(`session 1: save at ${at} frames -> ${results.save}`);
      const after = await waitPresents(page, (await presents(page)) + 300, "after save");
      results.sameSessionLoad = await clickAndWaitStatus(page, "state-load");
      console.log(`session 1: load at ${after} frames -> ${results.sameSessionLoad}`);
      const resumed = await waitPresents(page, (await presents(page)) + 300, "after load");
      console.log(`session 1: still running at ${resumed} frames`);
      await new Promise((r) => setTimeout(r, 8000)); /* the shader cache flushes 5 s after its last addition */
    });
    await session(opts, "2", async (page, lines) => {
      await page.getByTestId("state-entry").first().waitFor({ timeout: 120_000 });
      results.crossSessionLoad = await clickAndWaitStatus(page, "state-load");
      console.log(`session 2: load right after boot -> ${results.crossSessionLoad}`);
      const n = await waitPresents(page, (await presents(page)) + 300, "session 2");
      console.log(`session 2: running at ${n} frames`);
      results.shaderCache = lines.filter((l) => /shader cache/.test(l));
    });
  } finally {
    preview.kill();
  }
  console.log(`\nRESULT ${JSON.stringify(results)}`);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
