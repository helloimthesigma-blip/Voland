/**
 * Replays a GPU stream recorded by `voland-cli run ... --gpu-stream FILE`
 * through the GPU worker's record executor (workers/gpu-executor.ts) on
 * WebGPU in headless Chromium, and writes the presented frames as PPM -
 * the native check of the WebGPU renderer against the software reference
 * (compare with voland-cli --dump-frame output).
 *
 *   node tools/replay-gpu-stream.mjs FILE.vgs OUT_PREFIX [--every N] [--adapter swiftshader|default]
 *
 * Writes OUT_PREFIX.<present>.ppm every N presents (default: the last only)
 * and OUT_PREFIX.last.ppm; prints executor statistics and timings.
 */
import { chromium } from "@playwright/test";
import { readFileSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import ts from "typescript";

const args = process.argv.slice(2);
const file = args[0];
const prefix = args[1];
if (!file || !prefix) {
  console.error("usage: node tools/replay-gpu-stream.mjs FILE.vgs OUT_PREFIX [--every N] [--adapter swiftshader|default]");
  process.exit(2);
}
const every = Number(args[args.indexOf("--every") + 1] ?? 0) || 0;
const adapter = args.includes("--adapter") ? args[args.indexOf("--adapter") + 1] : "swiftshader";

/* The executor and its record bindings as one classic script. */
const here = dirname(fileURLToPath(import.meta.url));
function transpile(path) {
  const source = readFileSync(join(here, "..", path), "utf8");
  const js = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext } }).outputText;
  return js.replace(/^import[\s\S]*?from\s+"[^"]+";\s*$/gm, "").replace(/^export\s+/gm, "");
}
const bundle = `${transpile("bindings/gpu-records.ts")}\n${transpile("workers/gpu-executor.ts")}\nwindow.GpuExecutor = GpuExecutor;`;

const stream = readFileSync(file);
const launchArgs = ["--enable-unsafe-webgpu", "--enable-features=Vulkan"];
launchArgs.push(adapter === "swiftshader" ? "--use-webgpu-adapter=swiftshader" : "--use-angle=metal");
const browser = await chromium.launch({ args: launchArgs });
const page = await browser.newPage();
page.on("console", (m) => console.log(`[page] ${m.text()}`));
page.on("crash", () => console.log("[page] crashed"));
page.on("pageerror", (e) => console.log(`[page] error: ${e.message}`));
/* localhost is a secure context (WebGPU) and serves the stream without
 * routing it through the automation protocol. */
const server = createServer((req, res) => {
  if (req.url === "/stream") {
    res.writeHead(200, { "content-type": "application/octet-stream", "content-length": stream.byteLength });
    res.end(stream);
    return;
  }
  res.writeHead(200, { "content-type": "text/html" });
  res.end("<!doctype html><title>replay</title>");
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
await page.goto(`http://localhost:${server.address().port}/`);
await page.addScriptTag({ content: bundle });
if (process.env.TRACE_RECORDS) await page.evaluate(() => { globalThis.TRACE_RECORDS = true; });
if (process.env.TRIVIAL_SHADERS) await page.evaluate(() => { globalThis.TRIVIAL_SHADERS = true; });
if (process.env.MEASURE) await page.evaluate(() => { globalThis.MEASURE = true; });
if (process.env.NO_DEPTH) await page.evaluate(() => { globalThis.NO_DEPTH = true; });
if (process.env.SKIP_TYPES) await page.evaluate((t) => { globalThis.SKIP_TYPES = t; }, process.env.SKIP_TYPES.split(",").map(Number));
const result = await page.evaluate(async ({ every }) => {
  const a = await navigator.gpu.requestAdapter();
  const features = ["rg11b10ufloat-renderable", "depth32float-stencil8", "float32-filterable"].filter((f) => a.features.has(f));
  const device = await a.requestDevice({ requiredFeatures: features });
  const errors = [];
  device.addEventListener("uncapturederror", (e) => { if (errors.length < 20) errors.push(e.error.message.split("\n")[0]); });
  const bytes = new Uint8Array(await (await fetch("/stream")).arrayBuffer());
  const toBase64 = (u8) => {
    let s = "";
    for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
    return btoa(s);
  };
  let target = null;
  let presents = 0;
  const pending = [];
  const readback = (index) => {
    const { width: w, height: h } = target;
    const bytesPerRow = Math.ceil((w * 4) / 256) * 256;
    const buffer = device.createBuffer({ size: bytesPerRow * h, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: target.texture }, { buffer, bytesPerRow }, [w, h]);
    device.queue.submit([enc.finish()]);
    pending.push(buffer.mapAsync(GPUMapMode.READ).then(() => {
      const src = new Uint8Array(buffer.getMappedRange());
      const rgb = new Uint8Array(w * h * 3);
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        const s = y * bytesPerRow + x * 4, d = (y * w + x) * 3;
        rgb[d] = src[s]; rgb[d + 1] = src[s + 1]; rgb[d + 2] = src[s + 2];
      }
      buffer.destroy();
      return { index, w, h, rgb: toBase64(rgb) };
    }));
  };
  const host = {
    presentTarget(w, h) {
      if (!target || target.width !== w || target.height !== h) {
        target?.texture.destroy();
        const texture = device.createTexture({ size: [w, h], format: "rgba8unorm", usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
        target = { texture, view: texture.createView(), format: "rgba8unorm", width: w, height: h };
      }
      return target;
    },
    presented() {
      presents++;
      if (every && presents % every === 0) readback(presents);
    },
    log(level, message) { console.log(`${level}: ${message}`); },
  };
  const ex = new window.GpuExecutor(device, host);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const t0 = performance.now();
  let records = 0;
  const byType = {};
  for (let at = 0; at + 8 <= bytes.byteLength;) {
    const type = view.getUint32(at, true), size = view.getUint32(at + 4, true);
    if (globalThis.TRACE_RECORDS) console.log(`record ${records} type ${type} size ${size}`);
    const r0 = performance.now();
    if (globalThis.MEASURE && type === 6 && records > 8000 && records < 12000) {
      ex.flush(); await device.queue.onSubmittedWorkDone();
      const m0 = performance.now();
      ex.execute(type, bytes.subarray(at + 8, at + 8 + size));
      ex.flush(); await device.queue.onSubmittedWorkDone();
      const shader = view.getUint32(at + 8, true);
      const m = (globalThis.MEASURED ??= {});
      const e = (m[shader] ??= { n: 0, ms: 0 }); e.n++; e.ms += performance.now() - m0;
    } else if (!(globalThis.SKIP_TYPES ?? []).includes(type)) ex.execute(type, bytes.subarray(at + 8, at + 8 + size));
    const e = (byType[type] ??= { n: 0, ms: 0, bytes: 0 });
    e.n++; e.ms += performance.now() - r0; e.bytes += size;
    at += 8 + size;
    records++;
  }
  const cpuMs = performance.now() - t0;
  if (globalThis.MEASURED) console.log(`per-shader draw ms: ${JSON.stringify(Object.fromEntries(Object.entries(globalThis.MEASURED).map(([k, v]) => [k, { n: v.n, avg: +(v.ms / v.n).toFixed(3) }])))}`);
  ex.flush();
  await device.queue.onSubmittedWorkDone();
  const elapsed = performance.now() - t0;
  console.log(`prof ${JSON.stringify(ex.prof)}`); console.log(`CPU side ${cpuMs.toFixed(0)} ms; by record type ${JSON.stringify(Object.fromEntries(Object.entries(byType).map(([k, v]) => [k, { n: v.n, ms: Math.round(v.ms), MB: +(v.bytes / 1048576).toFixed(1) }])))}`);
  if (target && !(every && presents % every === 0)) readback(presents);
  const out = await Promise.all(pending);
  return { records, presents, elapsed, stats: ex.stats, errors, frames: out, features };
}, { every });
await browser.close();
server.close();

for (const f of result.frames) {
  const header = Buffer.from(`P6\n${f.w} ${f.h}\n255\n`);
  const body = Buffer.from(f.rgb, "base64");
  if (f.index === result.presents) writeFileSync(`${prefix}.last.ppm`, Buffer.concat([header, body]));
  if (every && f.index % every === 0) writeFileSync(`${prefix}.${f.index}.ppm`, Buffer.concat([header, body]));
}
console.log(`replayed ${result.records} records, ${result.presents} presents in ${result.elapsed.toFixed(0)} ms (${adapter}; features ${result.features.join(",") || "none"})`);
console.log(`executor: ${JSON.stringify(result.stats)}`);
for (const e of result.errors) console.log(`WebGPU error: ${e}`);
