/**
 * Persistent shader / pipeline cache (§13, DESIGN.md "pipeline cache"):
 * every WGSL module and draw-pipeline description a title used is kept in
 * OPFS, per title, so the next session builds them in the background
 * (createRenderPipelineAsync) before the game asks for them instead of
 * stalling the draw that first needs one.
 *
 * Shader ids in the GPU stream are per-session counters, so everything is
 * keyed by a hash of the WGSL text; a translator change simply produces new
 * hashes (old entries age out). A pipeline spec holds exactly what the
 * executor derives a GPURenderPipelineDescriptor from.
 */

export const SHADER_CACHE_VERSION = 1;
const CACHE_DIRECTORY = "shader-cache";
/* Bounds per title: the newest entries win. */
const MAX_SHADERS = 4096;
const MAX_PIPELINES = 8192;
const FLUSH_DELAY_MS = 5000;

const FNV32_PRIME = 0x01000193;
const FNV32_OFFSET_A = 0x811c9dc5;
const FNV32_OFFSET_B = 0x050c5d1f; /* a second lane with a different basis */

/** A 64-bit content hash of `text` (two FNV-1a 32 lanes over its UTF-16
 * code units), as 16 hex digits. Pure. */
export function wgslHash(text: string): string {
  let a = FNV32_OFFSET_A, b = FNV32_OFFSET_B;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    a = Math.imul(a ^ c, FNV32_PRIME);
    b = Math.imul(b ^ c ^ (i & 0xff), FNV32_PRIME);
  }
  return (a >>> 0).toString(16).padStart(8, "0") + (b >>> 0).toString(16).padStart(8, "0");
}

export interface TargetSpec {
  readonly format: string | null;
  readonly writeMask: number;
  /** color op/src/dst, alpha op/src/dst; null = no blending. */
  readonly blend: readonly number[] | null;
}

export interface StencilSpec {
  readonly front: readonly number[]; /* compare, fail, depthFail, pass */
  readonly back: readonly number[];
  readonly readMask: number;
  readonly writeMask: number;
}

export interface DepthSpec {
  readonly format: string;
  readonly test: boolean;
  readonly write: boolean;
  readonly compare: number;
  readonly stencil: StencilSpec | null;
}

/** Everything a draw pipeline is built from. */
export interface PipelineSpec {
  readonly wgsl: string;
  readonly varyings: number;
  /** Per texture binding: "F" (filtered, float) or its sample type. */
  readonly textures: readonly string[];
  readonly targets: readonly TargetSpec[];
  readonly depth: DepthSpec | null;
}

/** The in-memory and on-disk identity of a spec. Pure. */
export function specKey(spec: PipelineSpec): string {
  return JSON.stringify(spec);
}

function isSpec(value: unknown): value is PipelineSpec {
  if (typeof value !== "object" || value === null) return false;
  const v = value as Record<string, unknown>;
  return typeof v["wgsl"] === "string" && typeof v["varyings"] === "number" &&
    Array.isArray(v["textures"]) && Array.isArray(v["targets"]);
}

export interface CacheContents {
  readonly shaders: ReadonlyMap<string, string>;
  readonly pipelines: readonly PipelineSpec[];
}

/** Parses a stored cache file; damaged or other-version files are empty. Pure. */
export function parseCache(text: string): CacheContents {
  try {
    const raw: unknown = JSON.parse(text);
    if (typeof raw !== "object" || raw === null) return { shaders: new Map(), pipelines: [] };
    const r = raw as Record<string, unknown>;
    if (r["version"] !== SHADER_CACHE_VERSION) return { shaders: new Map(), pipelines: [] };
    const shaders = new Map<string, string>();
    const s = r["shaders"];
    if (typeof s === "object" && s !== null) {
      for (const [hash, code] of Object.entries(s)) if (typeof code === "string") shaders.set(hash, code);
    }
    const p = r["pipelines"];
    const pipelines = Array.isArray(p) ? p.filter(isSpec).filter((spec) => shaders.has(spec.wgsl)) : [];
    return { shaders, pipelines };
  } catch {
    return { shaders: new Map(), pipelines: [] };
  }
}

/** The file text for `contents`, keeping the newest entries within the
 * bounds (Map/array order is insertion order). Pure. */
export function serializeCache(contents: CacheContents): string {
  const shaders = [...contents.shaders].slice(-MAX_SHADERS);
  const kept = new Set(shaders.map(([hash]) => hash));
  const pipelines = contents.pipelines.filter((spec) => kept.has(spec.wgsl)).slice(-MAX_PIPELINES);
  return JSON.stringify({ version: SHADER_CACHE_VERSION, shaders: Object.fromEntries(shaders), pipelines });
}

/** A title's cache file in OPFS; additions are written back in batches. */
export class ShaderCacheStore {
  private shaders = new Map<string, string>();
  private pipelines = new Map<string, PipelineSpec>();
  private dirty = false;
  private timer: ReturnType<typeof setTimeout> | null = null;

  private readonly fileName: string;

  private constructor(fileName: string) {
    this.fileName = fileName;
  }

  /** Opens (and reads) the cache of `titleId`; null without OPFS. */
  static async open(titleId: string): Promise<{ store: ShaderCacheStore; contents: CacheContents } | null> {
    if (!/^[0-9A-F]{16}$/i.test(titleId)) return null;
    const store = new ShaderCacheStore(`${titleId.toUpperCase()}.json`);
    const dir = await store.directory();
    if (!dir) return null;
    let contents: CacheContents = { shaders: new Map(), pipelines: [] };
    try {
      const file = await (await dir.getFileHandle(store.fileName)).getFile();
      contents = parseCache(await file.text());
    } catch {
      /* no cache yet */
    }
    store.shaders = new Map(contents.shaders);
    store.pipelines = new Map(contents.pipelines.map((spec) => [specKey(spec), spec]));
    return { store, contents };
  }

  private async directory(): Promise<FileSystemDirectoryHandle | null> {
    if (typeof navigator === "undefined" || !navigator.storage?.getDirectory) return null;
    try {
      const root = await navigator.storage.getDirectory();
      return await root.getDirectoryHandle(CACHE_DIRECTORY, { create: true });
    } catch {
      return null;
    }
  }

  recordShader(hash: string, code: string): void {
    if (this.shaders.has(hash)) return;
    this.shaders.set(hash, code);
    this.markDirty();
  }

  recordPipeline(spec: PipelineSpec): void {
    const key = specKey(spec);
    if (this.pipelines.has(key)) return;
    this.pipelines.set(key, spec);
    this.markDirty();
  }

  get size(): { readonly shaders: number; readonly pipelines: number } {
    return { shaders: this.shaders.size, pipelines: this.pipelines.size };
  }

  private markDirty(): void {
    this.dirty = true;
    if (this.timer === null) this.timer = setTimeout(() => void this.flush(), FLUSH_DELAY_MS);
  }

  async flush(): Promise<void> {
    this.timer = null;
    if (!this.dirty) return;
    this.dirty = false;
    const dir = await this.directory();
    if (!dir) return;
    try {
      const writable = await (await dir.getFileHandle(this.fileName, { create: true })).createWritable();
      await writable.write(serializeCache({ shaders: this.shaders, pipelines: [...this.pipelines.values()] }));
      await writable.close();
    } catch {
      this.dirty = true; /* try again with the next addition */
    }
  }
}
