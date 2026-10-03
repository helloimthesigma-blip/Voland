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
 * queue orders that write after every earlier submission, so the buffers
 * are reused every batch. Anything that must happen on the queue timeline
 * between draws (texture uploads) flushes the batch first.
 */

import {
  BIND_DATA,
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

const VERTEX_BUFFER_BYTES = 64 * 1024 * 1024;
const DATA_BUFFER_BYTES = 32 * 1024 * 1024;
const DATA_ALIGN = 256;

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
  presents: number;
  errors: number;
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

function clearShader(type: "f32" | "u32" | "i32"): string {
  return `${FULLSCREEN_VS}
@group(0) @binding(0) var<storage, read> P: array<u32>;
@fragment fn fs() -> @location(0) vec4<${type}> { return bitcast<vec4<${type}>>(vec4<u32>(P[0], P[1], P[2], P[3])); }`;
}

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
  readonly stats: ExecutorStats = { draws: 0, passes: 0, submits: 0, pipelines: 0, presents: 0, errors: 0 };
  private readonly textures = new Map<number, Tex>();
  private readonly shaders = new Map<number, GPUShaderModule>();
  private readonly pipelines = new Map<string, CachedPipeline>();
  private readonly shadows = new Map<string, Tex>();
  private readonly vertexBuffer: GPUBuffer;
  private readonly dataBuffer: GPUBuffer;
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
  private readonly warned = new Set<string>();

  constructor(private readonly device: GPUDevice, private readonly host: ExecutorHost) {
    this.vertexBuffer = device.createBuffer({ size: VERTEX_BUFFER_BYTES, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
    this.dataBuffer = device.createBuffer({ size: DATA_BUFFER_BYTES, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
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
  flush(): void {
    this.endPass();
    if (!this.encoder) return;
    if (this.vertexUsed) this.device.queue.writeBuffer(this.vertexBuffer, 0, this.vertexStaging, 0, this.vertexUsed);
    if (this.dataUsed) this.device.queue.writeBuffer(this.dataBuffer, 0, this.dataStaging, 0, this.dataUsed);
    this.device.queue.submit([this.encoder.finish()]);
    this.stats.submits++;
    this.encoder = null;
    this.vertexUsed = 0;
    this.dataUsed = 0;
    for (const t of this.pendingDestroy) t.destroy();
    this.pendingDestroy = [];
  }

  private warnOnce(key: string, message: string): void {
    if (this.warned.has(key)) return;
    this.warned.add(key);
    this.host.log("warn", message);
  }

  /* ---- resources ---- */

  private makeTex(format: GPUTextureFormat, width: number, height: number, layers: number, render: boolean,
                  fallback: boolean): Tex {
    const usage = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC |
      (render ? GPUTextureUsage.RENDER_ATTACHMENT : 0);
    const texture = this.device.createTexture({ size: [width, height, layers], format, usage });
    const aspect: GPUTextureAspect = hasDepth(format) && hasStencil(format) ? "depth-only" : "all";
    return {
      texture, format, width, height, layers, sampleType: sampleTypeOf(format), fallback,
      sampleView: texture.createView({ dimension: "2d-array", aspect }),
      renderView: render ? texture.createView({ dimension: "2d", baseArrayLayer: 0, arrayLayerCount: 1 }) : null,
    };
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
    if (old) this.pendingDestroy.push(old.texture);
    this.textures.set(c.id, this.makeTex(format, Math.max(1, c.width), Math.max(1, c.height), Math.max(1, c.layers),
      (c.usage & USAGE_RENDER) !== 0, fallback));
  }

  private textureDestroy(id: number): void {
    const t = this.textures.get(id);
    if (!t) return;
    this.textures.delete(id);
    this.pendingDestroy.push(t.texture);
  }

  private textureWrite(v: DataView, payload: Uint8Array): void {
    const w = parseTextureWrite(v);
    const t = this.textures.get(w.id);
    if (!t || t.fallback || isDepthFormat(t.format)) return;
    this.flush(); /* earlier draws read the old contents */
    this.device.queue.writeTexture(
      { texture: t.texture, origin: { x: w.x, y: w.y, z: w.layer } },
      payload.subarray(TEXTURE_WRITE_BYTES, TEXTURE_WRITE_BYTES + w.dataBytes),
      { bytesPerRow: w.bytesPerRow, rowsPerImage: w.height },
      { width: w.width, height: w.height, depthOrArrayLayers: 1 },
    );
  }

  private shader(v: DataView, payload: Uint8Array): void {
    const id = v.getUint32(0, true);
    const bytes = v.getUint32(4, true);
    const code = new TextDecoder().decode(payload.slice(8, 8 + bytes));
    this.shaders.set(id, this.device.createShaderModule({ code }));
  }

  /* ---- passes ---- */

  private ensureEncoder(): GPUCommandEncoder {
    if (!this.encoder) this.encoder = this.device.createCommandEncoder();
    return this.encoder;
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
      return { view: t.renderView, loadOp: "load", storeOp: "store" };
    });
    const dt = depthId ? this.textures.get(depthId) : undefined;
    const depth = dt && dt.renderView ? this.depthAttachment(dt, null, null) : null;
    if (dt) size = [dt.width, dt.height];
    if (!size) return null;
    return this.beginPass(key, colors, depth, size);
  }

  private stageData(bytes: Uint8Array): number {
    let at = Math.ceil(this.dataUsed / DATA_ALIGN) * DATA_ALIGN;
    if (at + bytes.byteLength > DATA_BUFFER_BYTES) {
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

  private drawPipeline(d: Draw, module: GPUShaderModule, textures: readonly Tex[], colorFormats: readonly (GPUTextureFormat | null)[],
                       depth: Tex | undefined): CachedPipeline {
    const key = [
      "draw", d.shaderId, d.varyingCount,
      textures.map((t) => t.sampleType).join(","),
      d.targets.map((t, i) => `${colorFormats[i] ?? "-"}:${t.writeMask}:${t.blend ? `${t.colorOp}.${t.colorSrc}.${t.colorDst}.${t.alphaOp}.${t.alphaSrc}.${t.alphaDst}` : ""}`).join(","),
      depth ? `${depth.format}:${d.depthTest}:${d.depthWrite}:${d.depthCompare}:${d.stencil ? JSON.stringify([d.stencilFront, d.stencilBack, d.stencilReadMask, d.stencilWriteMask]) : ""}` : "",
    ].join("|");
    return this.cached(key, () => {
      const entries: GPUBindGroupLayoutEntry[] = [
        { binding: 0, visibility: GPUShaderStage.FRAGMENT, buffer: { type: "read-only-storage" } },
      ];
      textures.forEach((t, i) => entries.push({
        binding: 1 + i, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: t.sampleType, viewDimension: "2d-array" },
      }));
      const layout = this.device.createBindGroupLayout({ entries });
      const attributes: GPUVertexAttribute[] = [{ shaderLocation: 0, offset: 0, format: "float32x4" }];
      for (let i = 0; i < d.varyingCount; i++) attributes.push({ shaderLocation: i + 1, offset: VERTEX_HEADER_BYTES + 16 * i, format: "uint32x4" });
      const targets: (GPUColorTargetState | null)[] = d.targets.map((t, i) => {
        const format = colorFormats[i];
        if (!format) return null;
        const state: GPUColorTargetState = { format, writeMask: t.writeMask };
        if (t.blend && !format.endsWith("int")) {
          const minmax = (op: number): boolean => op === 3 || op === 4;
          state.blend = {
            color: {
              operation: BLEND_OPS[t.colorOp] ?? "add",
              srcFactor: minmax(t.colorOp) ? "one" : BLEND_FACTORS[t.colorSrc] ?? "one",
              dstFactor: minmax(t.colorOp) ? "one" : BLEND_FACTORS[t.colorDst] ?? "zero",
            },
            alpha: {
              operation: BLEND_OPS[t.alphaOp] ?? "add",
              srcFactor: minmax(t.alphaOp) ? "one" : BLEND_FACTORS[t.alphaSrc] ?? "one",
              dstFactor: minmax(t.alphaOp) ? "one" : BLEND_FACTORS[t.alphaDst] ?? "zero",
            },
          };
        }
        return state;
      });
      const desc: GPURenderPipelineDescriptor = {
        layout: this.device.createPipelineLayout({ bindGroupLayouts: [layout] }),
        vertex: { module, entryPoint: "vs", buffers: [{ arrayStride: VERTEX_HEADER_BYTES + 16 * d.varyingCount, attributes }] },
        fragment: { module, entryPoint: "fs", targets },
        primitive: { topology: "triangle-list", frontFace: "ccw", cullMode: "none" },
      };
      if (depth) {
        const ds: GPUDepthStencilState = { format: depth.format };
        if (hasDepth(depth.format)) {
          ds.depthWriteEnabled = d.depthWrite;
          ds.depthCompare = d.depthTest ? COMPARES[d.depthCompare] ?? "always" : "always";
        }
        if (d.stencil && hasStencil(depth.format)) {
          const face = (f: Draw["stencilFront"]): GPUStencilFaceState => ({
            compare: COMPARES[f.compare] ?? "always", failOp: STENCIL_OPS[f.fail] ?? "keep",
            depthFailOp: STENCIL_OPS[f.depthFail] ?? "keep", passOp: STENCIL_OPS[f.pass] ?? "keep",
          });
          ds.stencilFront = face(d.stencilFront);
          ds.stencilBack = face(d.stencilBack);
          ds.stencilReadMask = d.stencilReadMask;
          ds.stencilWriteMask = d.stencilWriteMask;
        }
        desc.depthStencil = ds;
      }
      return { pipeline: this.device.createRenderPipeline(desc), layout };
    });
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
  private shadowOf(t: Tex): Tex {
    const key = `${t.format}:${t.width}x${t.height}x${t.layers}`;
    let s = this.shadows.get(key);
    if (!s) {
      s = this.makeTex(t.format, t.width, t.height, t.layers, false, false);
      this.shadows.set(key, s);
    }
    this.endPass();
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
    for (let i = 0; i < d.bindingCount; i++) {
      const kind = v.getUint32(at, true);
      const bytes = v.getUint32(at + 8, true);
      const texture = v.getUint32(at + 12, true);
      at += BINDING_BYTES;
      if (kind === BIND_DATA) {
        data = payload.subarray(at, at + bytes);
        at += bytes;
      } else if (kind === BIND_TEXTURE) {
        textureIds.push(texture);
      }
    }
    const stride = VERTEX_HEADER_BYTES + 16 * d.varyingCount;
    const vertices = payload.subarray(at, at + stride * d.vertexCount);
    const colorIds = d.targets.map((t) => (t.id && this.textures.get(t.id)?.renderView ? t.id : 0));
    const depthTex = d.depthId ? this.textures.get(d.depthId) : undefined;
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
      textures.push(attached.has(id) ? this.shadowOf(t) : t);
    }
    const colorFormats = colorIds.map((id) => (id ? this.textures.get(id)?.format ?? null : null));
    const { pipeline, layout } = this.drawPipeline(d, module, textures, colorFormats, depthId ? depthTex : undefined);
    /* Vertices and data (a flush may happen in between: stage both first). */
    if (this.vertexUsed + vertices.byteLength > VERTEX_BUFFER_BYTES ||
        Math.ceil(this.dataUsed / DATA_ALIGN) * DATA_ALIGN + (data?.byteLength ?? 0) > DATA_BUFFER_BYTES) this.flush();
    const dataOffset = this.stageData(data ?? new Uint8Array(16));
    const dataSize = Math.max(16, data?.byteLength ?? 0);
    const vertexOffset = this.vertexUsed;
    this.vertexStaging.set(vertices, vertexOffset);
    this.vertexUsed += Math.ceil(vertices.byteLength / 16) * 16;
    const pass = this.passFor(colorIds, depthId);
    if (!pass) return;
    if (!this.scissor(pass, d.scissor)) return;
    const group = this.device.createBindGroup({
      layout,
      entries: [
        { binding: 0, resource: { buffer: this.dataBuffer, offset: dataOffset, size: dataSize } },
        ...textures.map((t, i) => ({ binding: 1 + i, resource: t.sampleView })),
      ],
    });
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, group);
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
      this.beginPass(`${c.colorId}|0`, [{
        view: t.renderView, loadOp: "clear", storeOp: "store",
        clearValue: { r: value[0] ?? 0, g: value[1] ?? 0, b: value[2] ?? 0, a: value[3] ?? 0 },
      }], null, [t.width, t.height]);
      return;
    }
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
      this.beginPass(`|${c.depthId}`, [], this.depthAttachment(t, depth, stencil), [t.width, t.height]);
      return;
    }
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
    if (!src || !dst || !dst.renderView || isDepthFormat(src.format) || shaderType(dst.format) !== "f32" ||
        shaderType(src.format) !== "f32") {
      this.warnOnce(`copy ${src?.format}->${dst?.format}`, `GPU copy ${src?.format ?? "?"} -> ${dst?.format ?? "?"} not supported`);
      return;
    }
    if (c.srcId === c.dstId) src = this.shadowOf(src);
    const pass = this.passFor([c.dstId], 0);
    if (!pass || !this.scissor(pass, c.dstRect)) return;
    this.blit(pass, src, dst.format, c.srcRect, c.dstRect, c.filter ? 4 : 0);
  }

  private present(p: Present): void {
    const src = this.textures.get(p.id);
    const width = p.rect[2] ?? 0, height = p.rect[3] ?? 0;
    if (!src || width <= 0 || height <= 0) return;
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
