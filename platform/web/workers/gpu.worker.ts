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
import { type MemoryLayout, toByteOffset } from "@bindings/layout";
import type { GPUToMainMessage, MainToGPUMessage } from "@bindings/protocol";

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

interface Renderer {
  readonly device: GPUDevice;
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
  const device = await adapter.requestDevice();
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
  const renderer: Renderer = { device, context, canvas, pipeline, sampler, texture: null, bindGroup: null };

  log("info", `WebGPU ready, format=${format}`);
  const ready: GPUToMainMessage = {
    type: "ready",
    adapterName: ((adapter as unknown as { info?: { vendor?: string } }).info?.vendor) ?? null,
  };
  self.postMessage(ready);

  consumeFrames(renderer, memory, regionBase).catch((e: unknown) => {
    log("error", `frame consumer stopped: ${e instanceof Error ? e.message : String(e)}`);
  });
}

self.addEventListener("message", (event: MessageEvent<MainToGPUMessage>) => {
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
