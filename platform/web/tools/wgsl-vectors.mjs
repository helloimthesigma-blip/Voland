/**
 * Differential test of the Maxwell -> WGSL translator: renders each vector
 * that tests/wgsl_test.c wrote (DIR/<name>.wgsl + .json) into a 1x1
 * rgba32uint target on WebGPU (headless Chromium, SwiftShader) and compares
 * the four output registers with the interpreter's.
 *
 *   build/native-noop/tests/wgsl_test DIR && node tools/wgsl-vectors.mjs DIR
 */
import { chromium } from "@playwright/test";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

/* core/gpu/wgsl.h */
const DRAW_TEXTURE_PARAMS = 4;
const TEX_PARAM_WORDS = 12;
const MAX_TEXTURES = 16;
const CBUF_TABLE = DRAW_TEXTURE_PARAMS + TEX_PARAM_WORDS * MAX_TEXTURES;
const CONSTANT_WORDS = CBUF_TABLE + 2 * 18;
const TEXP_SCALE = 1;
const TEXP_LINEAR = 2;
const TEXP_MIN_LINEAR = 16;
const TEXP_LOD_BIAS = 5; /* f32 x 3: bias, min LOD, max LOD */
const KILLED_MARK = 0xdeadbeef;

const dir = process.argv[2];
if (!dir) {
  console.error("usage: node tools/wgsl-vectors.mjs <dir>");
  process.exit(2);
}
const vectors = readdirSync(dir)
  .filter((f) => f.endsWith(".json"))
  .sort()
  .map((f) => {
    const v = JSON.parse(readFileSync(join(dir, f), "utf8"));
    return { ...v, code: readFileSync(join(dir, `${v.name}.wgsl`), "utf8") };
  });

const browser = await chromium.launch({
  args: ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader", "--enable-features=Vulkan"],
});
const page = await browser.newPage();
await page.goto("file:///");
const results = await page.evaluate(
  async ({ vectors, k }) => {
    const adapter = await navigator.gpu.requestAdapter();
    const device = await adapter.requestDevice();
    const out = [];
    for (const v of vectors) {
      device.pushErrorScope("validation");
      const data = new Uint32Array(k.CONSTANT_WORDS + v.cbuf.length);
      data[0] = new Uint32Array(new Float32Array([1]).buffer)[0];
      const tp = k.DRAW_TEXTURE_PARAMS;
      data[tp + 0] = k.TEXP_SCALE | (v.texture.linear ? k.TEXP_LINEAR : 0);
      data[tp + 1] = v.texture.wrap;
      data[tp + 2] = 0x5432;
      data[tp + 3] = v.texture.levels ?? 1;
      if (v.texture.lod) {
        data[tp + 0] |= k.TEXP_MIN_LINEAR;
        new Float32Array(data.buffer, (tp + k.TEXP_LOD_BIAS) * 4, 3).set(v.texture.lod);
      }
      data[k.CBUF_TABLE + 2 * v.cbuf_slot] = k.CONSTANT_WORDS;
      data[k.CBUF_TABLE + 2 * v.cbuf_slot + 1] = v.cbuf.length;
      data.set(v.cbuf, k.CONSTANT_WORDS);
      const buffer = device.createBuffer({ size: data.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
      device.queue.writeBuffer(buffer, 0, data);
      const tex = device.createTexture({
        size: [v.texture.width, v.texture.height, 1],
        format: "rgba8unorm",
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
        mipLevelCount: v.texture.levels ?? 1,
      });
      if (v.texture.mip1) {
        device.queue.writeTexture({ texture: tex, mipLevel: 1 }, new Uint8Array(v.texture.mip1), { bytesPerRow: 4 }, [1, 1, 1]);
      }
      device.queue.writeTexture(
        { texture: tex },
        new Uint8Array(v.texture.rgba8),
        { bytesPerRow: v.texture.width * 4 },
        [v.texture.width, v.texture.height, 1],
      );
      const target = device.createTexture({
        size: [1, 1],
        format: "rgba32uint",
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC,
      });
      const nv = v.varyings.length;
      const stride = 16 + 16 * nv;
      const verts = new ArrayBuffer(stride * 3);
      const pos = [[-1, -1], [3, -1], [-1, 3]];
      for (let i = 0; i < 3; i++) {
        const f = new Float32Array(verts, i * stride, 4);
        f[0] = pos[i][0];
        f[1] = pos[i][1];
        f[2] = 0.25;
        f[3] = new Float32Array(new Uint32Array([v.inv_w]).buffer)[0];
        for (let j = 0; j < nv; j++) new Uint32Array(verts, i * stride + 16 + 16 * j, 4).set(v.varyings[j]);
      }
      const vbuf = device.createBuffer({ size: verts.byteLength, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
      device.queue.writeBuffer(vbuf, 0, verts);
      const module = device.createShaderModule({ code: v.code });
      const info = await module.getCompilationInfo();
      const errors = info.messages.filter((m) => m.type === "error").map((m) => `${m.lineNum}: ${m.message}`);
      if (errors.length) {
        await device.popErrorScope();
        out.push({ name: v.name, error: errors.join("; ") });
        continue;
      }
      const attributes = [{ shaderLocation: 0, offset: 0, format: "float32x4" }];
      for (let j = 0; j < nv; j++) attributes.push({ shaderLocation: j + 1, offset: 16 + 16 * j, format: "uint32x4" });
      const pipeline = device.createRenderPipeline({
        layout: "auto",
        vertex: { module, entryPoint: "vs", buffers: [{ arrayStride: stride, attributes }] },
        fragment: { module, entryPoint: "fs", targets: [{ format: "rgba32uint" }] },
        primitive: { topology: "triangle-list" },
      });
      const entries = [{ binding: 0, resource: { buffer } }];
      if (v.code.includes("var T0:")) entries.push({ binding: 1, resource: tex.createView({ dimension: "2d-array" }) });
      if (v.code.includes("var S0:")) {
        entries.push({ binding: 17, resource: device.createSampler({ magFilter: "linear", minFilter: "linear" }) });
      }
      const group = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries });
      const readback = device.createBuffer({ size: 256, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
      const enc = device.createCommandEncoder();
      const pass = enc.beginRenderPass({
        colorAttachments: [
          {
            view: target.createView(),
            loadOp: "clear",
            storeOp: "store",
            clearValue: [k.KILLED_MARK, k.KILLED_MARK, k.KILLED_MARK, k.KILLED_MARK],
          },
        ],
      });
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, group);
      pass.setVertexBuffer(0, vbuf);
      pass.draw(3);
      pass.end();
      enc.copyTextureToBuffer({ texture: target }, { buffer: readback, bytesPerRow: 256 }, [1, 1]);
      device.queue.submit([enc.finish()]);
      await readback.mapAsync(GPUMapMode.READ);
      const got = Array.from(new Uint32Array(readback.getMappedRange().slice(0, 16)));
      readback.unmap();
      const scoped = await device.popErrorScope();
      out.push({ name: v.name, got, error: scoped ? scoped.message : undefined });
    }
    return out;
  },
  { vectors, k: { CONSTANT_WORDS, CBUF_TABLE, DRAW_TEXTURE_PARAMS, TEXP_SCALE, TEXP_LINEAR, TEXP_MIN_LINEAR, TEXP_LOD_BIAS, KILLED_MARK } },
);
await browser.close();

const asFloat = (u) => new Float32Array(new Uint32Array([u]).buffer)[0];
let failures = 0;
for (const r of results) {
  const v = vectors.find((x) => x.name === r.name);
  let ok = !r.error;
  if (ok && v.expected === "killed") ok = r.got.every((x) => x === KILLED_MARK);
  else if (ok) {
    ok = v.expected.every((e, i) => {
      if (e === r.got[i]) return true;
      if (!v.float) return false;
      const a = asFloat(e);
      const b = asFloat(r.got[i]);
      return Math.abs(a - b) <= (v.name.endsWith("_hw") ? 4e-3 : 1e-5) * Math.max(1, Math.abs(a));
    });
  }
  if (!ok) failures++;
  console.log(
    `${ok ? "ok  " : "FAIL"} ${r.name}${r.error ? ` error: ${r.error}` : ""} expected ${JSON.stringify(v.expected)} got ${JSON.stringify(r.got)}`,
  );
}
console.log(`${results.length - failures}/${results.length} vectors match the interpreter`);
process.exit(failures === 0 ? 0 : 1);
