/**
 * Mirror of core/gpu/gpu_stream.h and core/gpu/gpu_records.h: the GPU
 * stream's header, record types and payload layouts (all little-endian
 * u32 unless noted). Keep in step with the C headers (GPU_STREAM_VERSION).
 */

export const GPU_STREAM_MAGIC = 0x55504756;
export const GPU_STREAM_VERSION = 2;
export const GPU_STREAM_HEADER_BYTES = 56;
export const GPU_STREAM_RECORD_HEADER_BYTES = 8;
export const OFF_MAGIC = 0;
export const OFF_VERSION = 4;
export const OFF_RING_BASE = 8;
export const OFF_CAPACITY = 16;
export const OFF_WRITE = 24;
export const OFF_READ = 32;
export const OFF_WRITE_SIGNAL = 40;
export const OFF_READ_SIGNAL = 44;
export const OFF_PRESENTS = 48;

export const REC_PAD = 0;
export const REC_TEXTURE_CREATE = 1;
export const REC_TEXTURE_DESTROY = 2;
export const REC_TEXTURE_WRITE = 3;
export const REC_SHADER = 4;
export const REC_CLEAR = 5;
export const REC_DRAW = 6;
export const REC_COPY = 7;
export const REC_PRESENT = 8;

/* GPU_FMT_* by index. */
export const FORMATS: readonly (GPUTextureFormat | null)[] = [
  null,
  "rgba8unorm",
  "rgba8unorm-srgb",
  "bgra8unorm",
  "rgba16float",
  "rgba32float",
  "r8unorm",
  "rg8unorm",
  "r16float",
  "rg16float",
  "r32float",
  "rg32float",
  "rg11b10ufloat",
  "rgb10a2unorm",
  "r32uint",
  "rg32uint",
  "rgba32uint",
  "rgba16uint",
  "rgba8uint",
  "depth16unorm",
  "depth24plus-stencil8",
  "depth32float",
  "depth32float-stencil8",
  "bgra8unorm-srgb",
  "stencil8",
  "rgba16sint",
  "rgba32sint",
  "r32sint",
];

export const BLEND_FACTORS: readonly GPUBlendFactor[] = [
  "zero", "one", "src", "one-minus-src", "src-alpha", "one-minus-src-alpha", "dst", "one-minus-dst", "dst-alpha",
  "one-minus-dst-alpha", "src-alpha-saturated", "constant", "one-minus-constant",
];
export const BLEND_OPS: readonly GPUBlendOperation[] = ["add", "subtract", "reverse-subtract", "min", "max"];
export const COMPARES: readonly GPUCompareFunction[] = [
  "never", "less", "equal", "less-equal", "greater", "not-equal", "greater-equal", "always",
];
export const STENCIL_OPS: readonly GPUStencilOperation[] = [
  "keep", "zero", "replace", "invert", "increment-clamp", "decrement-clamp", "increment-wrap", "decrement-wrap",
];

export const USAGE_SAMPLED = 1;
export const USAGE_RENDER = 2;
export const CLEAR_COLOR = 1;
export const CLEAR_DEPTH = 2;
export const CLEAR_STENCIL = 4;
export const BIND_DATA = 1;
export const BIND_TEXTURE = 2;
export const BIND_SAMPLER = 3;
export const BIND_FILTERED = 1;
export const SAMPLER_LINEAR = 1;
export const SAMPLER_MIN_LINEAR = 1 << 7;
export const SAMPLER_MIP_LINEAR = 1 << 8;
export const SAMPLER_WRAPS: readonly GPUAddressMode[] = ["repeat", "mirror-repeat", "clamp-to-edge"];
export const PRESENT_FLIP_X = 1;
export const PRESENT_FLIP_Y = 2;
export const MAX_TARGETS = 8;
export const VERTEX_HEADER_BYTES = 16;

/* Byte sizes of the fixed parts. */
export const TEXTURE_CREATE_BYTES = 32; /* version 1: 24 (no levels) */
export const TEXTURE_WRITE_BYTES = 32;
export const CLEAR_BYTES = 60;
export const TARGET_BYTES = 36;
export const STENCIL_FACE_BYTES = 16;
export const DRAW_BYTES = 4 + 4 + TARGET_BYTES * MAX_TARGETS + 16 + 4 + STENCIL_FACE_BYTES * 2 + 12 + 16 + 16 + 16;
export const BINDING_BYTES = 16;
export const COPY_BYTES = 44;
export const PRESENT_BYTES = 24;

export interface TextureCreate {
  readonly id: number;
  readonly format: number;
  readonly width: number;
  readonly height: number;
  readonly layers: number;
  readonly usage: number;
  readonly levels: number;
}

export interface TextureWrite {
  readonly id: number;
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly height: number;
  readonly layer: number;
  readonly bytesPerRow: number;
  readonly dataBytes: number;
}

export interface Clear {
  readonly colorId: number;
  readonly colorMask: number;
  readonly color: readonly number[]; /* raw u32 */
  readonly depthId: number;
  readonly flags: number;
  readonly depth: number;            /* f32 */
  readonly stencil: number;
  readonly stencilMask: number;
  readonly rect: readonly number[];
}

export interface Target {
  readonly id: number;
  readonly writeMask: number;
  readonly blend: boolean;
  readonly colorOp: number;
  readonly colorSrc: number;
  readonly colorDst: number;
  readonly alphaOp: number;
  readonly alphaSrc: number;
  readonly alphaDst: number;
}

export interface StencilFace {
  readonly fail: number;
  readonly depthFail: number;
  readonly pass: number;
  readonly compare: number;
}

export interface Draw {
  readonly shaderId: number;
  readonly targets: readonly Target[];
  readonly depthId: number;
  readonly depthTest: boolean;
  readonly depthWrite: boolean;
  readonly depthCompare: number;
  readonly stencil: boolean;
  readonly stencilFront: StencilFace;
  readonly stencilBack: StencilFace;
  readonly stencilReadMask: number;
  readonly stencilWriteMask: number;
  readonly stencilRef: number;
  readonly blendConstant: readonly number[];
  readonly scissor: readonly number[];
  readonly varyingCount: number;
  readonly flatMask: number;
  readonly bindingCount: number;
  readonly vertexCount: number;
}

export interface Copy {
  readonly srcId: number;
  readonly dstId: number;
  readonly srcRect: readonly number[];
  readonly dstRect: readonly number[];
  readonly filter: number;
}

export interface Present {
  readonly id: number;
  readonly rect: readonly number[];
  readonly flags: number;
}

const u32 = (v: DataView, at: number): number => v.getUint32(at, true);
const i32 = (v: DataView, at: number): number => v.getInt32(at, true);
const u32s = (v: DataView, at: number, n: number): number[] => Array.from({ length: n }, (_, i) => u32(v, at + 4 * i));
const i32s = (v: DataView, at: number, n: number): number[] => Array.from({ length: n }, (_, i) => i32(v, at + 4 * i));

export function parseTextureCreate(v: DataView): TextureCreate {
  return {
    id: u32(v, 0), format: u32(v, 4), width: u32(v, 8), height: u32(v, 12), layers: u32(v, 16), usage: u32(v, 20),
    levels: v.byteLength >= 28 ? Math.max(1, u32(v, 24)) : 1,
  };
}

export function parseTextureWrite(v: DataView): TextureWrite {
  return {
    id: u32(v, 0), x: u32(v, 4), y: u32(v, 8), width: u32(v, 12), height: u32(v, 16), layer: u32(v, 20),
    bytesPerRow: u32(v, 24), dataBytes: u32(v, 28),
  };
}

export function parseClear(v: DataView): Clear {
  return {
    colorId: u32(v, 0), colorMask: u32(v, 4), color: u32s(v, 8, 4), depthId: u32(v, 24), flags: u32(v, 28),
    depth: v.getFloat32(32, true), stencil: u32(v, 36), stencilMask: u32(v, 40), rect: i32s(v, 44, 4),
  };
}

function parseStencilFace(v: DataView, at: number): StencilFace {
  return { fail: u32(v, at), depthFail: u32(v, at + 4), pass: u32(v, at + 8), compare: u32(v, at + 12) };
}

export function parseDraw(v: DataView): Draw {
  const targetCount = Math.min(u32(v, 4), MAX_TARGETS);
  const targets: Target[] = [];
  for (let i = 0; i < targetCount; i++) {
    const at = 8 + TARGET_BYTES * i;
    targets.push({
      id: u32(v, at), writeMask: u32(v, at + 4), blend: u32(v, at + 8) !== 0, colorOp: u32(v, at + 12),
      colorSrc: u32(v, at + 16), colorDst: u32(v, at + 20), alphaOp: u32(v, at + 24), alphaSrc: u32(v, at + 28),
      alphaDst: u32(v, at + 32),
    });
  }
  let at = 8 + TARGET_BYTES * MAX_TARGETS;
  const depthId = u32(v, at);
  const depthTest = u32(v, at + 4) !== 0;
  const depthWrite = u32(v, at + 8) !== 0;
  const depthCompare = u32(v, at + 12);
  const stencil = u32(v, at + 16) !== 0;
  at += 20;
  const stencilFront = parseStencilFace(v, at);
  const stencilBack = parseStencilFace(v, at + STENCIL_FACE_BYTES);
  at += 2 * STENCIL_FACE_BYTES;
  return {
    shaderId: u32(v, 0), targets, depthId, depthTest, depthWrite, depthCompare, stencil, stencilFront, stencilBack,
    stencilReadMask: u32(v, at), stencilWriteMask: u32(v, at + 4), stencilRef: u32(v, at + 8),
    blendConstant: Array.from({ length: 4 }, (_, i) => v.getFloat32(at + 12 + 4 * i, true)),
    scissor: i32s(v, at + 28, 4), varyingCount: u32(v, at + 44), flatMask: u32(v, at + 48),
    bindingCount: u32(v, at + 52), vertexCount: u32(v, at + 56),
  };
}

export function parseCopy(v: DataView): Copy {
  return { srcId: u32(v, 0), dstId: u32(v, 4), srcRect: i32s(v, 8, 4), dstRect: i32s(v, 24, 4), filter: u32(v, 40) };
}

export function parsePresent(v: DataView): Present {
  return { id: u32(v, 0), rect: i32s(v, 4, 4), flags: u32(v, 20) };
}
