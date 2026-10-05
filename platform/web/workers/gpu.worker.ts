/**
 * GPU worker (§6, §13, §16). Phase 3: consumes the framebuffer-slot
 * handoff - parks in Atomics.waitAsync on the publish counter, uploads the
 * newest published slot into a texture and draws it with a fullscreen
 * triangle, then advances the consume counter so the CPU side may reuse
 * the slot. The GPU command ring (Maxwell work) replaces the blit as the
 * main path from Phase 4; the slots stay for final presentation.
 */

import {
  CONSUME_INDEX,
  FRAMEBUFFER_FORMAT_RGBA8,
  FRAMEBUFFER_HEADER_BYTES,
  PUBLISH_INDEX,
  type PublishedFrame,
  newestFrame,
} from "@bindings/framebuffer";
import {
  GPU_STREAM_MAGIC,
  OFF_CAPACITY,
  OFF_MAGIC,
  OFF_READ,
  OFF_READ_SIGNAL,
  OFF_RING_BASE,
  OFF_WRITE,
  OFF_WRITE_SIGNAL,
  REC_PAD,
} from "@bindings/gpu-records";
import { type MemoryLayout, toByteOffset } from "@bindings/layout";
import type { GPUToMainMessage, MainToGPUMessage } from "@bindings/protocol";
import { GpuExecutor } from "./gpu-executor";
import { ShaderCacheStore } from "./shader-cache";

const self: DedicatedWorkerGlobalScope =
  globalThis as unknown as DedicatedWorkerGlobalScope;

function log(level: "debug" | "info" | "warn" | "error", message: string): void {
  const msg: GPUToMainMessage = { type: "log", level, message };
  self.postMessage(msg);
}

const BLIT_SHADER = /* wgsl */ `
@group(0) @binding(0) var frame: texture_2d<f32>;
@group(0) @binding(1) var frame_sampler: sampler;

struct VertexOut {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f,
};

@vertex fn vs(@builtin(vertex_index) index: u32) -> VertexOut {
  var corners = array(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  let p = corners[index];
  var out: VertexOut;
  out.position = vec4f(p, 0.0, 1.0);
  out.uv = vec2f((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
  return out;
}

@fragment fn fs(in: VertexOut) -> @location(0) vec4f {
  return textureSample(frame, frame_sampler, in.uv);
}`;

/* Fallback poll interval where Atomics.waitAsync is missing. */
const POLL_MS = 4;
/* Until the core turns GPU mode on, the stream header is all zeroes. */
const STREAM_IDLE_POLL_MS = 50;
const OPTIONAL_FEATURES: readonly GPUFeatureName[] = ["rg11b10ufloat-renderable", "depth32float-stencil8", "float32-filterable"];

type WaitAsync = (a: Int32Array, i: number, v: number) => { async: boolean; value: Promise<string> | string };
const waitAsync = (Atomics as unknown as { waitAsync?: WaitAsync }).waitAsync;

async function waitForChange(word: Int32Array, index: number, seen: number): Promise<void> {
  if (waitAsync) {
    const waited = waitAsync(word, index, seen);
    if (waited.async) await waited.value;
  } else {
    await new Promise((resolve) => setTimeout(resolve, POLL_MS));
  }
}

/**
 * The GPU stream (core/gpu/gpu_stream.h): GPU mode's draws, clears, copies
 * and presents, executed as they are published. Records are read in place
 * from the ring; the read position advances after each one (waking a
 * producer waiting for room).
 */
/* The persistent shader/pipeline cache attaches once both the executor
 * exists and the main thread has said which title is running. */
let activeExecutor: GpuExecutor | null = null;
let runningTitle: string | null = null;
let cacheAttached = false;

function attachShaderCache(): void {
  const executor = activeExecutor;
  const titleId = runningTitle;
  if (!executor || !titleId || cacheAttached) return;
  cacheAttached = true;
  void ShaderCacheStore.open(titleId).then(async (opened) => {
    if (!opened) return;
    const { shaders, pipelines } = opened.contents;
    if (shaders.size > 0) log("info", `shader cache: ${shaders.size} shader(s), ${pipelines.length} pipeline(s) for ${titleId}; building them in the background`);
    const started = performance.now();
    await executor.usePersistentCache(opened.store, opened.contents);
    if (pipelines.length > 0) {
      log("info", `shader cache: ${executor.stats.prewarmed} pipeline(s) ready in ${((performance.now() - started) / 1000).toFixed(1)} s`);
    }
  }).catch((e: unknown) => log("warn", `shader cache unavailable: ${e instanceof Error ? e.message : String(e)}`));
}

async function consumeStream(renderer: Renderer, memory: WebAssembly.Memory, headerBase: number): Promise<void> {
  const buffer = memory.buffer;
  const words = new Int32Array(buffer, headerBase, 12);
  const wide = new BigInt64Array(buffer, headerBase, 6);
  while (Atomics.load(words, OFF_MAGIC / 4) !== GPU_STREAM_MAGIC) {
    await new Promise((resolve) => setTimeout(resolve, STREAM_IDLE_POLL_MS));
  }
  const ringBase = Number(Atomics.load(wide, OFF_RING_BASE / 8));
  const capacity = Number(Atomics.load(wide, OFF_CAPACITY / 8));
  log("info", `GPU stream: ring 0x${ringBase.toString(16)} (${capacity >> 10} KiB)`);
  let presented = 0;
  const executor = new GpuExecutor(renderer.device, {
    presentTarget: (width, height) => {
      if (renderer.canvas.width !== width || renderer.canvas.height !== height) {
        renderer.canvas.width = width;
        renderer.canvas.height = height;
      }
      return { view: renderer.context.getCurrentTexture().createView(), format: renderer.format, width, height };
    },
    presented: (width, height) => {
      if (++presented === 1) log("info", `first WebGPU frame presented: ${width}x${height}`);
    },
    log,
  });
  activeExecutor = executor;
  attachShaderCache();
  for (;;) {
    const seen = Atomics.load(words, OFF_WRITE_SIGNAL / 4);
    const write = Number(Atomics.load(wide, OFF_WRITE / 8));
    let read = Number(Atomics.load(wide, OFF_READ / 8));
    while (read < write) {
      const at = ringBase + (read % capacity);
      const head = new DataView(buffer, at, 8);
      const type = head.getUint32(0, true);
      const size = head.getUint32(4, true);
      if (type !== REC_PAD) executor.execute(type, new Uint8Array(buffer, at + 8, size - 8));
      read += size;
      Atomics.store(wide, OFF_READ / 8, BigInt(read));
      Atomics.add(words, OFF_READ_SIGNAL / 4, 1);
      Atomics.notify(words, OFF_READ_SIGNAL / 4);
    }
    executor.flush();
    if (Number(Atomics.load(wide, OFF_WRITE / 8)) === write) await waitForChange(words, OFF_WRITE_SIGNAL / 4, seen);
  }
}

interface Renderer {
  readonly device: GPUDevice;
  readonly format: GPUTextureFormat;
  readonly context: GPUCanvasContext;
  readonly canvas: OffscreenCanvas;
  readonly pipeline: GPURenderPipeline;
  readonly sampler: GPUSampler;
  texture: GPUTexture | null;
  bindGroup: GPUBindGroup | null;
}

function present(renderer: Renderer, buffer: ArrayBufferLike, regionBase: number, frame: PublishedFrame): void {
  if (frame.format !== FRAMEBUFFER_FORMAT_RGBA8 || frame.width === 0 || frame.height === 0) return;
  const { device } = renderer;
  if (!renderer.texture || renderer.texture.width !== frame.width || renderer.texture.height !== frame.height) {
    renderer.texture?.destroy();
    renderer.texture = device.createTexture({
      size: { width: frame.width, height: frame.height },
      format: "rgba8unorm",
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });
    renderer.bindGroup = device.createBindGroup({
      layout: renderer.pipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: renderer.texture.createView() },
        { binding: 1, resource: renderer.sampler },
      ],
    });
    renderer.canvas.width = frame.width;
    renderer.canvas.height = frame.height;
  }
  const pixels = new Uint8Array(buffer, regionBase + frame.pixelOffset, frame.stride * frame.height);
  device.queue.writeTexture({ texture: renderer.texture }, pixels,
    { bytesPerRow: frame.stride, rowsPerImage: frame.height }, { width: frame.width, height: frame.height });

  const encoder = device.createCommandEncoder();
  const pass = encoder.beginRenderPass({
    colorAttachments: [{
      view: renderer.context.getCurrentTexture().createView(),
      clearValue: { r: 0, g: 0, b: 0, a: 1 },
      loadOp: "clear",
      storeOp: "store",
    }],
  });
  if (renderer.bindGroup) {
    pass.setPipeline(renderer.pipeline);
    pass.setBindGroup(0, renderer.bindGroup);
    pass.draw(3);
  }
  pass.end();
  device.queue.submit([encoder.finish()]);
}

async function consumeFrames(renderer: Renderer, memory: WebAssembly.Memory, regionBase: number): Promise<void> {
  const counters = new Int32Array(memory.buffer, regionBase, 2);
  const header = new DataView(memory.buffer, regionBase, FRAMEBUFFER_HEADER_BYTES);
  const waitAsync = (Atomics as unknown as {
    waitAsync?: (a: Int32Array, i: number, v: number) => { async: boolean; value: Promise<string> | string };
  }).waitAsync;
  let presented = 0;
  for (;;) {
    const published = Atomics.load(counters, PUBLISH_INDEX);
    if (published !== Atomics.load(counters, CONSUME_INDEX)) {
      const frame = newestFrame(header, published);
      if (frame) present(renderer, memory.buffer, regionBase, frame);
      Atomics.store(counters, CONSUME_INDEX, published);
      Atomics.notify(counters, CONSUME_INDEX);
      if (++presented === 1 && frame) log("info", `first frame presented: ${frame.width}x${frame.height}`);
    }
    if (waitAsync) {
      const waited = waitAsync(counters, PUBLISH_INDEX, published);
      if (waited.async) await waited.value;
    } else {
      await new Promise((resolve) => setTimeout(resolve, POLL_MS));
    }
  }
}

async function init(canvas: OffscreenCanvas, memory: WebAssembly.Memory, layout: MemoryLayout): Promise<void> {
  const regionBase = toByteOffset(layout.framebufferSlotBase);
  log("debug", `layout handshake: framebufferSlotBase=0x${regionBase.toString(16)}`);

  if (!("gpu" in navigator)) {
    const err: GPUToMainMessage = { type: "error", message: "WebGPU unavailable inside worker" };
    self.postMessage(err);
    return;
  }
  const gpu = (navigator as Navigator & { gpu: GPU }).gpu;
  const adapter = await gpu.requestAdapter();
  if (!adapter) {
    const err: GPUToMainMessage = { type: "error", message: "requestAdapter() returned null" };
    self.postMessage(err);
    return;
  }
  const requiredFeatures = OPTIONAL_FEATURES.filter((f) => adapter.features.has(f));
  const device = await adapter.requestDevice({ requiredFeatures });
  device.addEventListener("uncapturederror", (e: Event) => {
    log("error", `WebGPU: ${(e as GPUUncapturedErrorEvent).error.message.split("\n")[0] ?? ""}`);
  });
  const context = canvas.getContext("webgpu") as GPUCanvasContext | null;
  if (!context) {
    const err: GPUToMainMessage = { type: "error", message: "getContext('webgpu') returned null" };
    self.postMessage(err);
    return;
  }
  const format = gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: "opaque" });

  const module = device.createShaderModule({ code: BLIT_SHADER });
  const pipeline = device.createRenderPipeline({
    layout: "auto",
    vertex: { module, entryPoint: "vs" },
    fragment: { module, entryPoint: "fs", targets: [{ format }] },
    primitive: { topology: "triangle-list" },
  });
  const sampler = device.createSampler({ magFilter: "linear", minFilter: "linear" });
  const renderer: Renderer = { device, format, context, canvas, pipeline, sampler, texture: null, bindGroup: null };

  log("info", `WebGPU ready, format=${format}`);
  const ready: GPUToMainMessage = {
    type: "ready",
    adapterName: ((adapter as unknown as { info?: { vendor?: string } }).info?.vendor) ?? null,
    streamRenderer: true,
  };
  self.postMessage(ready);

  consumeFrames(renderer, memory, regionBase).catch((e: unknown) => {
    log("error", `frame consumer stopped: ${e instanceof Error ? e.message : String(e)}`);
  });
  consumeStream(renderer, memory, toByteOffset(layout.gpuRingBase)).catch((e: unknown) => {
    log("error", `GPU stream consumer stopped: ${e instanceof Error ? e.stack ?? e.message : String(e)}`);
  });
}

self.addEventListener("message", (event: MessageEvent<MainToGPUMessage>) => {
  if (event.data.type === "title") {
    runningTitle = event.data.titleId;
    attachShaderCache();
    return;
  }
  if (event.data.type === "init") {
    init(event.data.canvas, event.data.memory, event.data.layout).catch((e: unknown) => {
      const err: GPUToMainMessage = {
        type: "error",
        message: `init threw: ${e instanceof Error ? e.message : String(e)}`,
      };
      self.postMessage(err);
    });
  }
});
