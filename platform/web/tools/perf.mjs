#!/usr/bin/env node
/**
 * Browser perf harness: boots a game in Chromium on the host's real GPU
 * and prints how fast the CPU worker runs it.
 *
 *   node tools/perf.mjs --game FILE [--warmup-slices N] [--seconds S]
 *                       [--profile S] [--port P] [--no-build] [--software]
 *                       [--url-params "a=1&b=2"]
 *
 * - Builds the app (vite build; the core must already be staged by
 *   `cmake --build --preset web`) and serves it with `vite preview` on its
 *   own port, so it never collides with another checkout's server.
 * - Loads FILE through the load panel, runs until the CPU worker has done
 *   --warmup-slices scheduler slices (default 860000: Silksong's title
 *   screen), then measures for --seconds (default 30).
 * - Prints slices/s, virtual ticks/s (and the speed against real time),
 *   SVCs/s, presented fps, and where the worker's wall time went (inside
 *   emulator_run_slice, in game-file reads, yielding to the event loop),
 *   plus GPU stream stalls (the producer waiting on the GPU worker).
 * - --profile S also samples the CPU worker with the CDP Profiler for S
 *   seconds after the measurement and prints the top functions by self
 *   time; the .cpuprofile goes to --out-dir (default: the OS temp dir).
 *   Build the core with -DVOLAND_WASM_PROFILING=ON for wasm function names.
 *
 * Slices are deterministic for a given game and input, so the same
 * --warmup-slices reaches the same point natively:
 *   voland-cli run FILE --backend interpreter --budget 200000 \
 *     --max-slices N --gpu-stream /dev/null
 */
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { chromium } from "@playwright/test";

const WEB_DIR = resolve(dirname(fileURLToPath(import.meta.url)), "..");
/* Virtual ticks per second (the Switch's 19.2 MHz counter). */
const TICKS_PER_SECOND = 19_200_000;
const HARDWARE_GPU_ARGS = ["--enable-unsafe-webgpu", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"];
const SOFTWARE_GPU_ARGS = ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader", "--enable-features=Vulkan"];

function parseArgs(argv) {
  const opts = {
    game: "", warmupSlices: 860_000, seconds: 30, profile: 0, port: 5190, debugPort: 9390,
    build: true, software: false, urlParams: "", outDir: tmpdir(),
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => argv[++i] ?? "";
    if (a === "--game") opts.game = next();
    else if (a === "--warmup-slices") opts.warmupSlices = Number(next());
    else if (a === "--seconds") opts.seconds = Number(next());
    else if (a === "--profile") opts.profile = Number(next());
    else if (a === "--port") opts.port = Number(next());
    else if (a === "--debug-port") opts.debugPort = Number(next());
    else if (a === "--no-build") opts.build = false;
    else if (a === "--software") opts.software = true;
    else if (a === "--url-params") opts.urlParams = next();
    else if (a === "--out-dir") opts.outDir = next();
    else throw new Error(`unknown argument ${a}`);
  }
  if (!opts.game || !existsSync(opts.game)) throw new Error("--game FILE is required (a decrypted NCA or an NRO)");
  return opts;
}

function run(command, args) {
  return new Promise((resolvePromise, reject) => {
    const child = spawn(command, args, { cwd: WEB_DIR, stdio: "inherit" });
    child.on("exit", (code) => (code === 0 ? resolvePromise() : reject(new Error(`${command} ${args.join(" ")} exited ${code}`))));
  });
}

async function waitForUrl(url, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(url);
      if (response.ok) return;
    } catch {
      /* not up yet */
    }
    await new Promise((r) => setTimeout(r, 250));
  }
  throw new Error(`${url} did not come up`);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* ---- A minimal CDP client over the browser's WebSocket (flattened sessions),
 * because Playwright cannot open a CDP session on a dedicated worker. ---- */
class Cdp {
  constructor(socket) {
    this.socket = socket;
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = [];
    socket.addEventListener("message", (event) => {
      const msg = JSON.parse(String(event.data));
      if (msg.id && this.pending.has(msg.id)) {
        const { resolveCall, rejectCall } = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        if (msg.error) rejectCall(new Error(`${msg.error.message}`));
        else resolveCall(msg.result);
      } else if (msg.method) {
        for (const listener of this.listeners) listener(msg);
      }
    });
  }
  static async connect(debugPort) {
    const version = await (await fetch(`http://127.0.0.1:${debugPort}/json/version`)).json();
    const socket = new WebSocket(version.webSocketDebuggerUrl);
    await new Promise((r, j) => {
      socket.addEventListener("open", r, { once: true });
      socket.addEventListener("error", j, { once: true });
    });
    return new Cdp(socket);
  }
  send(method, params = {}, sessionId = undefined) {
    const id = this.nextId++;
    this.socket.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
    return new Promise((resolveCall, rejectCall) => this.pending.set(id, { resolveCall, rejectCall }));
  }
  close() {
    this.socket.close();
  }
}

async function attachToCpuWorker(cdp, pageUrl) {
  const { targetInfos } = await cdp.send("Target.getTargets");
  const pageTarget = targetInfos.find((t) => t.type === "page" && t.url.startsWith(pageUrl));
  if (!pageTarget) throw new Error("page target not found");
  const { sessionId: pageSession } = await cdp.send("Target.attachToTarget", { targetId: pageTarget.targetId, flatten: true });
  const found = new Promise((resolveFound) => {
    cdp.listeners.push((msg) => {
      if (msg.method === "Target.attachedToTarget" && /cpu\.worker/.test(msg.params.targetInfo.url)) {
        resolveFound(msg.params.sessionId);
      }
    });
  });
  await cdp.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, pageSession);
  return Promise.race([found, sleep(10_000).then(() => { throw new Error("CPU worker target not found"); })]);
}

function summariseProfile(profile, top) {
  const byId = new Map(profile.nodes.map((n) => [n.id, n]));
  const selfUs = new Map();
  for (let i = 0; i < profile.samples.length; i++) {
    const id = profile.samples[i];
    selfUs.set(id, (selfUs.get(id) ?? 0) + (profile.timeDeltas[i] ?? 0));
  }
  const byName = new Map();
  let total = 0;
  for (const [id, us] of selfUs) {
    const frame = byId.get(id)?.callFrame;
    const file = frame?.url ? frame.url.split("/").pop() : "";
    const name = `${frame?.functionName || "(anonymous)"}${file ? `  [${file}]` : ""}`;
    byName.set(name, (byName.get(name) ?? 0) + us);
    total += us;
  }
  return [...byName.entries()].sort((a, b) => b[1] - a[1]).slice(0, top)
    .map(([name, us]) => `${((100 * us) / total).toFixed(1).padStart(5)}%  ${(us / 1000).toFixed(0).padStart(7)} ms  ${name}`);
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.build) await run("npx", ["vite", "build", "--logLevel", "warn"]);
  const preview = spawn("npx", ["vite", "preview", "--port", String(opts.port), "--strictPort"], { cwd: WEB_DIR, stdio: "ignore" });
  const baseUrl = `http://localhost:${opts.port}/`;
  const browser = await chromium.launch({
    channel: "chromium",
    headless: true,
    args: [...(opts.software ? SOFTWARE_GPU_ARGS : HARDWARE_GPU_ARGS), `--remote-debugging-port=${opts.debugPort}`],
  });
  try {
    await waitForUrl(baseUrl, 60_000);
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
    page.on("console", (m) => {
      if (m.type() === "error" && /\[(ERROR|WARN )\]|failed|crash/i.test(m.text())) console.log(`  console: ${m.text()}`);
    });
    const pageUrl = opts.urlParams ? `${baseUrl}?${opts.urlParams}` : baseUrl;
    await page.goto(pageUrl);
    await page.getByTestId("load-panel").waitFor({ timeout: 30_000 });
    await page.getByTestId("load-input").setInputFiles(opts.game);
    await page.getByTestId("load-success").waitFor({ timeout: 120_000 });
    const worker = await (async () => {
      for (let i = 0; i < 100; i++) {
        const w = page.workers().find((x) => /cpu\.worker/.test(x.url()));
        if (w) return w;
        await sleep(100);
      }
      throw new Error("CPU worker not found");
    })();
    const sample = async () => ({
      at: Date.now(),
      perf: await worker.evaluate(() => ({ ...globalThis.__VOLAND_CPU_PERF__ })),
      fps: await page.evaluate(() => window.__VOLAND_STATS__?.fps ?? 0),
    });

    /* Warm up to a known slice count. */
    let s = await sample();
    let lastPrint = 0;
    while (s.perf.slices < opts.warmupSlices) {
      const state = await page.getByTestId("run-state").getAttribute("data-state");
      if (state === "crashed" || state === "deadlock" || state === "exited") throw new Error(`run state ${state}`);
      if (s.at - lastPrint > 15_000) {
        lastPrint = s.at;
        console.log(`warmup: ${s.perf.slices} slices, ${(s.perf.ticks / TICKS_PER_SECOND).toFixed(1)} s virtual`);
      }
      await sleep(1000);
      s = await sample();
    }
    const warm = s;
    const warmupWallS = (warm.perf.sliceMs + warm.perf.yieldMs) / 1000;
    console.log(`warmup done: ${warm.perf.slices} slices in ${warmupWallS.toFixed(1)} s worker time ` +
                `(${(warm.perf.slices / warmupWallS).toFixed(0)} slices/s)`);

    const fpsSamples = [];
    for (let t = 0; t < opts.seconds; t++) {
      await sleep(1000);
      fpsSamples.push(await page.evaluate(() => window.__VOLAND_STATS__?.fps ?? 0));
    }
    const end = await sample();
    const d = (k) => end.perf[k] - warm.perf[k];
    const wallMs = end.at - warm.at;
    const wallS = wallMs / 1000;
    const pct = (ms) => `${((100 * ms) / wallMs).toFixed(1)}%`;
    const fpsAvg = fpsSamples.reduce((a, b) => a + b, 0) / Math.max(1, fpsSamples.length);
    console.log("");
    console.log(`measured ${wallS.toFixed(1)} s from slice ${warm.perf.slices}:`);
    console.log(`  slices/s        ${(d("slices") / wallS).toFixed(0)}`);
    console.log(`  ticks/s         ${(d("ticks") / wallS / 1e6).toFixed(2)} M  (${((100 * d("ticks")) / wallS / TICKS_PER_SECOND).toFixed(1)}% of real time)`);
    console.log(`  SVCs/s          ${(d("svcs") / wallS).toFixed(0)}`);
    console.log(`  fps             ${fpsAvg.toFixed(2)}  [${fpsSamples.join(" ")}]`);
    console.log(`  in run_slice    ${pct(d("sliceMs"))}`);
    console.log(`    file reads    ${pct(d("fileReadMs"))}  (${d("fileReads")} reads, ${(d("fileReadBytes") / 1048576).toFixed(1)} MiB)`);
    console.log(`  yielding        ${pct(d("yieldMs"))}  (${d("bursts")} bursts)`);
    console.log(`  GPU stalls      ${d("gpuStalls")}  (${(d("gpuBytes") / 1048576 / wallS).toFixed(1)} MiB/s streamed)`);
    console.log(`RESULT slices_per_s=${(d("slices") / wallS).toFixed(0)} ticks_per_s=${(d("ticks") / wallS).toFixed(0)} fps=${fpsAvg.toFixed(2)}`);

    if (opts.profile > 0) {
      const cdp = await Cdp.connect(opts.debugPort);
      try {
        const session = await attachToCpuWorker(cdp, baseUrl);
        await cdp.send("Profiler.enable", {}, session);
        await cdp.send("Profiler.setSamplingInterval", { interval: 500 }, session);
        await cdp.send("Profiler.start", {}, session);
        await sleep(opts.profile * 1000);
        const { profile } = await cdp.send("Profiler.stop", {}, session);
        mkdirSync(opts.outDir, { recursive: true });
        const file = join(opts.outDir, `cpu-worker-${Date.now()}.cpuprofile`);
        writeFileSync(file, JSON.stringify(profile));
        console.log(`\nCPU worker profile (${opts.profile} s, self time), saved to ${file}:`);
        for (const line of summariseProfile(profile, 30)) console.log(`  ${line}`);
      } finally {
        cdp.close();
      }
    }
  } finally {
    await browser.close();
    preview.kill();
  }
}

main().catch((e) => {
  console.error(e instanceof Error ? e.stack : e);
  process.exit(1);
});
