/**
 * Differential test of the vertex-program translation (core/gpu/wgsl.c)
 * on a title's own programs: voland-cli with VOLAND_VS_CAPTURE=DIR writes
 * DIR/vs-<hash>.json per vertex program (its WGSL, the constant buffers
 * it read, and per vertex the inputs and the interpreter's outputs); this
 * runs each WGSL vertex body in a compute shader on WebGPU and compares
 * the position and generic outputs.
 *
 *   VOLAND_VS_CAPTURE=DIR voland-cli run GAME ... && node tools/vertex-vectors.mjs DIR [hash...]
 *   (WGSL_VECTORS_GPU=metal runs on the host GPU instead of SwiftShader)
 *
 * Floats compare with a relative tolerance (MUFU and friends differ by a
 * few ULPs); a program is reported when any output word is further off.
 */
import { chromium } from "@playwright/test";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

const dir = process.argv[2];
if (!dir) {
  console.error("usage: node tools/vertex-vectors.mjs <dir> [hash...]");
  process.exit(2);
}
const only = new Set(process.argv.slice(3));
const files = readdirSync(dir).filter((f) => /^vs-[0-9a-f]{8}\.json$/.test(f)).sort();
const vectors = files.map((f) => JSON.parse(readFileSync(join(dir, f), "utf8")))
  .filter((v) => only.size === 0 || only.has(v.hash));

const ATTRIBUTE_WORDS = 256;
const TOLERANCE = 1e-3; /* relative, on floats (fused vs separate multiply-adds) */
const ABSOLUTE = 1e-5; /* near zero, where cancellation leaves no relative precision */
const POOL_BINDING = (() => {
  const m = vectors.find((v) => !v.skip)?.wgsl.match(/@binding\((\d+)\) var<storage, read> P:/);
  return m ? Number(m[1]) : -1;
})();

/* The vertex entry becomes a plain function; the attribute outputs are
 * copied to a private array before the viewport epilogue, and a compute
 * entry runs it once per captured vertex. */
function harness(v) {
  const entry = "@vertex fn vs(vin: VIn) -> VOut {";
  const epilogue = "  var o: VOut;\n  let vp_clip";
  if (!v.wgsl.includes(entry) || !v.wgsl.includes(epilogue)) throw new Error("unexpected vertex WGSL shape");
  const inputs = Array.from({ length: v.inputs }, (_, i) =>
    `vec4<u32>(I[b + ${2 + 4 * i}u], I[b + ${3 + 4 * i}u], I[b + ${4 + 4 * i}u], I[b + ${5 + 4 * i}u])`);
  const stride = 2 + 4 * v.inputs;
  return v.wgsl.replace(entry, "fn vs(vin: VIn) -> VOut {")
    .replace(epilogue, `  for (var i = 0u; i < ${ATTRIBUTE_WORDS}u; i++) { g_aout[i] = aout[i]; }\n${epilogue}`) + `
var<private> g_aout: array<u32, ${ATTRIBUTE_WORDS}>;
@group(1) @binding(0) var<storage, read> I: array<u32>;
@group(1) @binding(1) var<storage, read_write> O: array<u32>;
@compute @workgroup_size(1) fn harness_main(@builtin(global_invocation_id) g: vec3<u32>) {
  let b = g.x * ${stride}u;
  _ = vs(VIn(vec4<u32>(I[b], I[b + 1u], 0u, 0u)${inputs.map((x) => `, ${x}`).join("")}));
  for (var i = 0u; i < ${ATTRIBUTE_WORDS}u; i++) { O[g.x * ${ATTRIBUTE_WORDS}u + i] = g_aout[i]; }
}
`;
}

const browser = await chromium.launch({
  channel: "chromium",
  headless: true,
  args: process.env.WGSL_VECTORS_GPU === "metal"
    ? ["--enable-unsafe-webgpu", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"]
    : ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader", "--enable-features=Vulkan"],
});
let failed = 0, passed = 0, skipped = 0;
try {
  const page = await browser.newPage();
  await page.goto("file:///"); /* a secure context: WebGPU */
  for (const v of vectors) {
    if (v.skip) {
      skipped++;
      continue;
    }
    const d = new Array(v.d_words).fill(0);
    v.table.forEach((w, i) => { d[v.cbuf_table + i] = w; });
    const inWords = v.lanes.flatMap((l) => [l.ids[0], l.ids[1], ...l.in]);
    const got = await page.evaluate(async ({ code, d, pool, inWords, lanes, poolBinding, attributeWords }) => {
      const adapter = await navigator.gpu.requestAdapter();
      if (!adapter) throw new Error("no WebGPU adapter");
      const device = await adapter.requestDevice({
        requiredLimits: { maxStorageBuffersPerShaderStage: adapter.limits.maxStorageBuffersPerShaderStage },
      });
      const module = device.createShaderModule({ code });
      const info = await module.getCompilationInfo();
      const errors = info.messages.filter((m) => m.type === "error");
      if (errors.length) return { error: errors.map((m) => `${m.lineNum}: ${m.message}`).join("\n") };
      const buffer = (words) => {
        const data = new Uint32Array(Math.max(4, words.length));
        data.set(words);
        const b = device.createBuffer({ size: data.byteLength, usage: GPUBufferUsage.STORAGE, mappedAtCreation: true });
        new Uint32Array(b.getMappedRange()).set(data);
        b.unmap();
        return b;
      };
      const outBytes = 4 * attributeWords * lanes;
      const o = device.createBuffer({ size: outBytes, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
      const read = device.createBuffer({ size: outBytes, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
      device.pushErrorScope("validation");
      const pipeline = device.createComputePipeline({ layout: "auto", compute: { module, entryPoint: "harness_main" } });
      const g0 = [{ binding: 0, resource: { buffer: buffer(d) } }];
      if (/let cbB\d+ = /.test(code)) g0.push({ binding: poolBinding, resource: { buffer: buffer(pool) } });
      const group0 = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: g0 });
      const group1 = device.createBindGroup({
        layout: pipeline.getBindGroupLayout(1),
        entries: [{ binding: 0, resource: { buffer: buffer(inWords) } }, { binding: 1, resource: { buffer: o } }],
      });
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginComputePass();
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, group0);
      pass.setBindGroup(1, group1);
      pass.dispatchWorkgroups(lanes);
      pass.end();
      encoder.copyBufferToBuffer(o, 0, read, 0, outBytes);
      device.queue.submit([encoder.finish()]);
      const validation = await device.popErrorScope();
      if (validation) return { error: validation.message };
      await read.mapAsync(GPUMapMode.READ);
      const words = Array.from(new Uint32Array(read.getMappedRange()));
      device.destroy();
      return { words };
    }, { code: harness(v), d, pool: v.pool, inWords, lanes: v.lanes.length, poolBinding: POOL_BINDING,
         attributeWords: ATTRIBUTE_WORDS });
    if (got.error) {
      failed++;
      console.log(`vs-${v.hash} (${v.address}): WGSL error: ${got.error.split("\n")[0]}`);
      continue;
    }
    const f32 = (w) => new Float32Array(new Uint32Array([w]).buffer)[0];
    const bad = [];
    v.lanes.forEach((lane, l) => {
      v.words.forEach((word, i) => {
        const want = lane.out[i], have = got.words[l * ATTRIBUTE_WORDS + word];
        if (want === have) return;
        const a = f32(want), b = f32(have);
        if (Number.isNaN(a) && Number.isNaN(b)) return;
        if (Math.abs(a - b) <= Math.max(ABSOLUTE, TOLERANCE * Math.max(Math.abs(a), Math.abs(b)))) return;
        bad.push({ lane: l, word, want: a, have: b });
      });
    });
    if (bad.length === 0) {
      passed++;
      continue;
    }
    failed++;
    const words = [...new Set(bad.map((x) => x.word))];
    console.log(`vs-${v.hash} (${v.address}): ${bad.length} words differ over ${v.lanes.length} vertices; attribute words ${words.map((w) => `0x${(w * 4).toString(16)}`).join(" ")}`);
    for (const x of bad.slice(0, 4)) console.log(`  vertex ${x.lane} a[0x${(x.word * 4).toString(16)}]: interpreter ${x.want} wgsl ${x.have}`);
  }
} finally {
  await browser.close();
}
console.log(`[vertex-vectors] ${passed} passed, ${failed} failed, ${skipped} skipped (textures / globals / untranslated)`);
process.exit(failed ? 1 : 0);
