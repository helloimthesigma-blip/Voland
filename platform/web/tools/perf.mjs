#!/usr/bin/env node
/**
 * Browser perf harness: boots a game in Chromium on the host's real GPU
 * and prints how fast the CPU worker runs it.
 *
 *   node tools/perf.mjs --game FILE [--warmup-slices N] [--seconds S]
 *                       [--profile S] [--port P] [--debug-port P] [--no-build] [--software]
 *                       [--url-params "a=1&b=2"] [--browser-arg ARG]...
 *                       [--press SLICE:KEY:SLICES]... [--shot FILE.png]
 *                       [--phases NAME:SLICE,NAME:SLICE,...,end:SLICE] [--json FILE]
 *                       [--restore-saves BACKUP.tar] [--user-data-dir DIR]
 *
 * - Builds the app (vite build; the core must already be staged by
 *   `cmake --build --preset web`) and serves it with `vite preview` on a
 *   free port (and a free DevTools port), so it never collides with
 *   another checkout running it.
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
 * - --press holds keyboard KEY (a Playwright key name; "z" is the A
 *   button) from scheduler slice SLICE for SLICES slices, like
 *   voland-cli's --input. Silksong's gameplay recipe is
 *   --press 860000:z:3000 --press 940000:z:3000 --press 1020000:z:3000
 *   --press 3000000:z:3000 --press 3100000:z:3000 --press 3200000:z:3000
 *   with --warmup-slices 4800000. Presses land within ~250 ms of their
 *   slice, so a browser run is close to, not identical with, the CLI's.
 * - --clock slices|ticks|presents picks the unit of --warmup-slices and
 *   --press points (default slices). With wall-clock pacing on (the web
 *   default), slices and virtual ticks follow the wall clock rather than
 *   game progress, so slice-counted recipes mis-time: use presents (frames
 *   the game presented) - or pass --url-params pacing=0 to keep slices.
 *   No counter tracks game progress exactly under pacing (timed screens
 *   take fewer frames), so menus are best driven by --mash KEY:EVERY_MS:
 *   KEY is pressed (200 ms) every EVERY_MS during the warm-up. Silksong to
 *   gameplay under pacing: --clock presents --warmup-slices 6500 --mash z:4000
 *   (A through title, profile, New Game, the opening video and the bind
 *   prompt).
 * - --phases profiles the run from load in phases that start at the given
 *   slices (the last entry only ends the previous one), and prints per
 *   phase: worker time, slices, virtual time, file reads, GPU stream bytes,
 *   and the CPU worker's profile by code kind and top functions. Warmup
 *   then runs to the last slice. For load time, e.g.
 *   --phases boot:0,menus:860000,newgame:3200000,end:4800000
 * - CPU time: alongside wall time the harness samples the page's renderer
 *   process CPU seconds (ps; it hosts the CPU worker, the GPU worker and the
 *   core's pthreads), and reports CPU seconds per virtual second - a
 *   figure much less sensitive to a busy machine than wall time.
 * - --web-dir DIR builds and serves another checkout's platform/web (its
 *   core staged there), so this harness can measure older commits.
 * - --restore-saves BACKUP.tar imports a saves backup (the Saves panel's
 *   "Back up saves" file) before the game loads, so a run can start from
 *   any save point.
 * - --json FILE writes the results (measurement window and phases) as JSON
 *   (used by tools/perf-track.mjs).
 * - --browser-arg passes a Chromium switch (repeatable), e.g.
 *   --browser-arg=--js-flags=--no-liftoff for a V8 tiering experiment.
 * - --expect-jit-async fails the run unless the JIT installed modules it
 *   compiled asynchronously (WebAssembly.compile) and none failed.
 *
 * Slices are deterministic for a given game and input, so the same
 * --warmup-slices reaches the same point natively:
 *   voland-cli run FILE --backend interpreter --budget 200000 \
 *     --max-slices N --gpu-stream /dev/null
 */
import { execFileSync, spawn } from "node:child_process";
import { createServer } from "node:net";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { chromium } from "@playwright/test";

let WEB_DIR = resolve(dirname(fileURLToPath(import.meta.url)), "..");
/* Virtual ticks per second (the Switch's 19.2 MHz counter). */
const TICKS_PER_SECOND = 19_200_000;
const HARDWARE_GPU_ARGS = ["--enable-unsafe-webgpu", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"];
const SOFTWARE_GPU_ARGS = ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader", "--enable-features=Vulkan"];

function parseArgs(argv) {
  const opts = {
    game: "", clock: "slices", warmupSlices: 860_000, seconds: 30, profile: 0, port: 0, debugPort: 0,
    build: true, software: false, urlParams: "", outDir: tmpdir(), browserArgs: [], presses: [], shot: "", restoreSaves: "", phases: [], json: "", expectJitAsync: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => argv[++i] ?? "";
    if (a === "--game") opts.game = next();
    else if (a === "--warmup-slices") opts.warmupSlices = Number(next());
    else if (a === "--clock") {
      opts.clock = next();
      if (!["slices", "ticks", "presents"].includes(opts.clock)) throw new Error("--clock wants slices, ticks or presents");
    }
    else if (a === "--mash") {
      const [key, every] = next().split(":");
      if (!key || !(Number(every) > 0)) throw new Error("--mash wants KEY:EVERY_MS");
      opts.mash = { key, every: Number(every), last: 0 };
    }
    else if (a === "--seconds") opts.seconds = Number(next());
    else if (a === "--profile") opts.profile = Number(next());
    else if (a === "--port") opts.port = Number(next());
    else if (a === "--debug-port") opts.debugPort = Number(next());
    else if (a === "--no-build") opts.build = false;
    else if (a === "--expect-jit-async") opts.expectJitAsync = true;
    else if (a === "--software") opts.software = true;
    else if (a === "--user-data-dir") opts.userDataDir = next();
    else if (a === "--url-params") opts.urlParams = next();
    else if (a === "--out-dir") opts.outDir = next();
    else if (a === "--shot") opts.shot = next();
    else if (a === "--restore-saves") opts.restoreSaves = next();
    else if (a === "--json") opts.json = next();
    else if (a === "--web-dir") WEB_DIR = resolve(next()); /* another checkout's platform/web */
    else if (a === "--phases") {
      opts.phases = next().split(",").map((entry) => {
        const [name, slice] = entry.split(":");
        if (!name || !Number.isFinite(Number(slice))) throw new Error("--phases wants NAME:SLICE,...");
        return { name, slice: Number(slice) };
      });
      if (opts.phases.length < 2) throw new Error("--phases needs at least a start and an end");
      opts.warmupSlices = opts.phases[opts.phases.length - 1].slice;
    }
    else if (a === "--browser-arg") opts.browserArgs.push(next());
    else if (a.startsWith("--browser-arg=")) opts.browserArgs.push(a.slice("--browser-arg=".length));
    else if (a === "--press") {
      const [slice, key, hold] = next().split(":");
      if (!key || !Number.isFinite(Number(slice)) || !Number.isFinite(Number(hold))) throw new Error("--press wants SLICE:KEY:SLICES");
      opts.presses.push({ slice: Number(slice), key, hold: Number(hold), state: "pending" });
    }
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

/* CPU seconds of the renderer process(es) under `browserPid` (ps TIME). */
function rendererCpuSeconds(browserPid) {
  const table = execFileSync("ps", ["-A", "-o", "pid=,ppid=,time=,command="], { encoding: "utf8", maxBuffer: 64 << 20 });
  let total = 0;
  for (const line of table.split("\n")) {
    const m = /^\s*(\d+)\s+(\d+)\s+(\S+)\s+(.*)$/.exec(line);
    if (!m || Number(m[2]) !== browserPid || !/--type=renderer/.test(m[4]) || /--extension-process/.test(m[4])) continue;
    const [clock, days] = m[3].split("-").reverse();
    const parts = clock.split(":").map(Number).reverse(); /* ss.cc, mm, hh */
    total += (parts[0] ?? 0) + 60 * (parts[1] ?? 0) + 3600 * (parts[2] ?? 0) + 86400 * Number(days ?? 0);
  }
  return total;
}

/* The browser process Playwright started: our child running Chromium. */
function chromiumChildPid() {
  const table = execFileSync("ps", ["-A", "-o", "pid=,ppid=,command="], { encoding: "utf8", maxBuffer: 64 << 20 });
  for (const line of table.split("\n")) {
    const m = /^\s*(\d+)\s+(\d+)\s+(.*)$/.exec(line);
    if (m && Number(m[2]) === process.pid && /chrom/i.test(m[3]) && !/--type=/.test(m[3])) return Number(m[1]);
  }
  return 0;
}

function loadAverage() {
  return execFileSync("sysctl", ["-n", "vm.loadavg"], { encoding: "utf8" }).replace(/[{}]/g, "").trim().split(/\s+/).map(Number)[0] ?? 0;
}

/* A free TCP port, so several checkouts can run the harness at once. */
function freePort() {
  return new Promise((resolvePort, reject) => {
    const server = createServer();
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const { port } = server.address();
      server.close(() => resolvePort(port));
    });
  });
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

/* Self time by kind of code: the core module, wasm compiled at run time
 * (the JIT's modules), JS, and V8's own buckets. */
function categoriseProfile(profile) {
  const byId = new Map(profile.nodes.map((n) => [n.id, n]));
  const totals = new Map();
  let total = 0;
  for (let i = 0; i < profile.samples.length; i++) {
    const frame = byId.get(profile.samples[i])?.callFrame;
    const us = profile.timeDeltas[i] ?? 0;
    const name = frame?.functionName ?? "";
    const url = frame?.url ?? "";
    const kind = /^\((idle|program|garbage collector)\)$/.test(name) ? name
      : url.endsWith("switch_core.wasm") ? "core wasm"
      : url.startsWith("wasm://") || (url === "" && /^(\$|wasm-function)/.test(name)) ? "run-time wasm (JIT modules)"
      : url.endsWith("switch_core.js") ? "core JS glue"
      : "other JS";
    totals.set(kind, (totals.get(kind) ?? 0) + us);
    total += us;
  }
  return [...totals.entries()].sort((a, b) => b[1] - a[1])
    .map(([kind, us]) => `${((100 * us) / total).toFixed(1).padStart(5)}%  ${kind}`);
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (!opts.port) opts.port = await freePort();
  if (!opts.debugPort) opts.debugPort = await freePort();
  if (opts.build) await run("npx", ["vite", "build", "--logLevel", "warn"]);
  const preview = spawn("npx", ["vite", "preview", "--port", String(opts.port), "--strictPort"], { cwd: WEB_DIR, stdio: "ignore" });
  const baseUrl = `http://localhost:${opts.port}/`;
  const launch = {
    channel: "chromium",
    headless: true,
    args: [...(opts.software ? SOFTWARE_GPU_ARGS : HARDWARE_GPU_ARGS), `--remote-debugging-port=${opts.debugPort}`,
           ...opts.browserArgs],
  };
  /* --user-data-dir DIR: a persistent profile, so OPFS (saves, shader
   * cache) and IndexedDB carry over between runs. */
  const browser = opts.userDataDir
    ? await chromium.launchPersistentContext(opts.userDataDir, { ...launch, viewport: { width: 1280, height: 800 } })
    : await chromium.launch(launch);
  const browserPid = chromiumChildPid();
  const results = { loadAvgStart: loadAverage(), phases: [], window: null };
  try {
    await waitForUrl(baseUrl, 60_000);
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
    page.on("console", (m) => {
      /* The configuration actually in effect, as the CPU worker logs it. */
      const backend = /backend=(\w+)/.exec(m.text());
      if (backend) results.backend = backend[1];
      const cores = /guest threads on (?:the serial scheduler|(\d+) host core)/.exec(m.text());
      if (cores) results.hostCores = cores[1] ? Number(cores[1]) : 0;
      if ((m.type() === "error" && /\[(ERROR|WARN )\]|failed|crash/i.test(m.text())) || /core: |svcBreak|shader cache/.test(m.text()))
        console.log(`  console: ${m.text()}`);
      /* Parallel guest threads' periodic timing report (docs/PARALLEL.md "Measuring"). */
      if (/\[parallel\] /.test(m.text())) console.log(`  ${m.text().replace(/^\[INFO \] /, "")}`);
    });
    const pageUrl = opts.urlParams ? `${baseUrl}?${opts.urlParams}` : baseUrl;
    await page.goto(pageUrl);
    await page.getByTestId("load-panel").waitFor({ timeout: 30_000 });
    if (opts.restoreSaves) {
      await page.getByTestId("saves-import-input").setInputFiles(opts.restoreSaves);
      const note = page.getByTestId("saves-note");
      await note.waitFor({ timeout: 30_000 });
      console.log(`restore saves: ${await note.textContent()}`);
    }
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
    /* Count run-time wasm compiles (the JIT's modules) inside the CPU worker:
     * wraps WebAssembly.Module/Instance there; read back with each sample. */
    await worker.evaluate(() => {
      const g = globalThis;
      if (g.__VOLAND_WASM_COMPILES__) return;
      const stats = { modules: 0, bytes: 0, moduleMs: 0, instanceMs: 0 };
      g.__VOLAND_WASM_COMPILES__ = stats;
      const RealModule = WebAssembly.Module, RealInstance = WebAssembly.Instance;
      const ModuleProxy = new Proxy(RealModule, { construct(target, args) {
        const t = performance.now();
        const m = Reflect.construct(target, args);
        stats.moduleMs += performance.now() - t;
        stats.modules++;
        stats.bytes += args[0]?.byteLength ?? 0;
        return m;
      } });
      const InstanceProxy = new Proxy(RealInstance, { construct(target, args) {
        const t = performance.now();
        const i = Reflect.construct(target, args);
        stats.instanceMs += performance.now() - t;
        return i;
      } });
      Object.defineProperty(WebAssembly, "Module", { value: ModuleProxy, configurable: true, writable: true });
      Object.defineProperty(WebAssembly, "Instance", { value: InstanceProxy, configurable: true, writable: true });
    });
    const sample = async () => ({
      at: Date.now(),
      cpuS: browserPid ? rendererCpuSeconds(browserPid) : 0,
      perf: await worker.evaluate(() => {
        const c = globalThis.__VOLAND_WASM_COMPILES__ ?? { modules: 0, bytes: 0, moduleMs: 0, instanceMs: 0 };
        return { ...globalThis.__VOLAND_CPU_PERF__, wasmModules: c.modules, wasmBytes: c.bytes,
                 wasmModuleMs: c.moduleMs, wasmInstanceMs: c.instanceMs,
                 /* the JIT's asynchronous compiles (WebAssembly.compile, not counted above) */
                 jitAsyncInstalled: globalThis.volandJitAsyncInstalled ?? 0,
                 jitAsyncFailed: globalThis.volandJitAsyncFailed ?? 0 };
      }),
      fps: await page.evaluate(() => window.__VOLAND_STATS__?.fps ?? 0),
      presents: await page.evaluate(() => window.__VOLAND_STATS__?.presents ?? 0),
    });
    /* The recipe clock (--clock): what --warmup-slices and --press count. */
    const progress = (s) => opts.clock === "ticks" ? s.perf.ticks : opts.clock === "presents" ? s.presents : s.perf.slices;

    /* Scripted input: press and release keys at slice counts. */
    const servicePresses = async (slices) => {
      for (const press of opts.presses) {
        if (press.state === "pending" && slices >= press.slice) {
          await page.keyboard.down(press.key);
          press.state = "down";
          console.log(`press ${press.key} at slice ${slices}`);
        } else if (press.state === "down" && slices >= press.slice + press.hold) {
          await page.keyboard.up(press.key);
          press.state = "done";
        }
      }
    };

    /* Phase profiling: one CDP profile per phase, switched at slice counts. */
    const phaseState = { cdp: null, session: "", index: -1, startSample: null, rows: [] };
    if (opts.phases.length) {
      phaseState.cdp = await Cdp.connect(opts.debugPort);
      phaseState.session = await attachToCpuWorker(phaseState.cdp, baseUrl);
      await phaseState.cdp.send("Profiler.enable", {}, phaseState.session);
      await phaseState.cdp.send("Profiler.setSamplingInterval", { interval: 1000 }, phaseState.session);
    }
    const servicePhases = async (current) => {
      if (!opts.phases.length) return;
      while (phaseState.index + 1 < opts.phases.length && current.perf.slices >= opts.phases[phaseState.index + 1].slice) {
        const { cdp, session } = phaseState;
        if (phaseState.index >= 0) {
          const { profile } = await cdp.send("Profiler.stop", {}, session);
          const phase = opts.phases[phaseState.index];
          const a = phaseState.startSample.perf, b = current.perf;
          const file = join(opts.outDir, `phase-${phase.name}-${Date.now()}.cpuprofile`);
          mkdirSync(opts.outDir, { recursive: true });
          writeFileSync(file, JSON.stringify(profile));
          const workerS = (b.sliceMs + b.yieldMs - a.sliceMs - a.yieldMs) / 1000;
          phaseState.rows.push({
            name: phase.name, from: a.slices, to: b.slices, workerS,
            wallS: (current.at - phaseState.startSample.at) / 1000,
            cpuS: current.cpuS - phaseState.startSample.cpuS,
            virtualS: (b.ticks - a.ticks) / TICKS_PER_SECOND,
            fileReads: b.fileReads - a.fileReads, fileMiB: (b.fileReadBytes - a.fileReadBytes) / 1048576,
            fileS: (b.fileReadMs - a.fileReadMs) / 1000, gpuMiB: (b.gpuBytes - a.gpuBytes) / 1048576,
            gpuStalls: b.gpuStalls - a.gpuStalls, waitS: ((b.streamWaitMs ?? 0) - (a.streamWaitMs ?? 0)) / 1000,
            chunks: (b.fileChunks ?? 0) - (a.fileChunks ?? 0), modules: b.wasmModules - a.wasmModules,
            moduleMiB: (b.wasmBytes - a.wasmBytes) / 1048576,
            compileS: (b.wasmModuleMs + b.wasmInstanceMs - a.wasmModuleMs - a.wasmInstanceMs) / 1000,
            kinds: categoriseProfile(profile), top: summariseProfile(profile, 25), file,
          });
        }
        phaseState.index++;
        phaseState.startSample = current;
        if (phaseState.index + 1 < opts.phases.length) await cdp.send("Profiler.start", {}, session);
      }
    };

    /* Warm up to a known slice count. */
    let s = await sample();
    await servicePhases(s);
    let lastPrint = 0;
    while (progress(s) < opts.warmupSlices) {
      const state = await page.getByTestId("run-state").getAttribute("data-state");
      if (state === "crashed" || state === "deadlock" || state === "exited") throw new Error(`run state ${state}`);
      if (s.at - lastPrint > 15_000) {
        lastPrint = s.at;
        console.log(`warmup: ${s.perf.slices} slices, ${(s.perf.ticks / TICKS_PER_SECOND).toFixed(1)} s virtual, ${s.presents} frames`);
      }
      await servicePresses(progress(s));
      if (opts.mash && Date.now() - opts.mash.last >= opts.mash.every) {
        opts.mash.last = Date.now();
        await page.keyboard.down(opts.mash.key);
        await sleep(200);
        await page.keyboard.up(opts.mash.key);
      }
      await sleep(250);
      s = await sample();
      await servicePhases(s);
    }
    const warm = s;
    if (opts.phases.length) {
      phaseState.cdp.close();
      results.phases = phaseState.rows.map(({ kinds, top, ...row }) => row);
      console.log("");
      console.log("phase       slices                 worker s   wall s   CPU s  virtual s  file reads (MiB, s)     GPU MiB  stalls (s)  wasm modules (MiB, s)  new chunks");
      for (const r of phaseState.rows) {
        console.log(`${r.name.padEnd(10)}  ${String(r.from).padStart(8)}-${String(r.to).padEnd(9)}  ${r.workerS.toFixed(1).padStart(9)}  ${r.wallS.toFixed(1).padStart(7)}  ${r.cpuS.toFixed(1).padStart(6)}` +
                    `  ${r.virtualS.toFixed(1).padStart(9)}  ${String(r.fileReads).padStart(6)} (${r.fileMiB.toFixed(0)}, ${r.fileS.toFixed(1)})`.padEnd(26) +
                    `  ${r.gpuMiB.toFixed(0).padStart(8)}  ${String(r.gpuStalls).padStart(6)} (${r.waitS.toFixed(1)})  ${String(r.modules).padStart(6)} (${r.moduleMiB.toFixed(1)}, ${r.compileS.toFixed(1)})  ${String(r.chunks).padStart(6)}`);
      }
      for (const r of phaseState.rows) {
        console.log(`\n[${r.name}] profile (${r.file}):`);
        for (const line of r.kinds) console.log(`  ${line}`);
        for (const line of r.top) console.log(`    ${line}`);
      }
    }
    const warmupWallS = (warm.perf.sliceMs + warm.perf.yieldMs) / 1000;
    console.log(`warmup done: ${warm.perf.slices} slices in ${warmupWallS.toFixed(1)} s worker time ` +
                `(${(warm.perf.slices / warmupWallS).toFixed(0)} slices/s)`);

    const fpsSamples = [];
    for (let t = 0; t < opts.seconds; t++) {
      for (let q = 0; q < 4; q++) {
        await sleep(250);
        if (opts.presses.length) await servicePresses(progress(await sample()));
      }
      fpsSamples.push(await page.evaluate(() => window.__VOLAND_STATS__?.fps ?? 0));
    }
    const end = await sample();
    if (opts.shot) {
      const box = await page.getByTestId("screen").boundingBox();
      if (box) await page.screenshot({ clip: box, path: opts.shot });
    }
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
    console.log(`  stream waits    ${(d("streamWaitMs") / 1000).toFixed(1)} s`);
    console.log(`  GPU stalls      ${d("gpuStalls")}  (${(d("gpuBytes") / 1048576 / wallS).toFixed(1)} MiB/s streamed)`);
    const cpuS = end.cpuS - warm.cpuS;
    console.log(`  CPU s           ${cpuS.toFixed(1)} (${(cpuS / Math.max(1e-9, d("ticks") / TICKS_PER_SECOND)).toFixed(2)} CPU s per virtual s)`);
    results.window = {
      fromSlice: warm.perf.slices, wallS, cpuS, slicesPerS: d("slices") / wallS, ticksPerS: d("ticks") / wallS,
      virtualS: d("ticks") / TICKS_PER_SECOND, fps: fpsAvg, wasmModules: d("wasmModules"),
    };
    results.warmupWorkerS = warmupWallS;
    console.log(`RESULT slices_per_s=${(d("slices") / wallS).toFixed(0)} ticks_per_s=${(d("ticks") / wallS).toFixed(0)} fps=${fpsAvg.toFixed(2)}`);
    {
      const finalPerf = (await sample()).perf;
      console.log(`JIT async compiles: ${finalPerf.jitAsyncInstalled} installed, ${finalPerf.jitAsyncFailed} failed`);
      if (opts.expectJitAsync && (finalPerf.jitAsyncInstalled === 0 || finalPerf.jitAsyncFailed > 0)) {
        console.error("FAIL: --expect-jit-async: the JIT's asynchronous compiles did not install cleanly");
        process.exitCode = 1;
      }
    }

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
        for (const line of categoriseProfile(profile)) console.log(`  ${line}`);
        console.log("");
        for (const line of summariseProfile(profile, 30)) console.log(`  ${line}`);
      } finally {
        cdp.close();
      }
    }
    results.loadAvgEnd = loadAverage();
    if (opts.json) writeFileSync(opts.json, JSON.stringify(results, null, 2));
  } finally {
    await browser.close();
    preview.kill();
  }
}

main().catch((e) => {
  console.error(e instanceof Error ? e.stack : e);
  process.exit(1);
});
