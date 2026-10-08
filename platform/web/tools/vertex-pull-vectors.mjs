/**
 * Differential test of the vertex-pulling decoder (core/gpu/wgsl.c vfetch)
 * against raster3d's CPU decoder: tests/wgsl_test.c writes DIR/vertex_pull.json
 * (a data buffer of attribute descriptors and element bytes, and per case
 * the descriptor, the vertex id, the expected four words and a float
 * tolerance in ULPs) and DIR/vertex_pull.wgsl (the decoder); this runs the
 * decoder in a compute shader on WebGPU and compares.
 *
 *   build/native-noop/tests/wgsl_test DIR && node tools/vertex-pull-vectors.mjs DIR
 *   (WGSL_VECTORS_GPU=metal runs on the host GPU instead of SwiftShader)
 */
import { chromium } from "@playwright/test";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const dir = process.argv[2];
if (!dir) {
  console.error("usage: node tools/vertex-pull-vectors.mjs <dir>");
  process.exit(2);
}
const vectors = JSON.parse(readFileSync(join(dir, "vertex_pull.json"), "utf8"));
const decoder = readFileSync(join(dir, "vertex_pull.wgsl"), "utf8");

const browser = await chromium.launch({
  channel: "chromium",
  headless: true,
  args: process.env.WGSL_VECTORS_GPU === "metal"
    ? ["--enable-unsafe-webgpu", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"]
    : ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader", "--enable-features=Vulkan"],
});
try {
  const page = await browser.newPage();
  await page.goto("file:///"); /* a secure context: WebGPU */
  const got = await page.evaluate(async ({ decoder, words, cases }) => {
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) throw new Error("no WebGPU adapter");
    const device = await adapter.requestDevice();
    const code = `@group(0) @binding(0) var<storage, read> D: array<u32>;
@group(0) @binding(1) var<storage, read> C: array<vec2<u32>>;
@group(0) @binding(2) var<storage, read_write> O: array<vec4<u32>>;
${decoder}
@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  O[g.x] = vfetch(C[g.x].x, C[g.x].y);
}`;
    const module = device.createShaderModule({ code });
    const info = await module.getCompilationInfo();
    const errors = info.messages.filter((m) => m.type === "error");
    if (errors.length) throw new Error(errors.map((m) => `${m.lineNum}: ${m.message}`).join("\n"));
    const buffer = (data, usage) => {
      const b = device.createBuffer({ size: Math.max(16, data.byteLength), usage, mappedAtCreation: true });
      new Uint8Array(b.getMappedRange()).set(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
      b.unmap();
      return b;
    };
    const d = buffer(new Uint32Array(words), GPUBufferUsage.STORAGE);
    const c = buffer(new Uint32Array(cases.flatMap((k) => [k[0], k[1]])), GPUBufferUsage.STORAGE);
    const outBytes = 16 * cases.length;
    const o = device.createBuffer({ size: outBytes, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
    const read = device.createBuffer({ size: outBytes, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    const pipeline = device.createComputePipeline({ layout: "auto", compute: { module, entryPoint: "main" } });
    const group = device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [{ binding: 0, resource: { buffer: d } }, { binding: 1, resource: { buffer: c } },
                { binding: 2, resource: { buffer: o } }],
    });
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginComputePass();
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, group);
    pass.dispatchWorkgroups(cases.length);
    pass.end();
    encoder.copyBufferToBuffer(o, 0, read, 0, outBytes);
    device.queue.submit([encoder.finish()]);
    await read.mapAsync(GPUMapMode.READ);
    return Array.from(new Uint32Array(read.getMappedRange().slice(0)));
  }, { decoder, words: vectors.words, cases: vectors.cases });

  const f32 = new Float32Array(1);
  const u32 = new Uint32Array(f32.buffer);
  const ulpDistance = (a, b) => {
    u32[0] = a;
    const fa = f32[0];
    u32[0] = b;
    const fb = f32[0];
    if (Number.isNaN(fa) || Number.isNaN(fb)) return a === b ? 0 : Infinity;
    const ordered = (bits) => (bits & 0x80000000 ? 0x80000000 - (bits & 0x7fffffff) : 0x80000000 + bits);
    return Math.abs(ordered(a >>> 0) - ordered(b >>> 0));
  };
  let failures = 0;
  vectors.cases.forEach((k, i) => {
    const want = k.slice(2, 6).map((x) => x >>> 0);
    const have = got.slice(4 * i, 4 * i + 4).map((x) => x >>> 0);
    const ulps = k[6];
    const ok = want.every((w, j) => w === have[j] || (ulps > 0 && ulpDistance(w, have[j]) <= ulps));
    if (!ok) {
      failures++;
      if (failures <= 20) {
        const attrib = vectors.words[k[0] + 2] >>> 0;
        console.log(`FAIL case ${i} (size 0x${((attrib >>> 21) & 0x3f).toString(16)} type ${(attrib >>> 27) & 7}` +
                    ` swap ${attrib >>> 31}): expected [${want}] got [${have}]`);
      }
    }
  });
  console.log(`${vectors.cases.length - failures}/${vectors.cases.length} vertex-pull cases match raster3d's decoder`);
  process.exitCode = failures ? 1 : 0;
} finally {
  await browser.close();
}
