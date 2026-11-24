#!/usr/bin/env node
/**
 * Times the render pipelines in a title's persistent shader cache (§13,
 * workers/shader-cache.ts): in a browser profile a game has run in
 * (perf.mjs --user-data-dir), builds every cached pipeline again with
 * createRenderPipelineAsync and prints the slowest - the shaders that
 * make the first visit to a scene stall - and how long the whole set
 * takes when several are built at once.
 *
 *   node tools/pipeline-bench.mjs --user-data-dir DIR
 *     [--port N]      the origin the game ran on (default: perf.mjs's
 *                     port for profiles; storage belongs to the origin)
 *     [--last N]      only the newest N pipelines of each cache file
 *     [--skip N]      ... after leaving out the newest N (a second run of
 *                     the same pipelines is quick: the system keeps what
 *                     it compiled)
 *     [--only HASH]   only the pipelines of this pixel shader
 *     [--replace HASH=FILE]  the shader's text from FILE instead, to try a
 *                     change to the translator's output (repeatable)
 *     [--salt N]      a first-time build whatever the system has kept: a
 *                     branch never taken, on N, opens each shader (the
 *                     timings are right, the pictures would be too)
 *     [--parallel K]  K builds in flight at a time (default 1)
 *     [--top N]       rows printed (default 20)
 *     [--dump DIR]    the WGSL of the printed rows, as DIR/<hash>.wgsl
 *     [--software]    SwiftShader instead of the hardware adapter
 *
 * A profile the game registered its service worker in fails at page.goto
 * (net::ERR_FAILED): bench a copy of the profile with its
 * Default/Service Worker folder removed (the shader cache stays).
 *
 * The profile must not be open in another browser. Nothing is written to
 * it: the pipelines are built on a device of the tool's own.
 */
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

import { chromium } from "@playwright/test";

const PROFILE_PORT = 4877; /* perf.mjs: the server port of runs with a persistent profile */
const HARDWARE_GPU_ARGS = ["--enable-unsafe-webgpu", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"];
const SOFTWARE_GPU_ARGS = ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader", "--enable-features=Vulkan"];
const BENCH_TIMEOUT_MS = 3_600_000;

function parseArgs(argv) {
  const opts = {
    userDataDir: "", port: PROFILE_PORT, last: 0, skip: 0, only: "", replace: {}, salt: 0, parallel: 1, top: 20, dump: "", software: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => argv[++i];
    if (a === "--user-data-dir") opts.userDataDir = next();
    else if (a === "--port") opts.port = Number(next());
    else if (a === "--last") opts.last = Number(next());
    else if (a === "--skip") opts.skip = Number(next());
    else if (a === "--only") opts.only = next();
    else if (a === "--replace") {
      const [hash, file] = next().split("=");
      opts.replace = { ...opts.replace, [hash]: readFileSync(file, "utf8") };
    } else if (a === "--salt") opts.salt = Number(next());
    else if (a === "--parallel") opts.parallel = Math.max(1, Number(next()));
    else if (a === "--top") opts.top = Number(next());
    else if (a === "--dump") opts.dump = next();
    else if (a === "--software") opts.software = true;
    else throw new Error(`unknown argument ${a}`);
  }
  if (!opts.userDataDir) throw new Error("--user-data-dir DIR is required");
  return opts;
}

/** Runs in the page: every cache file's pipelines, built and timed. */
async function benchInPage({ last, skip, only, replace, salt, parallel }) {
  /* As the executor's (workers/gpu-executor.ts pipelineDescriptor), with
   * the bind-group layout left to the shader: the layout costs nothing to
   * build, and blending and stencil are not shader work. */
  const VERTEX_HEADER_BYTES = 16; /* bindings/gpu-records.ts */
  const DRAW_FLAGS_WORD = 1; /* core/gpu/wgsl.h WGSL_DRAW_FLAGS: a few low bits, never a salt */
  const SALT_BASE = 0x5a000000;
  const salted = (code) =>
    salt > 0
      ? code.replace(/(@fragment fn fs\([^{]*\{\n)/, `$1  if (D[${DRAW_FLAGS_WORD}u] == ${SALT_BASE + salt}u) { discard; }\n`)
        .replace(/(@vertex fn vs\([^{]*-> VOut \{\n)/, `$1  if (D[${DRAW_FLAGS_WORD}u] == ${SALT_BASE + salt}u) { return VOut(); }\n`)
      : code;
  const root = await navigator.storage.getDirectory();
  let dir;
  try {
    dir = await root.getDirectoryHandle("shader-cache");
  } catch {
    return { error: "this profile has no shader cache at this origin" };
  }
  const adapter = await navigator.gpu?.requestAdapter();
  if (!adapter) return { error: "no WebGPU adapter" };
  const device = await adapter.requestDevice({
    requiredFeatures: [...adapter.features],
    requiredLimits: {
      maxColorAttachmentBytesPerSample: adapter.limits.maxColorAttachmentBytesPerSample,
      maxTextureDimension2D: adapter.limits.maxTextureDimension2D,
    },
  });
  const describe = (spec, module, vsModule) => {
    const attributes = [];
    let stride = 0;
    if (spec.vs && !spec.vs.pull) {
      for (let i = 0; i <= spec.vs.inputs; i++) attributes.push({ shaderLocation: i, offset: 16 * i, format: "uint32x4" });
      stride = 16 * (spec.vs.inputs + 1);
    } else if (!spec.vs) {
      attributes.push({ shaderLocation: 0, offset: 0, format: "float32x4" });
      for (let i = 0; i < spec.varyings; i++) attributes.push({ shaderLocation: i + 1, offset: VERTEX_HEADER_BYTES + 16 * i, format: "uint32x4" });
      stride = VERTEX_HEADER_BYTES + 16 * spec.varyings;
    }
    const desc = {
      layout: "auto",
      vertex: { module: vsModule, entryPoint: "vs", buffers: spec.vs?.pull ? [] : [{ arrayStride: stride, attributes }] },
      fragment: { module, entryPoint: "fs", targets: spec.targets.map((t) => (t.format ? { format: t.format, writeMask: t.writeMask } : null)) },
      primitive: {
        topology: spec.points ? "point-list" : "triangle-list",
        ...(spec.unclippedDepth && device.features.has("depth-clip-control") ? { unclippedDepth: true } : {}),
      },
    };
    if (spec.depth) {
      desc.depthStencil = { format: spec.depth.format };
      if (spec.depth.format !== "stencil8") {
        desc.depthStencil.depthWriteEnabled = spec.depth.write;
        desc.depthStencil.depthCompare = "always";
      }
    }
    return desc;
  };
  const files = [];
  for await (const [name, handle] of dir.entries()) {
    if (handle.kind !== "file") continue;
    let cache;
    try {
      cache = JSON.parse(await (await handle.getFile()).text());
    } catch {
      continue;
    }
    const shaders = { ...(cache.shaders ?? {}), ...replace };
    const specs = (cache.pipelines ?? []).filter((s) => shaders[s.wgsl] && (!s.vs || shaders[s.vs.wgsl]) && (!only || s.wgsl === only));
    const kept = skip > 0 ? specs.slice(0, Math.max(0, specs.length - skip)) : specs;
    const wanted = last > 0 ? kept.slice(-last) : kept;
    /* Modules first, checked, so a pipeline's time is its own. */
    const modules = new Map();
    const moduleStart = performance.now();
    for (const spec of wanted) {
      for (const hash of [spec.wgsl, spec.vs?.wgsl]) {
        if (hash && !modules.has(hash)) modules.set(hash, device.createShaderModule({ code: salted(shaders[hash]) }));
      }
    }
    for (const module of modules.values()) await module.getCompilationInfo();
    const moduleMs = performance.now() - moduleStart;
    const rows = [];
    let next = 0;
    const start = performance.now();
    const worker = async () => {
      while (next < wanted.length) {
        const index = next++;
        const spec = wanted[index];
        const row = {
          index, fs: spec.wgsl, fsBytes: shaders[spec.wgsl].length, vs: spec.vs?.wgsl ?? null,
          vsBytes: spec.vs ? shaders[spec.vs.wgsl].length : 0, targets: spec.targets.length, ms: 0, error: "",
        };
        const began = performance.now();
        try {
          await device.createRenderPipelineAsync(describe(spec, modules.get(spec.wgsl), modules.get(spec.vs?.wgsl ?? spec.wgsl)));
        } catch (e) {
          row.error = String(e).slice(0, 200);
        }
        row.ms = performance.now() - began;
        rows.push(row);
      }
    };
    await Promise.all(Array.from({ length: parallel }, worker));
    files.push({
      name, cached: specs.length, modules: modules.size, moduleMs, wallMs: performance.now() - start, rows,
      shaders: Object.fromEntries([...modules.keys()].map((hash) => [hash, shaders[hash]])), /* for --dump */
    });
  }
  return { files };
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const browser = await chromium.launchPersistentContext(opts.userDataDir, {
    channel: "chromium", headless: true, args: opts.software ? SOFTWARE_GPU_ARGS : HARDWARE_GPU_ARGS,
  });
  try {
    const page = await browser.newPage();
    page.setDefaultTimeout(BENCH_TIMEOUT_MS);
    /* The game's origin without its server: storage goes by the URL. */
    await page.route("**/*", (route) => route.fulfill({ contentType: "text/html", body: "<!doctype html><title>pipeline bench</title>" }));
    await page.goto(`http://localhost:${opts.port}/`);
    const result = await page.evaluate(benchInPage, {
      last: opts.last, skip: opts.skip, only: opts.only, replace: opts.replace, salt: opts.salt, parallel: opts.parallel,
    });
    if (result.error) throw new Error(result.error);
    for (const file of result.files) {
      const failed = file.rows.filter((r) => r.error);
      const sumMs = file.rows.reduce((sum, r) => sum + r.ms, 0);
      console.log(`${file.name}: ${file.rows.length} of ${file.cached} cached pipelines, ${file.modules} shader modules (${(file.moduleMs / 1000).toFixed(1)} s to check); ` +
                  `built ${opts.parallel} at a time in ${(file.wallMs / 1000).toFixed(1)} s (${(sumMs / 1000).toFixed(1)} s summed), ${failed.length} failed`);
      const slowest = [...file.rows].sort((a, b) => b.ms - a.ms).slice(0, opts.top);
      console.log("      ms  pixel shader (KiB)        vertex shader (KiB)       targets");
      for (const r of slowest) {
        console.log(`${r.ms.toFixed(0).padStart(8)}  ${r.fs} ${(r.fsBytes / 1024).toFixed(0).padStart(5)}    ${(r.vs ?? "-").padEnd(16)} ${(r.vsBytes / 1024).toFixed(0).padStart(5)}    ${r.targets}` +
                    (r.error ? `  ${r.error}` : ""));
      }
      /* Where the time is: by pixel shader, since one shader serves many pipelines. */
      const byShader = new Map();
      for (const r of file.rows) byShader.set(r.fs, (byShader.get(r.fs) ?? 0) + r.ms);
      const heavy = [...byShader.entries()].sort((a, b) => b[1] - a[1]);
      const half = heavy.findIndex((_, i) => heavy.slice(0, i + 1).reduce((sum, [, ms]) => sum + ms, 0) >= sumMs / 2) + 1;
      console.log(`${heavy.length} pixel shaders; the slowest ${half} take half the time`);
      if (failed.length) console.log(`first failure: ${failed[0].error}`);
      if (opts.dump) {
        mkdirSync(opts.dump, { recursive: true });
        for (const r of slowest) {
          writeFileSync(join(opts.dump, `${r.fs}.wgsl`), file.shaders[r.fs]);
          if (r.vs) writeFileSync(join(opts.dump, `${r.vs}.wgsl`), file.shaders[r.vs]);
        }
      }
    }
  } finally {
    await browser.close();
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
