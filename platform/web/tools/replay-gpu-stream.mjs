/**
 * Replays a GPU stream recorded by `voland-cli run ... --gpu-stream FILE`
 * through the GPU worker's record executor (workers/gpu-executor.ts) on
 * WebGPU in headless Chromium, and writes the presented frames as PPM -
 * the native check of the WebGPU renderer against the software reference
 * (compare with voland-cli --dump-frame output).
 *
 *   node tools/replay-gpu-stream.mjs FILE.vgs OUT_PREFIX [--every N] [--at P1,P2,...]
 *                                    [--adapter swiftshader|default]
 *
 * Writes OUT_PREFIX.<present>.ppm every N presents and at each listed
 * present (voland-cli --gpu-stream --dump-frames-every logs the present
 * each slice multiple ended on), and OUT_PREFIX.last.ppm; prints executor
 * statistics and timings. The stream is read in chunks, so recordings far
 * larger than one ArrayBuffer (gameplay runs are gigabytes) replay too.
 *
 * --dump-targets P1,P2,... DIR: after those presents, every RGBA8 and
 * float (rgba16float, rg11b10ufloat, r16float; clamped to [0, 1]) render
 * target as DIR/p<P>-tex<id>-<w>x<h>.pam (RGBA), to compare with
 * VOLAND_DUMP_SURFACES output from a software run (voland-cli).
 *
 * Diagnostics (environment): DRAW_TRACE=P sums the RGBA8 targets after
 * each draw of present P, or with TRACE_TARGET=id that one target (float
 * formats too); TRACE_SHAPES=P prints each draw's shader, colour targets
 * and depth state; NO_SHADOW_COMPARE=1 makes every depth compare pass.
 */
import { chromium } from "@playwright/test";
import { readFileSync, statSync, createReadStream, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import ts from "typescript";

const args = process.argv.slice(2);
const file = args[0];
const prefix = args[1];
if (!file || !prefix) {
  console.error("usage: node tools/replay-gpu-stream.mjs FILE.vgs OUT_PREFIX [--every N] [--at P1,P2,...] [--adapter swiftshader|default]");
  process.exit(2);
}
const every = args.includes("--every") ? Number(args[args.indexOf("--every") + 1]) || 0 : 0;
const dumpTargets = args.includes("--dump-targets") ? args[args.indexOf("--dump-targets") + 1].split(",").map(Number) : [];
const dumpDir = args.includes("--dump-targets") ? args[args.indexOf("--dump-targets") + 2] : "";
const at = args.includes("--at") ? args[args.indexOf("--at") + 1].split(",").map(Number).filter((n) => n > 0) : [];
const adapter = args.includes("--adapter") ? args[args.indexOf("--adapter") + 1] : "swiftshader";

/* The executor and its record bindings as one classic script. */
const here = dirname(fileURLToPath(import.meta.url));
function transpile(path) {
  const source = readFileSync(join(here, "..", path), "utf8");
  const js = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext } }).outputText;
  return js.replace(/^import[\s\S]*?from\s+"[^"]+";\s*$/gm, "").replace(/^export\s+/gm, "");
}
const bundle = `${transpile("bindings/gpu-records.ts")}\n${transpile("workers/shader-cache.ts")}\n` +
  `${transpile("workers/gpu-executor.ts")}\nwindow.GpuExecutor = GpuExecutor;`;

const streamBytes = statSync(file).size;
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
    res.writeHead(200, { "content-type": "application/octet-stream", "content-length": streamBytes });
    createReadStream(file, { highWaterMark: 8 << 20 }).pipe(res);
    return;
  }
  res.writeHead(200, { "content-type": "text/html" });
  res.end("<!doctype html><title>replay</title>");
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
await page.goto(`http://localhost:${server.address().port}/`);
let lastFrame = null;
const writeFrame = (name, w, h, b64) => writeFileSync(name, Buffer.concat([Buffer.from(`P6\n${w} ${h}\n255\n`), Buffer.from(b64, "base64")]));
await page.exposeFunction("saveTarget", (name, w, h, b64) => {
  writeFileSync(`${dumpDir}/${name}`, Buffer.concat([Buffer.from(`P7\nWIDTH ${w}\nHEIGHT ${h}\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n`), Buffer.from(b64, "base64")]));
});
await page.exposeFunction("saveFrame", (index, w, h, b64, last) => {
  if (last) lastFrame = { w, h, b64 };
  else writeFrame(`${prefix}.${index}.ppm`, w, h, b64);
});
await page.addScriptTag({ content: bundle });
if (process.env.TRACE_RECORDS) await page.evaluate(() => { globalThis.TRACE_RECORDS = true; });
if (process.env.TRIVIAL_SHADERS) await page.evaluate(() => { globalThis.TRIVIAL_SHADERS = true; });
if (process.env.MEASURE) await page.evaluate(() => { globalThis.MEASURE = true; });
if (process.env.DRAW_TRACE) await page.evaluate((n) => { globalThis.DRAW_TRACE = n; }, Number(process.env.DRAW_TRACE));
if (process.env.NO_SHADOW_COMPARE) await page.evaluate(() => { globalThis.NO_SHADOW_COMPARE = true; });
if (process.env.TRACE_SHAPES) await page.evaluate((n) => { globalThis.TRACE_SHAPES = n; }, Number(process.env.TRACE_SHAPES));
if (process.env.TEX_INFO) await page.evaluate((t) => { globalThis.TEX_INFO = t; }, process.env.TEX_INFO.split(",").map(Number));
if (process.env.TRACE_TARGET) await page.evaluate((n) => { globalThis.TRACE_TARGET = n; }, Number(process.env.TRACE_TARGET));
if (process.env.SYNC_DRAWS) await page.evaluate(() => { globalThis.SYNC_DRAWS = true; });
if (process.env.NO_CULL) await page.evaluate(() => { globalThis.NO_CULL = true; });
if (process.env.SOLID_SHADERS) await page.evaluate((t) => { globalThis.SOLID_SHADERS = t; }, process.env.SOLID_SHADERS.split(",").map(Number));
if (process.env.PRINT_SHADER) await page.evaluate((n) => { globalThis.PRINT_SHADER = n; }, Number(process.env.PRINT_SHADER));
if (process.env.CB_DUMP) await page.evaluate((t) => { globalThis.CB_DUMP = { shader: t[0], slot: t[1], words: t[2] }; }, process.env.CB_DUMP.split(",").map(Number));
if (process.env.TRACE_SIZES) await page.evaluate(() => { globalThis.TRACE_SIZES = true; });
if (process.env.NO_DEPTH) await page.evaluate(() => { globalThis.NO_DEPTH = true; });
if (process.env.SKIP_TYPES) await page.evaluate((t) => { globalThis.SKIP_TYPES = t; }, process.env.SKIP_TYPES.split(",").map(Number));
const result = await page.evaluate(async ({ every, at, dumpTargets }) => {
  const wanted = new Set(at);
  const targetsAt = new Set(dumpTargets);
  const a = await navigator.gpu.requestAdapter();
  const features = ["rg11b10ufloat-renderable", "depth32float-stencil8", "float32-filterable", "float32-blendable"].filter((f) => a.features.has(f));
  const device = await a.requestDevice({
    requiredFeatures: features,
    requiredLimits: { maxColorAttachmentBytesPerSample: a.limits.maxColorAttachmentBytesPerSample }, // as gpu.worker.ts
  });
  if (globalThis.NO_SHADOW_COMPARE) { /* experiment: every depth compare passes */
    const create = device.createShaderModule.bind(device);
    device.createShaderModule = (d) => create({ ...d, code: d.code.replace(/fn tcmp\(([^)]*)\)\s*->\s*bool\s*\{/, (m) => `${m} return true;`) });
  }
  const errors = [];
  device.addEventListener("uncapturederror", (e) => { if (errors.length < 20) errors.push(e.error.message.split("\n")[0]); });
  const toBase64 = (u8) => {
    let s = "";
    for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
    return btoa(s);
  };
  let target = null;
  let presents = 0;
  const pending = [];
  const readback = (index, last) => {
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
      return window.saveFrame(index, w, h, toBase64(rgb), last);
    }));
  };
  /* Float texels to bytes (clamped to [0, 1]) for the float targets. */
  const half = (h) => {
    const e = (h >> 10) & 31, m = h & 1023, sgn = h & 0x8000 ? -1 : 1;
    if (e === 0) return sgn * m * 2 ** -24;
    if (e === 31) return m ? NaN : sgn * Infinity;
    return sgn * (1 + m / 1024) * 2 ** (e - 15);
  };
  const small = (bits, mbits) => { /* unsigned 5-bit exponent floats (RG11B10) */
    const e = bits >> mbits, m = bits & ((1 << mbits) - 1);
    if (e === 0) return (m / (1 << mbits)) * 2 ** -14;
    if (e === 31) return m ? NaN : Infinity;
    return (1 + m / (1 << mbits)) * 2 ** (e - 15);
  };
  const byte = (v) => (Number.isFinite(v) ? Math.max(0, Math.min(255, Math.round(v * 255))) : v > 0 ? 255 : 0);
  const TEXEL_BYTES = { "rgba8unorm": 4, "rgba8unorm-srgb": 4, "bgra8unorm": 4, "rgb10a2unorm": 4, "rgba16float": 8, "rg11b10ufloat": 4, "r16float": 2 };
  /* Every RGBA8 and float render target, as it is now. */
  const dumpAll = (index) => {
    for (const [id, t] of ex.textures) {
      const isDepth = t.format === "depth32float" || t.format === "depth32float-stencil8";
      const texel = isDepth ? 4 : TEXEL_BYTES[t.format];
      if (!t.renderView || !texel) continue;
      const bytesPerRow = Math.ceil((t.width * texel) / 256) * 256;
      const buffer = device.createBuffer({ size: bytesPerRow * t.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
      const enc = device.createCommandEncoder();
      enc.copyTextureToBuffer({ texture: t.texture, aspect: isDepth ? "depth-only" : "all" }, { buffer, bytesPerRow }, [t.width, t.height]);
      device.queue.submit([enc.finish()]);
      pending.push(buffer.mapAsync(GPUMapMode.READ).then(() => {
        const src = new Uint8Array(buffer.getMappedRange());
        const out = new Uint8Array(t.width * t.height * 4);
        if (isDepth) { /* depth as grey: 1 - depth, stretched (near is bright) */
          const dv = new DataView(src.buffer, src.byteOffset, src.byteLength);
          for (let y = 0; y < t.height; y++)
            for (let x = 0; x < t.width; x++) {
              const d = dv.getFloat32(y * bytesPerRow + x * 4, true), o = (y * t.width + x) * 4;
              const v = byte(Math.min(1, (1 - d) * 20));
              out[o] = out[o + 1] = out[o + 2] = v; out[o + 3] = 255;
            }
        } else if (texel === 4 && t.format !== "rg11b10ufloat") {
          for (let y = 0; y < t.height; y++) out.set(src.subarray(y * bytesPerRow, y * bytesPerRow + t.width * 4), y * t.width * 4);
        } else {
          const dv = new DataView(src.buffer, src.byteOffset, src.byteLength);
          for (let y = 0; y < t.height; y++)
            for (let x = 0; x < t.width; x++) {
              const at = y * bytesPerRow + x * texel, o = (y * t.width + x) * 4;
              let c;
              if (t.format === "rgba16float") c = [0, 2, 4, 6].map((k) => half(dv.getUint16(at + k, true)));
              else if (t.format === "r16float") { const v = half(dv.getUint16(at, true)); c = [v, v, v, 1]; }
              else { const w = dv.getUint32(at, true); c = [small(w & 2047, 6), small((w >> 11) & 2047, 6), small(w >>> 22, 5), 1]; }
              for (let k = 0; k < 4; k++) out[o + k] = byte(c[k]);
            }
        }
        buffer.destroy();
        return window.saveTarget(`p${index}-tex${id}-${t.width}x${t.height}.pam`, t.width, t.height, toBase64(out));
      }));
    }
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
      if ((every && presents % every === 0) || wanted.has(presents)) readback(presents, false);
      if (targetsAt.has(presents)) dumpAll(presents);
      if (targetsAt.has(presents) && globalThis.TEX_INFO)
        for (const id of globalThis.TEX_INFO) {
          const t = ex.textures.get(id);
          console.log(`texture ${id}: ${t ? `${t.format} ${t.width}x${t.height}x${t.texture.depthOrArrayLayers} levels ${t.texture.mipLevelCount} ${t.texture.dimension}${t.renderView ? " render" : ""}` : "missing"}`);
        }
    },
    log(level, message) { console.log(`${level}: ${message}`); },
  };
  const ex = new window.GpuExecutor(device, host);
  /* Records never exceed half the producer's ring; a chunk buffer twice
   * the largest record plus the read size always holds the next one. */
  const reader = (await fetch("/stream")).body.getReader();
  let buf = new Uint8Array(64 << 20);
  let have = 0, eof = false;
  const t0 = performance.now();
  let records = 0;
  const byType = {};
  for (;;) {
    let pos = 0;
    while (pos + 8 <= have) {
      const view = new DataView(buf.buffer, pos, have - pos);
      const type = view.getUint32(0, true), size = view.getUint32(4, true);
      if (pos + 8 + size > have) break;
      if (globalThis.TRACE_RECORDS) console.log(`record ${records} type ${type} size ${size}`);
      const payload = buf.subarray(pos + 8, pos + 8 + size);
      const r0 = performance.now();
      if (globalThis.MEASURE && type === 6 && records > 8000 && records < 12000) {
        ex.flush(); await device.queue.onSubmittedWorkDone();
        const m0 = performance.now();
        ex.execute(type, payload);
        ex.flush(); await device.queue.onSubmittedWorkDone();
        const shader = new DataView(payload.buffer, payload.byteOffset, 4).getUint32(0, true);
        const m = (globalThis.MEASURED ??= {});
        const e = (m[shader] ??= { n: 0, ms: 0 }); e.n++; e.ms += performance.now() - m0;
      } else if (!(globalThis.SKIP_TYPES ?? []).includes(type)) ex.execute(type, payload);
      if (globalThis.SYNC_DRAWS && type === 6) { ex.flush(); await device.queue.onSubmittedWorkDone(); }
      if (globalThis.TRACE_SHAPES && type === 6 && presents + 1 === globalThis.TRACE_SHAPES) {
        /* Each draw's shader, colour targets and depth target (Gpu_Rec_Draw: targets of 36 bytes at 8, depth at 296). */
        const dv = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
        const n = dv.getUint32(4, true), ids = [];
        for (let k = 0; k < n && k < 8; k++) {
          const id = dv.getUint32(8 + 36 * k, true), t = ex.textures.get(id);
          ids.push(id && globalThis.TRACE_SIZES ? `${id}(${t ? `${t.format} ${t.width}x${t.height}` : "missing"})` : id);
        }
        if (globalThis.TRACE_SIZES) { const t = ex.textures.get(dv.getUint32(296, true)); ids.push(`depth(${t ? `${t.format} ${t.width}x${t.height}` : "-"})`); }
        /* The sampled textures: Gpu_Rec_Binding {kind, binding, bytes, id} from byte 432 (binding_count at 400). */
        const tex = [];
        for (let k = 0, at = 432; k < dv.getUint32(400, true) && at + 16 <= payload.byteLength; k++) {
          const kind = dv.getUint32(at, true);
          if (kind === 2) tex.push(dv.getUint32(at + 12, true));
          if (kind === 1 && globalThis.CB_DUMP && (globalThis.CB_DUMP.shader === dv.getUint32(0, true) || globalThis.CB_DUMP.shader === dv.getUint32(408, true))) {
            /* The draw's data words D: constant buffer s at D[D[248 + 2s]], D[249 + 2s] words. */
            const D = new DataView(payload.buffer, payload.byteOffset + at + 16, dv.getUint32(at + 8, true));
            const w = (i) => D.getUint32(4 * i, true), f = (i) => D.getFloat32(4 * i, true);
            const s = globalThis.CB_DUMP.slot, base = w(248 + 2 * s), size = w(249 + 2 * s);
            const vals = [];
            for (let i = 0; i < Math.min(size, globalThis.CB_DUMP.words); i++) vals.push(f(base + i).toPrecision(5));
            console.log(`cb${s} record ${records} (${size} words): ${vals.join(" ")}`);
            const hdr = [];
            for (let i = 284; i < 360; i++) hdr.push(w(i).toString(16));
            console.log(`D[284..360] record ${records}: ${hdr.join(" ")}`);
            /* The first input's (position's) values for the first indices, through cb3's view (0..11) and projection (28..43). */
            const idxCount = dv.getUint32(424, true), idxAt = payload.byteLength - 4 * idxCount;
            const posOff = w(294), stride = w(295) & 0xfff, idLo = w(297);
            const cb = (i) => f(base + i);
            const out = [];
            for (let k = 0; k < 6 && k < idxCount; k++) {
              const id = dv.getUint32(idxAt + 4 * k, true);
              const at = posOff + (id - idLo) * stride;
              if (at + 12 > D.byteLength) { out.push(`id ${id} out of range`); continue; }
              const x = D.getFloat32(at, true), y = D.getFloat32(at + 4, true), z = D.getFloat32(at + 8, true);
              const v = [0, 1, 2].map((r) => cb(4 * r) * x + cb(4 * r + 1) * y + cb(4 * r + 2) * z + cb(4 * r + 3));
              const c = [0, 1, 2, 3].map((r) => cb(28 + 4 * r) * v[0] + cb(29 + 4 * r) * v[1] + cb(30 + 4 * r) * v[2] + cb(31 + 4 * r));
              out.push(`id ${id} pos ${[x, y, z].map((q) => q.toFixed(1))} ndc ${[c[0] / c[3], c[1] / c[3], c[2] / c[3]].map((q) => q.toFixed(2))} w ${c[3].toFixed(1)}`);
            }
            console.log(out.join(" | "));
          }
          at += 16 + (kind === 1 ? dv.getUint32(at + 8, true) : 0);
        }
        console.log(`shape ${records} shader ${dv.getUint32(0, true)} vs ${dv.getUint32(8 + 36 * 8 + 4 * 13 + 4 * 4 + 4 * 4 + 4 * 4 + 4 * 4, true)} targets ${ids.join(",")} depth ${dv.getUint32(296, true)} test ${dv.getUint32(300, true)} write ${dv.getUint32(304, true)} cmp ${dv.getUint32(308, true)} tex ${tex.join(",")} vtx ${dv.getUint32(404, true)} vsid ${dv.getUint32(408, true)} inputs ${dv.getUint32(412, true)} cull ${dv.getUint32(416, true)} idx ${dv.getUint32(424, true)} flags ${dv.getUint32(428, true).toString(16)} scissor ${[0, 1, 2, 3].map((k) => dv.getInt32(376 + 4 * k, true)).join(",")}`);
      }
      if (globalThis.DRAW_TRACE && type === 6 && presents + 1 === globalThis.DRAW_TRACE) {
        /* Diagnostics: each RGBA8 target's channel sums after every draw of one present
         * (TRACE_TARGET=id: that one target, float formats too, with the draw's targets). */
        ex.flush();
        const sums = [];
        const only = globalThis.TRACE_TARGET;
        if (only) {
          const t = ex.textures.get(only);
          const isDepth = t && (t.format === "depth32float" || t.format === "depth32float-stencil8");
          const texel = t ? (isDepth ? 4 : TEXEL_BYTES[t.format]) : 0;
          if (t && texel) {
            const bytesPerRow = Math.ceil((t.width * texel) / 256) * 256;
            const buffer = device.createBuffer({ size: bytesPerRow * t.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
            const enc = device.createCommandEncoder();
            enc.copyTextureToBuffer({ texture: t.texture, aspect: isDepth ? "depth-only" : "all" }, { buffer, bytesPerRow }, [t.width, t.height]);
            device.queue.submit([enc.finish()]);
            await buffer.mapAsync(GPUMapMode.READ);
            const dv2 = new DataView(buffer.getMappedRange());
            let sum = 0;
            if (isDepth) for (let y = 0; y < t.height; y += 4) for (let x = 0; x < t.width; x += 4) sum += 1 - dv2.getFloat32(y * bytesPerRow + x * 4, true);
            for (let y = 0; y < t.height; y += 4)
              for (let x = 0; x < t.width; x += 4) {
                const at = y * bytesPerRow + x * texel;
                if (t.format === "rgba16float") sum += half(dv2.getUint16(at, true)) + half(dv2.getUint16(at + 2, true)) + half(dv2.getUint16(at + 4, true));
                else if (t.format === "rg11b10ufloat") { const w = dv2.getUint32(at, true); sum += small(w & 2047, 6) + small((w >> 11) & 2047, 6) + small(w >>> 22, 5); }
                else if (!isDepth) sum += dv2.getUint8(at) + dv2.getUint8(at + 1) + dv2.getUint8(at + 2);
              }
            buffer.destroy();
            const dv = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
            console.log(`draw ${records} shader ${dv.getUint32(0, true)} target ${only} ${t.format} sum ${sum.toFixed(2)}`);
          }
        }
        for (const [id, t] of only ? [] : ex.textures) {
          if (!t.renderView || !["rgba8unorm", "rgba8unorm-srgb", "bgra8unorm"].includes(t.format)) continue;
          const bytesPerRow = Math.ceil((t.width * 4) / 256) * 256;
          const buffer = device.createBuffer({ size: bytesPerRow * t.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
          const enc = device.createCommandEncoder();
          enc.copyTextureToBuffer({ texture: t.texture }, { buffer, bytesPerRow }, [t.width, t.height]);
          device.queue.submit([enc.finish()]);
          await buffer.mapAsync(GPUMapMode.READ);
          const src = new Uint8Array(buffer.getMappedRange());
          let sum = 0;
          for (let i = 0; i < src.length; i += 4) sum += src[i] + 3 * src[i + 1] + 7 * src[i + 2];
          buffer.destroy();
          sums.push(`${id}:${t.width}x${t.height}:${sum}`);
        }
        if (!only) {
          const dv = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
          console.log(`draw ${records} shader ${dv.getUint32(0, true)} ${sums.join(" ")}`);
        }
      }
      const e = (byType[type] ??= { n: 0, ms: 0, bytes: 0 });
      e.n++; e.ms += performance.now() - r0; e.bytes += size;
      pos += 8 + size;
      records++;
      /* Keep the GPU from queueing unbounded work (and frames from piling up). */
      if (type === 8 && presents % 4 === 0) { await device.queue.onSubmittedWorkDone(); await Promise.all(pending.splice(0)); }
    }
    if (eof) break;
    /* The executor copies what it keeps (staging arrays, writeBuffer/
     * writeTexture), so the consumed prefix can be overwritten. */
    buf.copyWithin(0, pos, have);
    have -= pos;
    const { value, done } = await reader.read();
    if (done) { eof = true; continue; }
    if (have + value.byteLength > buf.byteLength) {
      const grown = new Uint8Array(Math.max(buf.byteLength * 2, have + value.byteLength));
      grown.set(buf.subarray(0, have));
      buf = grown;
    }
    buf.set(value, have);
    have += value.byteLength;
  }
  const cpuMs = performance.now() - t0;
  if (globalThis.MEASURED) console.log(`per-shader draw ms: ${JSON.stringify(Object.fromEntries(Object.entries(globalThis.MEASURED).map(([k, v]) => [k, { n: v.n, avg: +(v.ms / v.n).toFixed(3) }])))}`);
  ex.flush();
  await device.queue.onSubmittedWorkDone();
  const elapsed = performance.now() - t0;
  console.log(`prof ${JSON.stringify(ex.prof)}`); console.log(`CPU side ${cpuMs.toFixed(0)} ms; by record type ${JSON.stringify(Object.fromEntries(Object.entries(byType).map(([k, v]) => [k, { n: v.n, ms: Math.round(v.ms), MB: +(v.bytes / 1048576).toFixed(1) }])))}`);
  if (target) readback(presents, true);
  await Promise.all(pending);
  return { records, presents, elapsed, stats: ex.stats, errors, features };
}, { every, at, dumpTargets });
await browser.close();
server.close();

if (lastFrame) writeFrame(`${prefix}.last.ppm`, lastFrame.w, lastFrame.h, lastFrame.b64);
console.log(`replayed ${result.records} records, ${result.presents} presents in ${result.elapsed.toFixed(0)} ms (${adapter}; features ${result.features.join(",") || "none"})`);
console.log(`executor: ${JSON.stringify(result.stats)}`);
for (const e of result.errors) console.log(`WebGPU error: ${e}`);
