/**
 * Differential test of translated compute programs (core/gpu/wgsl.c,
 * SM_STAGE_COMPUTE) against the interpreter: tests/wgsl_compute_test.c
 * writes DIR/<name>.wgsl and DIR/<name>.json (the data buffer, each
 * global-memory window before the dispatch, and the windows the
 * interpreter left); this dispatches each on WebGPU and compares every
 * window word - exactly, or within `ulps` for words that are floats on
 * both sides (or zeros of either sign).
 *
 *   build/native-noop/tests/wgsl_compute_test DIR && node tools/compute-vectors.mjs DIR
 *   (WGSL_VECTORS_GPU=metal runs on the host GPU instead of SwiftShader)
 */
import { chromium } from "@playwright/test";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

const dir = process.argv[2];
if (!dir) {
  console.error("usage: node tools/compute-vectors.mjs <dir>");
  process.exit(2);
}
const vectors = readdirSync(dir).filter((f) => f.endsWith(".json") && f.startsWith("cs_")).sort().map((f) => {
  const v = JSON.parse(readFileSync(join(dir, f), "utf8"));
  return { ...v, code: readFileSync(join(dir, f.replace(/\.json$/, ".wgsl")), "utf8") };
});

const browser = await chromium.launch({
  channel: "chromium",
  headless: true,
  args: process.env.WGSL_VECTORS_GPU === "metal"
    ? ["--enable-unsafe-webgpu", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"]
    : ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader", "--enable-features=Vulkan"],
});
let failures = 0;
try {
  const page = await browser.newPage();
  await page.goto("file:///"); /* a secure context: WebGPU */
  for (const v of vectors) {
    const got = await page.evaluate(async (v) => {
      const adapter = await navigator.gpu.requestAdapter();
      if (!adapter) throw new Error("no WebGPU adapter");
      const device = await adapter.requestDevice();
      device.pushErrorScope("validation");
      const module = device.createShaderModule({ code: v.code });
      const info = await module.getCompilationInfo();
      const errors = info.messages.filter((m) => m.type === "error");
      if (errors.length) return { error: errors.map((m) => `${m.lineNum}: ${m.message}`).join("\n") };
      const pipeline = device.createComputePipeline({ layout: "auto", compute: { module, entryPoint: "cs" } });
      const make = (words, usage) => {
        const data = new Uint32Array(words);
        const b = device.createBuffer({ size: Math.max(16, data.byteLength), usage, mappedAtCreation: true });
        new Uint32Array(b.getMappedRange()).set(data);
        b.unmap();
        return b;
      };
      const d = make(v.data, GPUBufferUsage.STORAGE);
      const windows = v.windows.map((w) => make(w, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC));
      const entries = [{ binding: 0, resource: { buffer: d } }];
      /* Only the bindings the module uses are in its auto layout. */
      windows.forEach((b, k) => {
        if (v.code.includes(`var<storage, read_write> G${k}:`)) entries.push({ binding: v.window_binding + k, resource: { buffer: b } });
      });
      const group = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries });
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginComputePass();
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, group);
      pass.dispatchWorkgroups(v.grid[0], v.grid[1], v.grid[2]);
      pass.end();
      const reads = windows.map((b) => {
        const r = device.createBuffer({ size: b.size, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
        encoder.copyBufferToBuffer(b, 0, r, 0, b.size);
        return r;
      });
      device.queue.submit([encoder.finish()]);
      const scoped = await device.popErrorScope();
      if (scoped) return { error: scoped.message };
      const out = [];
      for (const r of reads) {
        await r.mapAsync(GPUMapMode.READ);
        out.push(Array.from(new Uint32Array(r.getMappedRange().slice(0))));
      }
      return { out };
    }, v);
    if (got.error) {
      failures++;
      console.log(`FAIL ${v.name}: ${got.error}`);
      continue;
    }
    const f32 = new Float32Array(1);
    const u32 = new Uint32Array(f32.buffer);
    const ordered = (bits) => (bits & 0x80000000 ? 0x80000000 - (bits & 0x7fffffff) : 0x80000000 + bits);
    const isFloat = (bits) => {
      u32[0] = bits;
      return Number.isFinite(f32[0]) && (bits & 0x7f800000) !== 0;
    };
    let wrong = 0, near = 0;
    v.expected.forEach((want, k) => {
      want.forEach((w, i) => {
        const h = got.out[k][i] >>> 0;
        const e = w >>> 0;
        if (h === e) return;
        /* WGSL need not keep the sign of a zero. */
        if (v.ulps > 0 && (h | e | 0x80000000) >>> 0 === 0x80000000) {
          near++;
          return;
        }
        if (v.ulps > 0 && isFloat(h) && isFloat(e) && Math.abs(ordered(h) - ordered(e)) <= v.ulps) {
          near++;
          return;
        }
        if (wrong < 6) console.log(`  ${v.name}: window ${k} word ${i}: expected ${e.toString(16)} got ${h.toString(16)}`);
        wrong++;
      });
    });
    const written = v.expected.reduce((n, w, k) => n + w.filter((x, i) => x !== v.windows[k][i]).length, 0);
    console.log(`${wrong ? "FAIL" : "ok  "} ${v.name}: ${written} words written by the interpreter, ${wrong} wrong, ${near} within ${v.ulps} ULPs`);
    if (wrong) failures++;
  }
} finally {
  await browser.close();
}
console.log(`${vectors.length - failures}/${vectors.length} compute vectors match the interpreter`);
process.exitCode = failures ? 1 : 0;
