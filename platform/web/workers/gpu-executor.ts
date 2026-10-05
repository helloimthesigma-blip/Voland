/**
 * Executes the GPU stream's records (bindings/gpu-records.ts, produced by
 * core/gpu/raster3d.c's GPU mode) on a WebGPU device. Shared by the GPU
 * worker (records from the shared-memory ring, presents to the canvas) and
 * tools/replay-gpu-stream.mjs (records from a file voland-cli wrote).
 *
 * Batching: draws accumulate in one command encoder; consecutive draws to
 * the same attachments share a render pass. Per-draw data (constants +
 * constant buffers) and vertices go to CPU staging arrays and reach their
 * GPU buffers with writeBuffer just before the encoder is submitted - the
 * queue orders that write after every earlier submission. The buffers
 * rotate (BUFFER_SETS) so that write does not stall on in-flight work.
 * Anything that must happen on the queue timeline
 * between draws (texture uploads) flushes the batch first.
 *
 * Full-surface clears are deferred: the next pass that attaches the
 * surface clears it as its load operation (one pass instead of two or
 * three per frame); any other use first performs the clear in a pass of
 * its own. Copies between equal formats and sizes are texture copies.
 *
 * Mipmapped textures (TEXTURE_CREATE levels > 1) receive level 0 only;
 * the rest of the chain is rebuilt from it (2x2 box filter, render passes
 * in the encoder) before the next draw samples a texture written since.
 */

import { type CacheContents, type PipelineSpec, type ShaderCacheStore, specKey, wgslHash } from "./shader-cache";
import {
  BIND_DATA,
  BIND_FILTERED,
  BIND_SAMPLER,
  BIND_TEXTURE,
  BINDING_BYTES,
  BLEND_FACTORS,
  BLEND_OPS,
  CLEAR_COLOR,
  CLEAR_DEPTH,
  CLEAR_STENCIL,
  COMPARES,
  type Clear,
  type Copy,
  DRAW_BYTES,
  type Draw,
  FORMATS,
  PRESENT_FLIP_X,
  PRESENT_FLIP_Y,
  type Present,
  REC_CLEAR,
  REC_COPY,
  REC_DRAW,
  REC_PRESENT,
  REC_SHADER,
  REC_TEXTURE_CREATE,
  REC_TEXTURE_DESTROY,
  REC_TEXTURE_WRITE,
  SAMPLER_LINEAR,
  SAMPLER_MIN_LINEAR,
  SAMPLER_MIP_LINEAR,
  SAMPLER_WRAPS,
  STENCIL_OPS,
  TEXTURE_WRITE_BYTES,
  USAGE_RENDER,
  VERTEX_HEADER_BYTES,
  parseClear,
  parseCopy,
  parseDraw,
  parsePresent,
  parseTextureCreate,
  parseTextureWrite,
} from "@bindings/gpu-records";

const VERTEX_BUFFER_BYTES = 16 * 1024 * 1024;
const DATA_BUFFER_BYTES = 16 * 1024 * 1024;
/* Batches rotate through this many buffer pairs, so writing the next
 * batch's data never waits for the GPU to finish reading the last one. */
const BUFFER_SETS = 3;
const DATA_ALIGN = 256;
/* Draw data binds through a dynamic offset into a window of this size
 * (the most a draw carries: constants + 18 constant buffers of 64 KB), so
 * one bind group serves every draw with the same textures and samplers. */
const DATA_WINDOW_BYTES = 2 * 1024 * 1024;
const BIND_GROUP_CACHE_LIMIT = 4096;
const MAX_TEXTURES = 16; /* core/gpu/wgsl.h WGSL_MAX_TEXTURES: samplers bind at 1 + MAX_TEXTURES + i */

export interface PresentTarget {
  readonly view: GPUTextureView;
  readonly format: GPUTextureFormat;
  readonly width: number;
  readonly height: number;
}

export interface ExecutorHost {
  /** Where a PRESENT of a `width` x `height` image goes (null: dropped). */
  presentTarget(width: number, height: number): PresentTarget | null;
  /** After the present's commands were submitted. */
  presented(width: number, height: number): void;
  log(level: "debug" | "info" | "warn" | "error", message: string): void;
}

interface Tex {
  readonly texture: GPUTexture;
  readonly format: GPUTextureFormat;
  readonly width: number;
  readonly height: number;
  readonly layers: number;
  readonly levels: number;
  readonly sampleType: GPUTextureSampleType;
  readonly sampleView: GPUTextureView; /* 2d-array, depth aspect for depth formats */
  readonly renderView: GPUTextureView | null;
  readonly fallback: boolean;          /* not the format the producer asked for: its uploads do not fit */
}

interface CachedPipeline {
  readonly pipeline: GPURenderPipeline;
  readonly layout: GPUBindGroupLayout;
}

export interface ExecutorStats {
  draws: number;
  passes: number;
  submits: number;
  pipelines: number;
  /** Pipelines built ahead of use from the persistent cache. */
  prewarmed: number;
  presents: number;
  errors: number;
  shadows: number;
  copies: number;
}

const FULLSCREEN_VS = /* wgsl */ `
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
  let c = array(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(c[i], 0.0, 1.0);
}`;

/* Copies / presents: dst pixel -> src texel through the rectangles in P
 * (f32 sx, sy, sw, sh, dx, dy, dw, dh, then u32 flags: 1 flip x, 2 flip y,
 * 4 bilinear, 8 encode to sRGB, 16 decode from sRGB). */
function blitShader(filterable: boolean): string {
  return `${FULLSCREEN_VS}
@group(0) @binding(0) var<storage, read> P: array<u32>;
@group(0) @binding(1) var S: texture_2d_array<f32>;
${filterable ? "@group(0) @binding(2) var L: sampler;" : ""}
fn enc(c: f32) -> f32 { if (c <= 0.0031308) { return c * 12.92; } return 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
fn dec(c: f32) -> f32 { if (c <= 0.04045) { return c / 12.92; } return pow((c + 0.055) / 1.055, 2.4); }
@fragment fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let s = vec4<f32>(bitcast<f32>(P[0]), bitcast<f32>(P[1]), bitcast<f32>(P[2]), bitcast<f32>(P[3]));
  let d = vec4<f32>(bitcast<f32>(P[4]), bitcast<f32>(P[5]), bitcast<f32>(P[6]), bitcast<f32>(P[7]));
  let flags = P[8];
  var u = (pos.x - d.x) / d.z;
  var v = (pos.y - d.y) / d.w;
  if ((flags & 1u) != 0u) { u = 1.0 - u; }
  if ((flags & 2u) != 0u) { v = 1.0 - v; }
  let p = vec2<f32>(s.x + u * s.z, s.y + v * s.w);
  let dm = vec2<f32>(textureDimensions(S));
  var c = vec4<f32>(0.0);
  ${filterable
    ? "if ((flags & 4u) != 0u) { c = textureSampleLevel(S, L, p / dm, 0, 0.0); } else { c = textureLoad(S, vec2<i32>(clamp(floor(p), vec2<f32>(0.0), dm - 1.0)), 0, 0); }"
    : "c = textureLoad(S, vec2<i32>(clamp(floor(p), vec2<f32>(0.0), dm - 1.0)), 0, 0);"}
  if ((flags & 8u) != 0u) { c = vec4<f32>(enc(c.r), enc(c.g), enc(c.b), c.a); }
  if ((flags & 16u) != 0u) { c = vec4<f32>(dec(c.r), dec(c.g), dec(c.b), c.a); }
  return c;
}`;
}

/* Integer copies: nearest texels, raw values (no flips or conversions
 * apply between integer surfaces). */
function intBlitShader(type: "u32" | "i32"): string {
  return `${FULLSCREEN_VS}
@group(0) @binding(0) var<storage, read> P: array<u32>;
@group(0) @binding(1) var S: texture_2d_array<${type}>;
@fragment fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<${type}> {
  let s = vec4<f32>(bitcast<f32>(P[0]), bitcast<f32>(P[1]), bitcast<f32>(P[2]), bitcast<f32>(P[3]));
  let d = vec4<f32>(bitcast<f32>(P[4]), bitcast<f32>(P[5]), bitcast<f32>(P[6]), bitcast<f32>(P[7]));
  let p = vec2<f32>(s.x + (pos.x - d.x) / d.z * s.z, s.y + (pos.y - d.y) / d.w * s.w);
  let dm = vec2<f32>(textureDimensions(S));
  return textureLoad(S, vec2<i32>(clamp(floor(p), vec2<f32>(0.0), dm - 1.0)), 0, 0);
}`;
}

function clearShader(type: "f32" | "u32" | "i32"): string {
  return `${FULLSCREEN_VS}
@group(0) @binding(0) var<storage, read> P: array<u32>;
@fragment fn fs() -> @location(0) vec4<${type}> { return bitcast<vec4<${type}>>(vec4<u32>(P[0], P[1], P[2], P[3])); }`;
}

/* One mip level from the one above: the 2x2 box (edge texels repeat for
 * odd sizes). */
const MIP_SHADER = `${FULLSCREEN_VS}
@group(0) @binding(0) var S: texture_2d<f32>;
@fragment fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let m = vec2<i32>(textureDimensions(S)) - 1;
  let p = vec2<i32>(floor(pos.xy)) * 2;
  return 0.25 * (textureLoad(S, min(p, m), 0) + textureLoad(S, min(p + vec2<i32>(1, 0), m), 0) +
                 textureLoad(S, min(p + vec2<i32>(0, 1), m), 0) + textureLoad(S, min(p + vec2<i32>(1, 1), m), 0));
}`;

const DEPTH_CLEAR_SHADER = `${FULLSCREEN_VS}
@group(0) @binding(0) var<storage, read> P: array<u32>;
struct O { @builtin(frag_depth) depth: f32 }
@fragment fn fs() -> O { var o: O; o.depth = bitcast<f32>(P[0]); return o; }`;

function isDepthFormat(f: GPUTextureFormat): boolean {
  return f.startsWith("depth") || f === "stencil8";
}
function hasDepth(f: GPUTextureFormat): boolean {
  return f.startsWith("depth");
}
function hasStencil(f: GPUTextureFormat): boolean {
  return f === "depth24plus-stencil8" || f === "depth32float-stencil8" || f === "stencil8";
}
function isSrgb(f: GPUTextureFormat): boolean {
  return f.endsWith("-srgb");
}
function sampleTypeOf(f: GPUTextureFormat): GPUTextureSampleType {
  if (f === "stencil8") return "uint";
  if (f.endsWith("uint")) return "uint";
  if (f.endsWith("sint")) return "sint";
  return "unfilterable-float";
}
function filterable(f: GPUTextureFormat): boolean {
  return !isDepthFormat(f) && !f.endsWith("int") && !f.startsWith("r32float") && !f.startsWith("rg32float") &&
    !f.startsWith("rgba32float");
}
function shaderType(f: GPUTextureFormat): "f32" | "u32" | "i32" {
  if (f.endsWith("uint")) return "u32";
  if (f.endsWith("sint")) return "i32";
  return "f32";
}

function f32Bits(v: number): number {
  const b = new DataView(new ArrayBuffer(4));
  b.setFloat32(0, v, true);
  return b.getUint32(0, true);
}

export class GpuExecutor {
  readonly stats: ExecutorStats = { draws: 0, passes: 0, submits: 0, pipelines: 0, prewarmed: 0, presents: 0, errors: 0, shadows: 0, copies: 0 };
  private readonly textures = new Map<number, Tex>();
  private readonly shaders = new Map<number, GPUShaderModule>();
  /* Per-session shader ids -> WGSL content hash; modules shared by hash. */
  private readonly shaderHashes = new Map<number, string>();
  private readonly modulesByHash = new Map<string, GPUShaderModule>();
  private cacheStore: ShaderCacheStore | null = null;
  private readonly pipelines = new Map<string, CachedPipeline>();
  private readonly shadows = new Map<string, Tex>();
  private readonly samplers = new Map<number, GPUSampler>();
  private readonly vertexBuffers: GPUBuffer[] = [];
  private readonly dataBuffers: GPUBuffer[] = [];
  private set = 0;
  private get vertexBuffer(): GPUBuffer { return this.vertexBuffers[this.set] as GPUBuffer; }
  private get dataBuffer(): GPUBuffer { return this.dataBuffers[this.set] as GPUBuffer; }
  private readonly vertexStaging = new Uint8Array(VERTEX_BUFFER_BYTES);
  private readonly dataStaging = new Uint8Array(DATA_BUFFER_BYTES);
  private readonly linearSampler: GPUSampler;
  private vertexUsed = 0;
  private dataUsed = 0;
  private encoder: GPUCommandEncoder | null = null;
  private pass: GPURenderPassEncoder | null = null;
  private passKey = "";
  private passSize: readonly [number, number] = [0, 0];
  private pendingDestroy: GPUTexture[] = [];
  private readonly mipsStale = new Set<GPUTexture>(); /* level 0 written since the chain was built */
  /* Deferred full clears by texture id: colour value, or depth / stencil. */
  private readonly pendingColor = new Map<number, GPUColorDict>();
  private readonly pendingDepth = new Map<number, { depth: number | null; stencil: number | null }>();
  private readonly bindGroups = new Map<string, GPUBindGroup>();
  private readonly objectIds = new WeakMap<object, number>();
  private nextObjectId = 1;
  private readonly warned = new Set<string>();

  constructor(private readonly device: GPUDevice, private readonly host: ExecutorHost) {
    for (let i = 0; i < BUFFER_SETS; i++) {
      this.vertexBuffers.push(device.createBuffer({ size: VERTEX_BUFFER_BYTES, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST }));
      this.dataBuffers.push(device.createBuffer({ size: DATA_BUFFER_BYTES, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }));
    }
    this.linearSampler = device.createSampler({ magFilter: "linear", minFilter: "linear" });
  }

  /** One record (the payload after the 8-byte record header). */
  execute(type: number, payload: Uint8Array): void {
    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    switch (type) {
      case REC_TEXTURE_CREATE: this.textureCreate(view); break;
      case REC_TEXTURE_DESTROY: this.textureDestroy(view.getUint32(0, true)); break;
      case REC_TEXTURE_WRITE: this.textureWrite(view, payload); break;
      case REC_SHADER: this.shader(view, payload); break;
      case REC_CLEAR: this.clear(parseClear(view)); break;
      case REC_DRAW: this.draw(parseDraw(view), view, payload); break;
      case REC_COPY: this.copy(parseCopy(view)); break;
      case REC_PRESENT: this.present(parsePresent(view)); break;
      default: this.warnOnce(`record ${type}`, `unknown GPU record type ${type}`); break;
    }
  }

  /** Submits everything recorded so far. */
  readonly prof = { write: 0, finish: 0, submit: 0 };
  flush(): void {
    this.endPass();
    if (!this.encoder) return;
    const t0 = performance.now();
    if (this.vertexUsed) this.device.queue.writeBuffer(this.vertexBuffer, 0, this.vertexStaging.slice(0, this.vertexUsed));
    if (this.dataUsed) this.device.queue.writeBuffer(this.dataBuffer, 0, this.dataStaging.slice(0, this.dataUsed));
    const t1 = performance.now();
    const cb = this.encoder.finish();
    const t2 = performance.now();
    this.device.queue.submit([cb]);
    const t3 = performance.now();
    this.prof.write += t1 - t0; this.prof.finish += t2 - t1; this.prof.submit += t3 - t2;
    this.stats.submits++;
    this.encoder = null;
    if (this.vertexUsed || this.dataUsed) this.set = (this.set + 1) % BUFFER_SETS;
    this.vertexUsed = 0;
    this.dataUsed = 0;
    for (const t of this.pendingDestroy) t.destroy();
    this.pendingDestroy = [];
  }

  private idOf(o: object): number {
    let id = this.objectIds.get(o);
    if (id === undefined) {
      id = this.nextObjectId++;
      this.objectIds.set(o, id);
    }
    return id;
  }

  /** Performs a deferred clear of texture `id` now (before it is read or
   * written other than as the next pass's attachment). */
  private materialize(id: number): void {
    const color = this.pendingColor.get(id);
    const depth = this.pendingDepth.get(id);
    if (!color && !depth) return;
    const t = this.textures.get(id);
    this.pendingColor.delete(id);
    this.pendingDepth.delete(id);
    if (!t || !t.renderView) return;
    if (color) {
      this.beginPass(`${id}|0`, [{ view: t.renderView, loadOp: "clear", storeOp: "store", clearValue: color }], null,
        [t.width, t.height]);
    } else if (depth) {
      this.beginPass(`|${id}`, [], this.depthAttachment(t, depth.depth, depth.stencil), [t.width, t.height]);
    }
    this.endPass();
  }

  private warnOnce(key: string, message: string): void {
    if (this.warned.has(key)) return;
    this.warned.add(key);
    this.host.log("warn", message);
  }

  /* ---- resources ---- */

  private makeTex(format: GPUTextureFormat, width: number, height: number, layers: number, render: boolean,
                  fallback: boolean, levels = 1): Tex {
    const usage = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC |
      (render || levels > 1 ? GPUTextureUsage.RENDER_ATTACHMENT : 0);
    const texture = this.device.createTexture({ size: [width, height, layers], format, usage, mipLevelCount: levels });
    const aspect: GPUTextureAspect = hasDepth(format) && hasStencil(format) ? "depth-only" : "all";
    return {
      texture, format, width, height, layers, levels, sampleType: sampleTypeOf(format), fallback,
      sampleView: texture.createView({ dimension: "2d-array", aspect }),
      renderView: render
        ? texture.createView({ dimension: "2d", baseArrayLayer: 0, arrayLayerCount: 1, baseMipLevel: 0, mipLevelCount: 1 })
        : null,
    };
  }

  /** Rebuilds `t`'s mip chain from level 0, in the encoder (after earlier
   * passes, before later ones). */
  private buildMips(t: Tex): void {
    this.mipsStale.delete(t.texture);
    this.endPass();
    const entries: GPUBindGroupLayoutEntry[] = [
      { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "unfilterable-float", viewDimension: "2d" } },
    ];
    const { pipeline, layout } = this.simplePipeline("mip", MIP_SHADER, t.format, 0xf, null, entries);
    const encoder = this.ensureEncoder();
    for (let layer = 0; layer < t.layers; layer++) {
      for (let level = 1; level < t.levels; level++) {
        const view = (mip: number): GPUTextureView => t.texture.createView({
          dimension: "2d", baseMipLevel: mip, mipLevelCount: 1, baseArrayLayer: layer, arrayLayerCount: 1,
        });
        const pass = encoder.beginRenderPass({ colorAttachments: [{ view: view(level), loadOp: "clear", storeOp: "store" }] });
        pass.setPipeline(pipeline);
        pass.setBindGroup(0, this.device.createBindGroup({ layout, entries: [{ binding: 0, resource: view(level - 1) }] }));
        pass.draw(3);
        pass.end();
        this.stats.passes++;
      }
    }
  }

  private textureCreate(v: DataView): void {
    const c = parseTextureCreate(v);
    let format = FORMATS[c.format] ?? null;
    if (!format) {
      this.warnOnce(`format ${c.format}`, `GPU texture format ${c.format} unknown`);
      return;
    }
    let fallback = false;
    if (format === "rg11b10ufloat" && (c.usage & USAGE_RENDER) && !this.device.features.has("rg11b10ufloat-renderable")) {
      format = "rgba16float";
      fallback = true;
    }
    if (format === "depth32float-stencil8" && !this.device.features.has("depth32float-stencil8")) {
      format = "depth24plus-stencil8";
      fallback = true;
    }
    const old = this.textures.get(c.id);
    if (old) {
      this.pendingDestroy.push(old.texture);
      this.mipsStale.delete(old.texture);
    }
    this.pendingColor.delete(c.id);
    this.pendingDepth.delete(c.id);
    const width = Math.max(1, c.width), height = Math.max(1, c.height);
    /* Mip chains: float formats the box filter can render (the producer
     * asks for them on sampled float textures only). */
    const full = Math.floor(Math.log2(Math.max(width, height))) + 1;
    const levels = (c.usage & USAGE_RENDER) === 0 && shaderType(format) === "f32" && !isDepthFormat(format)
      ? Math.min(c.levels, full) : 1;
    this.textures.set(c.id, this.makeTex(format, width, height, Math.max(1, c.layers), (c.usage & USAGE_RENDER) !== 0,
      fallback, levels));
  }

  private textureDestroy(id: number): void {
    const t = this.textures.get(id);
    if (!t) return;
    this.textures.delete(id);
    this.mipsStale.delete(t.texture);
    this.pendingColor.delete(id);
    this.pendingDepth.delete(id);
    this.pendingDestroy.push(t.texture);
  }

  private textureWrite(v: DataView, payload: Uint8Array): void {
    const w = parseTextureWrite(v);
    const t = this.textures.get(w.id);
    if (!t || t.fallback || isDepthFormat(t.format)) return;
    this.materialize(w.id);
    this.flush(); /* earlier draws read the old contents */
    this.device.queue.writeTexture(
      { texture: t.texture, origin: { x: w.x, y: w.y, z: w.layer } },
      payload.subarray(TEXTURE_WRITE_BYTES, TEXTURE_WRITE_BYTES + w.dataBytes),
      { bytesPerRow: w.bytesPerRow, rowsPerImage: w.height },
      { width: w.width, height: w.height, depthOrArrayLayers: 1 },
    );
    if (t.levels > 1) this.mipsStale.add(t.texture);
  }

  private shader(v: DataView, payload: Uint8Array): void {
    const id = v.getUint32(0, true);
    const bytes = v.getUint32(4, true);
    let code = new TextDecoder().decode(payload.slice(8, 8 + bytes));
    if ((globalThis as { TRIVIAL_SHADERS?: boolean }).TRIVIAL_SHADERS) {
      code = code.replace(/@fragment fn fs\(([^)]*)\)( -> FOut)? \{[\s\S]*$/, (_m, args: string, ret: string | undefined) =>
        ret ? `@fragment fn fs(${args}) -> FOut { var fo: FOut; return fo; }` : `@fragment fn fs(${args}) { }`);
    }
    const hash = wgslHash(code);
    let module = this.modulesByHash.get(hash);
    if (!module) {
      module = this.device.createShaderModule({ code });
      this.modulesByHash.set(hash, module);
    }
    this.shaders.set(id, module);
    this.shaderHashes.set(id, hash);
    this.cacheStore?.recordShader(hash, code);
  }

  /* ---- persistent pipeline cache (shader-cache.ts) ---- */

  /** From now on, new shaders and pipelines are recorded in `store`; the
   * ones it already holds are built in the background, oldest first, so
   * they are ready before the title draws with them. */
  usePersistentCache(store: ShaderCacheStore, contents: CacheContents): Promise<void> {
    this.cacheStore = store;
    for (const [hash, code] of contents.shaders) {
      if (!this.modulesByHash.has(hash)) this.modulesByHash.set(hash, this.device.createShaderModule({ code }));
    }
    const builds = contents.pipelines.map(async (spec) => {
      const key = specKey(spec);
      const module = this.modulesByHash.get(spec.wgsl);
      if (!module || this.pipelines.has(key)) return;
      const { desc, layout } = this.pipelineDescriptor(spec, module);
      try {
        const pipeline = await this.device.createRenderPipelineAsync(desc);
        if (!this.pipelines.has(key)) {
          this.pipelines.set(key, { pipeline, layout });
          this.stats.prewarmed++;
        }
      } catch {
        /* not valid here (another adapter's formats): built on demand */
      }
    });
    return Promise.all(builds).then(() => undefined);
  }

  /* ---- passes ---- */

  private ensureEncoder(): GPUCommandEncoder {
    if (!this.encoder) this.encoder = this.device.createCommandEncoder();
    return this.encoder;
  }

  /** Whether the open pass attaches texture `id`. */
  private attachedNow(id: number): boolean {
    if (!this.pass) return false;
    const [colors, depth] = this.passKey.split("|");
    return depth === String(id) || (colors ?? "").split(",").includes(String(id));
  }

  private endPass(): void {
    if (this.pass) this.pass.end();
    this.pass = null;
    this.passKey = "";
  }

  private beginPass(key: string, colors: readonly (GPURenderPassColorAttachment | null)[],
                    depth: GPURenderPassDepthStencilAttachment | null, size: readonly [number, number]): GPURenderPassEncoder {
    this.endPass();
    const desc: GPURenderPassDescriptor = { colorAttachments: [...colors] };
    if (depth) desc.depthStencilAttachment = depth;
    this.pass = this.ensureEncoder().beginRenderPass(desc);
    this.passKey = key;
    this.passSize = size;
    this.stats.passes++;
    return this.pass;
  }

  private depthAttachment(t: Tex, clearDepth: number | null, clearStencil: number | null): GPURenderPassDepthStencilAttachment {
    const view = t.renderView;
    if (!view) throw new Error("depth texture without a render view");
    const a: GPURenderPassDepthStencilAttachment = { view };
    if (hasDepth(t.format)) {
      a.depthLoadOp = clearDepth === null ? "load" : "clear";
      a.depthStoreOp = "store";
      if (clearDepth !== null) a.depthClearValue = clearDepth;
    }
    if (hasStencil(t.format)) {
      a.stencilLoadOp = clearStencil === null ? "load" : "clear";
      a.stencilStoreOp = "store";
      if (clearStencil !== null) a.stencilClearValue = clearStencil;
    }
    return a;
  }

  /** A pass rendering into these attachments (loading their contents). */
  private passFor(colorIds: readonly number[], depthId: number): GPURenderPassEncoder | null {
    const key = `${colorIds.join(",")}|${depthId}`;
    if (this.pass && this.passKey === key) return this.pass;
    let size: [number, number] | null = null;
    const colors: (GPURenderPassColorAttachment | null)[] = colorIds.map((id) => {
      const t = id ? this.textures.get(id) : undefined;
      if (!t || !t.renderView) return null;
      size = [t.width, t.height];
      const clear = this.pendingColor.get(id);
      this.pendingColor.delete(id);
      return clear ? { view: t.renderView, loadOp: "clear", storeOp: "store", clearValue: clear }
        : { view: t.renderView, loadOp: "load", storeOp: "store" };
    });
    const dt = depthId ? this.textures.get(depthId) : undefined;
    const pd = this.pendingDepth.get(depthId);
    this.pendingDepth.delete(depthId);
    const depth = dt && dt.renderView ? this.depthAttachment(dt, pd?.depth ?? null, pd?.stencil ?? null) : null;
    if (dt) size = [dt.width, dt.height];
    if (!size) return null;
    return this.beginPass(key, colors, depth, size);
  }

  /** Stages `bytes` at a DATA_ALIGN offset; `window`: the binding reads
   * that many bytes from the offset (they must lie inside the buffer). */
  private stageData(bytes: Uint8Array, window = 0): number {
    let at = Math.ceil(this.dataUsed / DATA_ALIGN) * DATA_ALIGN;
    if (at + Math.max(bytes.byteLength, window) > DATA_BUFFER_BYTES) {
      this.flush();
      at = 0;
    }
    this.dataStaging.set(bytes, at);
    this.dataUsed = at + bytes.byteLength;
    return at;
  }

  private stageWords(words: readonly number[]): number {
    const bytes = new Uint8Array(Math.max(16, words.length * 4));
    const v = new DataView(bytes.buffer);
    words.forEach((w, i) => v.setUint32(4 * i, w >>> 0, true));
    return this.stageData(bytes);
  }

  /* ---- pipelines ---- */

  private cached(key: string, make: () => CachedPipeline): CachedPipeline {
    const hit = this.pipelines.get(key);
    if (hit) return hit;
    const made = make();
    this.pipelines.set(key, made);
    this.stats.pipelines++;
    return made;
  }

  private sampler(state: number): GPUSampler {
    let s = this.samplers.get(state);
    if (!s) {
      const filter = (bit: number): GPUFilterMode => (state & bit ? "linear" : "nearest");
      const wrap = (axis: number): GPUAddressMode => SAMPLER_WRAPS[(state >> (1 + 2 * axis)) & 3] ?? "clamp-to-edge";
      s = this.device.createSampler({
        magFilter: filter(SAMPLER_LINEAR), minFilter: filter(SAMPLER_MIN_LINEAR), mipmapFilter: filter(SAMPLER_MIP_LINEAR),
        addressModeU: wrap(0), addressModeV: wrap(1), addressModeW: wrap(2),
      });
      this.samplers.set(state, s);
    }
    return s;
  }

  /** The spec (cache identity) of draw `d`'s pipeline. */
  private drawSpec(d: Draw, textures: readonly Tex[], filtered: readonly boolean[],
                   colorFormats: readonly (GPUTextureFormat | null)[], depth: Tex | undefined): PipelineSpec {
    return {
      wgsl: this.shaderHashes.get(d.shaderId) ?? `id${d.shaderId}`,
      varyings: d.varyingCount,
      textures: textures.map((t, i) => (filtered[i] ? "F" : t.sampleType)),
      targets: d.targets.map((t, i) => ({
        format: colorFormats[i] ?? null,
        writeMask: t.writeMask,
        blend: t.blend ? [t.colorOp, t.colorSrc, t.colorDst, t.alphaOp, t.alphaSrc, t.alphaDst] : null,
      })),
      depth: depth ? {
        format: depth.format, test: d.depthTest, write: d.depthWrite, compare: d.depthCompare,
        stencil: d.stencil ? {
          front: [d.stencilFront.compare, d.stencilFront.fail, d.stencilFront.depthFail, d.stencilFront.pass],
          back: [d.stencilBack.compare, d.stencilBack.fail, d.stencilBack.depthFail, d.stencilBack.pass],
          readMask: d.stencilReadMask, writeMask: d.stencilWriteMask,
        } : null,
      } : null,
    };
  }

  private drawPipeline(d: Draw, module: GPUShaderModule, textures: readonly Tex[], filtered: readonly boolean[],
                       colorFormats: readonly (GPUTextureFormat | null)[], depth: Tex | undefined): CachedPipeline {
    const spec = this.drawSpec(d, textures, filtered, colorFormats, depth);
    return this.cached(specKey(spec), () => {
      const { desc, layout } = this.pipelineDescriptor(spec, module);
      const made = { pipeline: this.device.createRenderPipeline(desc), layout };
      if (this.shaderHashes.has(d.shaderId)) this.cacheStore?.recordPipeline(spec);
      return made;
    });
  }

  /** The descriptor (and its bind-group layout) `spec` stands for. */
  private pipelineDescriptor(spec: PipelineSpec, module: GPUShaderModule):
    { readonly desc: GPURenderPipelineDescriptor; readonly layout: GPUBindGroupLayout } {
    const entries: GPUBindGroupLayoutEntry[] = [
      { binding: 0, visibility: GPUShaderStage.FRAGMENT, buffer: { type: "read-only-storage", hasDynamicOffset: true } },
    ];
    spec.textures.forEach((kind, i) => {
      const filtered = kind === "F";
      entries.push({
        binding: 1 + i, visibility: GPUShaderStage.FRAGMENT,
        texture: { sampleType: filtered ? "float" : kind as GPUTextureSampleType, viewDimension: "2d-array" },
      });
      if (filtered) entries.push({ binding: 1 + MAX_TEXTURES + i, visibility: GPUShaderStage.FRAGMENT, sampler: { type: "filtering" } });
    });
    const layout = this.device.createBindGroupLayout({ entries });
    const attributes: GPUVertexAttribute[] = [{ shaderLocation: 0, offset: 0, format: "float32x4" }];
    for (let i = 0; i < spec.varyings; i++) attributes.push({ shaderLocation: i + 1, offset: VERTEX_HEADER_BYTES + 16 * i, format: "uint32x4" });
    const targets: (GPUColorTargetState | null)[] = spec.targets.map((t) => {
      const format = t.format as GPUTextureFormat | null;
      if (!format) return null;
      const state: GPUColorTargetState = { format, writeMask: t.writeMask };
      const [colorOp = 0, colorSrc = 0, colorDst = 0, alphaOp = 0, alphaSrc = 0, alphaDst = 0] = t.blend ?? [];
      if (t.blend && !format.endsWith("int")) {
        const minmax = (op: number): boolean => op === 3 || op === 4;
        state.blend = {
          color: {
            operation: BLEND_OPS[colorOp] ?? "add",
            srcFactor: minmax(colorOp) ? "one" : BLEND_FACTORS[colorSrc] ?? "one",
            dstFactor: minmax(colorOp) ? "one" : BLEND_FACTORS[colorDst] ?? "zero",
          },
          alpha: {
            operation: BLEND_OPS[alphaOp] ?? "add",
            srcFactor: minmax(alphaOp) ? "one" : BLEND_FACTORS[alphaSrc] ?? "one",
            dstFactor: minmax(alphaOp) ? "one" : BLEND_FACTORS[alphaDst] ?? "zero",
          },
        };
      }
      return state;
    });
    const desc: GPURenderPipelineDescriptor = {
      layout: this.device.createPipelineLayout({ bindGroupLayouts: [layout] }),
      vertex: { module, entryPoint: "vs", buffers: [{ arrayStride: VERTEX_HEADER_BYTES + 16 * spec.varyings, attributes }] },
      fragment: { module, entryPoint: "fs", targets },
      primitive: { topology: "triangle-list", frontFace: "ccw", cullMode: "none" },
    };
    const depth = spec.depth;
    if (depth) {
      const format = depth.format as GPUTextureFormat;
      const ds: GPUDepthStencilState = { format };
      if (hasDepth(format)) {
        ds.depthWriteEnabled = depth.write;
        ds.depthCompare = depth.test ? COMPARES[depth.compare] ?? "always" : "always";
      }
      if (depth.stencil && hasStencil(format)) {
        const face = (f: readonly number[]): GPUStencilFaceState => ({
          compare: COMPARES[f[0] ?? 0] ?? "always", failOp: STENCIL_OPS[f[1] ?? 0] ?? "keep",
          depthFailOp: STENCIL_OPS[f[2] ?? 0] ?? "keep", passOp: STENCIL_OPS[f[3] ?? 0] ?? "keep",
        });
        ds.stencilFront = face(depth.stencil.front);
        ds.stencilBack = face(depth.stencil.back);
        ds.stencilReadMask = depth.stencil.readMask;
        ds.stencilWriteMask = depth.stencil.writeMask;
      }
      desc.depthStencil = ds;
    }
    return { desc, layout };
  }

  private simplePipeline(kind: string, code: string, format: GPUTextureFormat | null, writeMask: number,
                         depth: GPUDepthStencilState | null, entries: GPUBindGroupLayoutEntry[]): CachedPipeline {
    return this.cached(`${kind}|${format ?? ""}|${writeMask}|${depth ? JSON.stringify(depth) : ""}`, () => {
      const module = this.device.createShaderModule({ code });
      const layout = this.device.createBindGroupLayout({ entries });
      const desc: GPURenderPipelineDescriptor = {
        layout: this.device.createPipelineLayout({ bindGroupLayouts: [layout] }),
        vertex: { module, entryPoint: "vs" },
        fragment: { module, entryPoint: "fs", targets: format ? [{ format, writeMask }] : [] },
        primitive: { topology: "triangle-list" },
      };
      if (depth) desc.depthStencil = depth;
      return { pipeline: this.device.createRenderPipeline(desc), layout };
    });
  }

  /** A copy of attachment `t` for a draw that also samples it. */
  private shadowOf(id: number, t: Tex): Tex {
    this.materialize(id);
    const key = `${t.format}:${t.width}x${t.height}x${t.layers}`;
    let s = this.shadows.get(key);
    if (!s) {
      s = this.makeTex(t.format, t.width, t.height, t.layers, false, false);
      this.shadows.set(key, s);
    }
    this.endPass();
    this.stats.shadows++;
    this.ensureEncoder().copyTextureToTexture({ texture: t.texture }, { texture: s.texture }, [t.width, t.height, t.layers]);
    return s;
  }

  private scissor(pass: GPURenderPassEncoder, rect: readonly number[]): boolean {
    const [w, h] = this.passSize;
    const x0 = Math.max(0, rect[0] ?? 0), y0 = Math.max(0, rect[1] ?? 0);
    const x1 = Math.min(w, (rect[0] ?? 0) + (rect[2] ?? 0)), y1 = Math.min(h, (rect[1] ?? 0) + (rect[3] ?? 0));
    if (x1 <= x0 || y1 <= y0) return false;
    pass.setScissorRect(x0, y0, x1 - x0, y1 - y0);
    return true;
  }

  /* ---- records ---- */

  private draw(d: Draw, v: DataView, payload: Uint8Array): void {
    const module = this.shaders.get(d.shaderId);
    if (!module) {
      this.warnOnce(`shader ${d.shaderId}`, `draw with unknown shader ${d.shaderId}`);
      return;
    }
    /* Bindings. */
    let at = DRAW_BYTES;
    let data: Uint8Array | null = null;
    const textureIds: number[] = [];
    const filtered: boolean[] = [];
    const samplerStates = new Map<number, number>();
    for (let i = 0; i < d.bindingCount; i++) {
      const kind = v.getUint32(at, true);
      const binding = v.getUint32(at + 4, true);
      const bytes = v.getUint32(at + 8, true);
      const texture = v.getUint32(at + 12, true);
      at += BINDING_BYTES;
      if (kind === BIND_DATA) {
        data = payload.subarray(at, at + bytes);
        at += bytes;
      } else if (kind === BIND_TEXTURE) {
        textureIds.push(texture);
        filtered.push((bytes & BIND_FILTERED) !== 0);
      } else if (kind === BIND_SAMPLER) {
        samplerStates.set(binding, texture);
      }
    }
    const stride = VERTEX_HEADER_BYTES + 16 * d.varyingCount;
    const vertices = payload.subarray(at, at + stride * d.vertexCount);
    const colorIds = d.targets.map((t) => (t.id && this.textures.get(t.id)?.renderView ? t.id : 0));
    const depthTex = d.depthId && !(globalThis as { NO_DEPTH?: boolean }).NO_DEPTH ? this.textures.get(d.depthId) : undefined;
    const depthId = depthTex?.renderView ? d.depthId : 0;
    /* Textures; one that is also an attachment is sampled from a copy. */
    const attached = new Set<number>([...colorIds, depthId].filter((id) => id !== 0));
    const textures: Tex[] = [];
    for (const id of textureIds) {
      const t = this.textures.get(id);
      if (!t) {
        this.warnOnce(`texture ${id}`, `draw samples unknown texture ${id}`);
        return;
      }
      if (this.mipsStale.has(t.texture)) this.buildMips(t);
      if (!attached.has(id)) this.materialize(id);
      textures.push(attached.has(id) ? this.shadowOf(id, t) : t);
    }
    const colorFormats = colorIds.map((id) => (id ? this.textures.get(id)?.format ?? null : null));
    const { pipeline, layout } = this.drawPipeline(d, module, textures, filtered, colorFormats, depthId ? depthTex : undefined);
    /* Vertices and data (a flush may happen in between: stage both first). */
    if (this.vertexUsed + vertices.byteLength > VERTEX_BUFFER_BYTES ||
        Math.ceil(this.dataUsed / DATA_ALIGN) * DATA_ALIGN + Math.max(data?.byteLength ?? 0, DATA_WINDOW_BYTES) > DATA_BUFFER_BYTES) {
      this.flush();
    }
    const dataOffset = this.stageData(data ?? new Uint8Array(16), DATA_WINDOW_BYTES);
    const vertexOffset = this.vertexUsed;
    this.vertexStaging.set(vertices, vertexOffset);
    this.vertexUsed += Math.ceil(vertices.byteLength / 16) * 16;
    const pass = this.passFor(colorIds, depthId);
    if (!pass) return;
    if (!this.scissor(pass, d.scissor)) return;
    const samplers = [...samplerStates].map(([binding, state]) => ({ binding, resource: this.sampler(state) }));
    const key = [this.idOf(layout), this.set, ...textures.map((t) => this.idOf(t.sampleView)),
      ...samplers.map((e) => `${e.binding}:${this.idOf(e.resource)}`)].join(",");
    let group = this.bindGroups.get(key);
    if (!group) {
      if (this.bindGroups.size >= BIND_GROUP_CACHE_LIMIT) this.bindGroups.clear();
      group = this.device.createBindGroup({
        layout,
        entries: [
          { binding: 0, resource: { buffer: this.dataBuffer, offset: 0, size: DATA_WINDOW_BYTES } },
          ...textures.map((t, i) => ({ binding: 1 + i, resource: t.sampleView })),
          ...samplers,
        ],
      });
      this.bindGroups.set(key, group);
    }
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, group, [dataOffset]);
    pass.setVertexBuffer(0, this.vertexBuffer, vertexOffset, vertices.byteLength);
    pass.setBlendConstant({ r: d.blendConstant[0] ?? 0, g: d.blendConstant[1] ?? 0, b: d.blendConstant[2] ?? 0, a: d.blendConstant[3] ?? 0 });
    pass.setStencilReference(d.stencilRef);
    pass.draw(d.vertexCount);
    this.stats.draws++;
  }

  private clear(c: Clear): void {
    if (c.flags & CLEAR_COLOR) this.clearColor(c);
    if (c.flags & (CLEAR_DEPTH | CLEAR_STENCIL)) this.clearDepth(c);
  }

  private clearColor(c: Clear): void {
    const t = this.textures.get(c.colorId);
    if (!t || !t.renderView) return;
    const [x, y, w, h] = [c.rect[0] ?? 0, c.rect[1] ?? 0, c.rect[2] ?? 0, c.rect[3] ?? 0];
    const full = x <= 0 && y <= 0 && x + w >= t.width && y + h >= t.height && c.colorMask === 0xf;
    const type = shaderType(t.format);
    if (full) {
      const value = c.color.map((bits) => {
        if (type === "u32") return bits >>> 0;
        if (type === "i32") return bits | 0;
        return new DataView(new Uint32Array([bits]).buffer).getFloat32(0, true);
      });
      if (this.attachedNow(c.colorId)) this.endPass();
      this.pendingColor.set(c.colorId, { r: value[0] ?? 0, g: value[1] ?? 0, b: value[2] ?? 0, a: value[3] ?? 0 });
      return;
    }
    this.materialize(c.colorId);
    const pass = this.passFor([c.colorId], 0);
    if (!pass || !this.scissor(pass, c.rect)) return;
    const { pipeline, layout } = this.simplePipeline(`clear-${type}`, clearShader(type), t.format, c.colorMask, null,
      [{ binding: 0, visibility: GPUShaderStage.FRAGMENT, buffer: { type: "read-only-storage" } }]);
    const offset = this.stageWords(c.color);
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, this.device.createBindGroup({ layout, entries: [{ binding: 0, resource: { buffer: this.dataBuffer, offset, size: 16 } }] }));
    pass.draw(3);
  }

  private clearDepth(c: Clear): void {
    const t = this.textures.get(c.depthId);
    if (!t || !t.renderView) return;
    const [x, y, w, h] = [c.rect[0] ?? 0, c.rect[1] ?? 0, c.rect[2] ?? 0, c.rect[3] ?? 0];
    const whole = x <= 0 && y <= 0 && x + w >= t.width && y + h >= t.height;
    const depth = (c.flags & CLEAR_DEPTH) && hasDepth(t.format) ? c.depth : null;
    const stencil = (c.flags & CLEAR_STENCIL) && hasStencil(t.format) ? c.stencil : null;
    if (whole && (stencil === null || (c.stencilMask & 0xff) === 0xff)) {
      if (this.attachedNow(c.depthId)) this.endPass();
      const prev = this.pendingDepth.get(c.depthId);
      this.pendingDepth.set(c.depthId, { depth: depth ?? prev?.depth ?? null, stencil: stencil ?? prev?.stencil ?? null });
      return;
    }
    this.materialize(c.depthId);
    const pass = this.passFor([], c.depthId);
    if (!pass || !this.scissor(pass, c.rect)) return;
    const ds: GPUDepthStencilState = { format: t.format };
    if (hasDepth(t.format)) {
      ds.depthWriteEnabled = depth !== null;
      ds.depthCompare = "always";
    }
    if (stencil !== null) {
      const face: GPUStencilFaceState = { compare: "always", passOp: "replace", failOp: "keep", depthFailOp: "keep" };
      ds.stencilFront = face;
      ds.stencilBack = face;
      ds.stencilWriteMask = c.stencilMask;
    }
    const { pipeline, layout } = this.simplePipeline("clear-depth", DEPTH_CLEAR_SHADER, null, 0, ds,
      [{ binding: 0, visibility: GPUShaderStage.FRAGMENT, buffer: { type: "read-only-storage" } }]);
    const offset = this.stageWords([f32Bits(depth ?? 0)]);
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, this.device.createBindGroup({ layout, entries: [{ binding: 0, resource: { buffer: this.dataBuffer, offset, size: 16 } }] }));
    if (stencil !== null) pass.setStencilReference(stencil);
    pass.draw(3);
  }

  /** Draws `src`'s rectangle into a pass already targeting the destination. */
  private blit(pass: GPURenderPassEncoder, src: Tex, dstFormat: GPUTextureFormat, srcRect: readonly number[],
               dstRect: readonly number[], flags: number): void {
    const canFilter = filterable(src.format);
    const entries: GPUBindGroupLayoutEntry[] = [
      { binding: 0, visibility: GPUShaderStage.FRAGMENT, buffer: { type: "read-only-storage" } },
      { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: canFilter ? "float" : "unfilterable-float", viewDimension: "2d-array" } },
    ];
    if (canFilter) entries.push({ binding: 2, visibility: GPUShaderStage.FRAGMENT, sampler: { type: "filtering" } });
    const { pipeline, layout } = this.simplePipeline(`blit-${canFilter}`, blitShader(canFilter), dstFormat, 0xf, null, entries);
    let f = flags;
    if (!canFilter) f &= ~4;
    if (isSrgb(src.format) && !isSrgb(dstFormat)) f |= 8;
    if (!isSrgb(src.format) && isSrgb(dstFormat)) f |= 16;
    const words = [...srcRect.map(f32Bits), ...dstRect.map(f32Bits), f];
    const offset = this.stageWords(words);
    const bind: GPUBindGroupEntry[] = [
      { binding: 0, resource: { buffer: this.dataBuffer, offset, size: Math.max(16, words.length * 4) } },
      { binding: 1, resource: src.sampleView },
    ];
    if (canFilter) bind.push({ binding: 2, resource: this.linearSampler });
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, this.device.createBindGroup({ layout, entries: bind }));
    pass.draw(3);
  }

  private copy(c: Copy): void {
    let src = this.textures.get(c.srcId);
    const dst = this.textures.get(c.dstId);
    if (!src || !dst || !dst.renderView || isDepthFormat(src.format) || shaderType(dst.format) !== shaderType(src.format)) {
      this.warnOnce(`copy ${src?.format}->${dst?.format}`, `GPU copy ${src?.format ?? "?"} -> ${dst?.format ?? "?"} not supported`);
      return;
    }
    this.materialize(c.srcId);
    const [sx, sy, sw, sh] = [c.srcRect[0] ?? 0, c.srcRect[1] ?? 0, c.srcRect[2] ?? 0, c.srcRect[3] ?? 0];
    const [dx, dy, dw, dh] = [c.dstRect[0] ?? 0, c.dstRect[1] ?? 0, c.dstRect[2] ?? 0, c.dstRect[3] ?? 0];
    if (c.srcId !== c.dstId && src.format === dst.format && sw === dw && sh === dh && sw > 0 && sh > 0 && sx >= 0 &&
        sy >= 0 && dx >= 0 && dy >= 0 && sx + sw <= src.width && sy + sh <= src.height && dx + dw <= dst.width &&
        dy + dh <= dst.height) {
      this.materialize(c.dstId);
      this.endPass();
      this.ensureEncoder().copyTextureToTexture({ texture: src.texture, origin: { x: sx, y: sy } },
        { texture: dst.texture, origin: { x: dx, y: dy } }, [sw, sh, 1]);
      this.stats.copies++;
      return;
    }
    if (c.srcId === c.dstId) src = this.shadowOf(c.srcId, src);
    const pass = this.passFor([c.dstId], 0);
    const type = shaderType(src.format);
    if (type !== "f32") {
      if (!pass || !this.scissor(pass, c.dstRect)) return;
      const entries: GPUBindGroupLayoutEntry[] = [
        { binding: 0, visibility: GPUShaderStage.FRAGMENT, buffer: { type: "read-only-storage" } },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: type === "u32" ? "uint" : "sint", viewDimension: "2d-array" } },
      ];
      const { pipeline, layout } = this.simplePipeline(`blit-${type}`, intBlitShader(type), dst.format, 0xf, null, entries);
      const words = [...c.srcRect.map(f32Bits), ...c.dstRect.map(f32Bits)];
      const offset = this.stageWords(words);
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, this.device.createBindGroup({ layout, entries: [
        { binding: 0, resource: { buffer: this.dataBuffer, offset, size: words.length * 4 } },
        { binding: 1, resource: src.sampleView },
      ] }));
      pass.draw(3);
      return;
    }
    if (!pass || !this.scissor(pass, c.dstRect)) return;
    this.blit(pass, src, dst.format, c.srcRect, c.dstRect, c.filter ? 4 : 0);
  }

  private present(p: Present): void {
    const src = this.textures.get(p.id);
    const width = p.rect[2] ?? 0, height = p.rect[3] ?? 0;
    if (!src || width <= 0 || height <= 0) return;
    this.materialize(p.id);
    const target = this.host.presentTarget(width, height);
    if (!target) return;
    this.beginPass("present", [{ view: target.view, loadOp: "clear", storeOp: "store", clearValue: { r: 0, g: 0, b: 0, a: 1 } }],
      null, [target.width, target.height]);
    const pass = this.pass;
    if (!pass) return;
    const flags = ((p.flags & PRESENT_FLIP_X) ? 1 : 0) | ((p.flags & PRESENT_FLIP_Y) ? 2 : 0);
    this.blit(pass, src, target.format, p.rect, [0, 0, target.width, target.height], flags);
    this.flush();
    this.stats.presents++;
    this.host.presented(width, height);
  }
}
