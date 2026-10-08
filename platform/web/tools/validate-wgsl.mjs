/**
 * Validates a directory of generated WGSL (core/gpu/wgsl.c, e.g. from
 * voland-cli's VOLAND_DUMP_WGSL) against a real WebGPU implementation:
 * headless Chromium with SwiftShader compiles every module and builds the
 * render pipeline the renderer would (vertex layout from VIn, one target
 * per FOut colour, depth32float when the shader writes depth).
 *
 *   node tools/validate-wgsl.mjs <dir> [--quiet]
 *
 * Exit status 0 when every module validates.
 */
import { chromium } from "@playwright/test";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

const dir = process.argv[2];
const quiet = process.argv.includes("--quiet");
if (!dir) {
  console.error("usage: node tools/validate-wgsl.mjs <dir> [--quiet]");
  process.exit(2);
}
const files = readdirSync(dir).filter((f) => f.endsWith(".wgsl")).sort();
const failed = readdirSync(dir).filter((f) => f.endsWith(".fail"));
const modules = files.map((f) => ({ name: f, code: readFileSync(join(dir, f), "utf8") }));

const browser = await chromium.launch({
  /* WGSL_VECTORS_GPU=metal: the host GPU (SwiftShader loses its device on some machines). */
  args: process.env.WGSL_VECTORS_GPU === "metal"
    ? ["--enable-unsafe-webgpu", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"]
    : ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader", "--enable-features=Vulkan"],
});
const page = await browser.newPage();
await page.goto("file:///");
const results = await page.evaluate(async (mods) => {
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) return [{ name: "(adapter)", errors: ["no WebGPU adapter"] }];
  const device = await adapter.requestDevice();
  const out = [];
  for (const m of mods) {
    const errors = [];
    device.pushErrorScope("validation");
    const module = device.createShaderModule({ code: m.code });
    const info = await module.getCompilationInfo();
    for (const msg of info.messages) {
      if (msg.type === "error") errors.push(`${msg.lineNum}:${msg.linePos} ${msg.message}`);
    }
    /* A compute program: one compute pipeline. */
    if (m.code.includes("@compute")) {
      if (errors.length === 0) {
        try {
          device.createComputePipeline({ layout: "auto", compute: { module, entryPoint: "cs" } });
        } catch (e) {
          errors.push(String(e));
        }
      }
      const scopedCs = await device.popErrorScope();
      if (scopedCs) errors.push(scopedCs.message.split("\n").slice(0, 6).join(" | "));
      out.push({ name: m.name, errors });
      continue;
    }
    /* A GPU vertex stage (no fragment entry): every VIn location is a
     * vec4<u32> input; the pipeline is built without a fragment stage. */
    const vertexOnly = !m.code.includes("@fragment");
    const inputs = (m.code.match(/struct VIn \{([\s\S]*?)\}/)?.[1].match(/@location/g) ?? []).length;
    if (vertexOnly && errors.length === 0) {
      try {
        const attrs = Array.from({ length: inputs }, (_, i) => ({ shaderLocation: i, offset: 16 * i, format: "uint32x4" }));
        device.createRenderPipeline({
          layout: "auto",
          vertex: { module, entryPoint: "vs", buffers: [{ arrayStride: 16 * inputs, attributes: attrs }] },
          primitive: { topology: "triangle-list" },
          depthStencil: { format: "depth32float", depthWriteEnabled: true, depthCompare: "always" },
        });
      } catch (e) {
        errors.push(String(e));
      }
      const scopedVs = await device.popErrorScope();
      if (scopedVs) errors.push(scopedVs.message.split("\n").slice(0, 6).join(" | "));
      out.push({ name: m.name, errors });
      continue;
    }
    const varyings = inputs - 1;
    const fout = m.code.match(/struct FOut \{([\s\S]*?)\}/)?.[1] ?? "";
    const targets = [...fout.matchAll(/@location\((\d+)\) c\d+: vec4<(\w+)>/g)].map((t) => ({
      format: t[2] === "f32" ? "rgba8unorm" : t[2] === "u32" ? "rgba32uint" : "rgba32sint",
    }));
    const attributes = [{ shaderLocation: 0, offset: 0, format: "float32x4" }];
    for (let i = 0; i < varyings; i++) attributes.push({ shaderLocation: i + 1, offset: 16 + 16 * i, format: "uint32x4" });
    if (errors.length === 0) {
      try {
        device.createRenderPipeline({
          layout: "auto",
          vertex: { module, entryPoint: "vs", buffers: [{ arrayStride: 16 + 16 * varyings, attributes }] },
          fragment: { module, entryPoint: "fs", targets },
          primitive: { topology: "triangle-list" },
          depthStencil: fout.includes("frag_depth")
            ? { format: "depth32float", depthWriteEnabled: true, depthCompare: "always" }
            : undefined,
        });
      } catch (e) {
        errors.push(String(e));
      }
    }
    const scoped = await device.popErrorScope();
    if (scoped) errors.push(scoped.message.split("\n").slice(0, 6).join(" | "));
    out.push({ name: m.name, errors });
  }
  return out;
}, modules);
await browser.close();

let bad = 0;
for (const r of results) {
  if (r.errors.length === 0) continue;
  bad++;
  console.log(`FAIL ${r.name}`);
  for (const e of r.errors.slice(0, quiet ? 1 : 8)) console.log(`  ${e}`);
}
for (const f of failed) console.log(`UNTRANSLATED ${f}: ${readFileSync(join(dir, f), "utf8").trim()}`);
console.log(`${results.length - bad}/${results.length} modules valid, ${failed.length} untranslated`);
process.exit(bad === 0 ? 0 : 1);
