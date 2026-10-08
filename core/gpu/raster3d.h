/**
 * Maxwell 3D reference renderer (§13): executes CLEAR_SURFACE and draws
 * from the B197 register file in software, rendering straight into the
 * guest's render targets (block-linear or pitch, in guest memory), so
 * the existing vi/compositor present path shows the result and the CLI
 * can hash frames. Deviation from §13's "GPU worker translates to
 * WebGPU": this is the correctness oracle that path will be checked
 * against, and the path real homebrew uses today.
 *
 * Pipeline: index/vertex fetch (SET_VERTEX_ATTRIBUTE_A, vertex streams,
 * index buffer / vertex array / inline indices) -> vertex program
 * (maxwell_shader) -> clip (near/far and a wide guard band) -> viewport
 * transform -> half-space rasterizer (8 subpixel bits, top-left rule,
 * scissor / viewport clip / surface clip) -> pixel program -> depth test
 * -> blend (OGL and D3D factor/op enums) -> colour write mask -> render
 * target. Tessellation and geometry stages are not run; stencil is not
 * applied; textures sample level 0.
 *
 * Render targets live in a small cache of host-linear copies, loaded on
 * first use and written back by raster3d_flush (end of each submission,
 * and before anything else reads guest memory the GPU may have written:
 * DMA copies).
 */
#ifndef SWITCH_GPU_RASTER3D_H
#define SWITCH_GPU_RASTER3D_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/workers.h"
#include "gpu/compute.h"
#include "gpu/maxwell_shader.h"
#include "gpu/texture.h"

struct Gpu_Memory;
struct Gpu_Stream;

#define RASTER_BIND_GROUPS 5u
#define RASTER_SURFACES 12u
#define RASTER_MAX_SURFACE_BYTES ((size_t)2048 * 1280 * 4)
#define RASTER_PROGRAMS 64u
#define RASTER_TEXTURES 256u
#define RASTER_SURFACE_VIEWS 32u
/* Decoded textures. A commercial scene's working set reaches ~500MB
 * (Silksong's first room: 29 textures, one a 64MB 4096x4096 atlas) - a
 * smaller pool re-decodes it every frame. The web heap is what remains of
 * the fixed linear memory beside guest RAM (§4), so it gets the most that
 * leaves the rest headroom. */
#ifdef __EMSCRIPTEN__
/* Web: GPU mode (the default) frees each decoded copy once it is uploaded,
 * so the pool only holds the textures being decoded now (the largest is a
 * 64MB atlas); 256MB leaves the fixed heap room for the game's own
 * allocations (512MB ran Node builds out of heap at Silksong's thread
 * creation). The software renderer evicts more at this size. */
#define RASTER_TEXTURE_POOL_BYTES ((size_t)256 * 1024 * 1024)
#else
#define RASTER_TEXTURE_POOL_BYTES ((size_t)1024 * 1024 * 1024)
#endif
/* Texture change detection: a sampled hash each frame, the whole texture
 * every RASTER_TEXTURE_FULL_EVERY frames. */
#define RASTER_TEXTURE_SAMPLES 64u
#define RASTER_TEXTURE_SAMPLE_BYTES 64u
#define RASTER_TEXTURE_FULL_EVERY 30u
#define RASTER_TEXTURE_RAW_BYTES ((size_t)64 * 1024 * 1024) /* a 4096x4096 RGBA8 texture */
#define RASTER_INLINE_INDICES 0x10000u
#define RASTER_STREAMS 32u
#define RASTER_STREAM_WINDOW 0x1000u
#define RASTER_PROGRAM_READ_BYTES (SM_SPH_BYTES + SM_MAX_WORDS * 8u)
/* GPU mode (raster3d_set_gpu): render targets as GPU textures, shaders. */
#define RASTER_GPU_SURFACES 64u
#define RASTER_GPU_SHADERS 1024u

typedef struct Raster3d_Bindings {
  uint64_t address[RASTER_BIND_GROUPS][SM_CBUF_SLOTS];
  uint32_t size[RASTER_BIND_GROUPS][SM_CBUF_SLOTS];
} Raster3d_Bindings;

typedef struct Raster3d_Surface {
  bool in_use;
  bool loaded;      /* pixels hold the guest contents */
  bool dirty;       /* pixels differ from guest memory */
  bool depth;       /* a zeta surface */
  uint64_t address;
  uint32_t width, height;
  uint32_t format;
  uint32_t bytes_per_pixel;
  bool block_linear;
  uint32_t block_height_log2;
  uint32_t pitch;   /* guest row pitch (pitch layout) */
  uint64_t guest_bytes;
  uint32_t last_used;
  uint8_t *pixels;  /* width * bytes_per_pixel per row */
} Raster3d_Surface;

typedef struct Raster3d_Program {
  bool valid;
  uint32_t validated;  /* submission counter when last checked */
  uint32_t last_used;
  Sm_Program program;
} Raster3d_Program;

typedef struct Raster3d_Texture {
  bool valid;
  uint32_t tic[8];
  uint64_t raw_hash;     /* of the guest bytes it was decoded from */
  uint32_t validated;    /* epoch (frame) it was last checked against guest memory */
  uint32_t full_epoch;   /* epoch of its last whole-texture hash (between those, a sampled hash) */
  bool forced;           /* textures_invalidate asked for a whole hash */
  uint64_t sample_hash;  /* hash of RASTER_TEXTURE_SAMPLES spans of its guest bytes */
  uint64_t address;      /* guest bytes it was decoded from */
  uint64_t raw_bytes;
  size_t pool_offset;    /* its decoded texels' block in the texture pool */
  size_t pool_bytes;
  uint32_t last_used;    /* draw serial; the current draw's textures are never evicted */
  Tex_Image image;
  /* GPU mode: the GPU texture holding it (0 = none) and what was uploaded. */
  uint32_t gpu_id;
  uint64_t gpu_hash;
  uint32_t gpu_width, gpu_height, gpu_layers, gpu_format, gpu_levels;
} Raster3d_Texture;

/* GPU mode: a render target that lives only on the GPU (its guest memory
 * is not written: presents and copies of it become stream records). */
typedef struct Raster3d_Gpu_Surface {
  bool in_use;
  bool depth;
  bool stale;           /* guest memory was written by someone else: re-upload before use */
  uint32_t id;
  uint64_t address;     /* GPU virtual address */
  uint64_t cpu_address; /* the guest address it maps to (presents name buffers by it) */
  uint32_t width, height;
  uint32_t format;      /* the render-target (or zeta) format */
  uint32_t gpu_format;  /* GPU_FMT_* */
  uint32_t bytes_per_pixel;
  bool block_linear;
  uint32_t block_height_log2;
  uint32_t pitch;
  uint64_t guest_bytes;
  uint32_t last_used;
} Raster3d_Gpu_Surface;

typedef struct Raster3d_Gpu_Shader {
  uint64_t key;         /* wgsl_desc_hash */
  uint32_t id;          /* 0: free; UINT32_MAX: did not translate */
} Raster3d_Gpu_Shader;

typedef struct Raster3d_Gpu_Stats {
  uint64_t draws;
  uint64_t triangles;
  uint64_t shaders;
  uint64_t untranslated_draws;
  uint64_t texture_uploads;
  uint64_t upload_bytes;
  uint64_t surfaces;
  uint64_t presents;
  uint64_t copies;
  uint64_t hashed_bytes; /* guest texture bytes re-hashed to detect changes */
  /* ...by reason: first sight (or evicted), too small or unreadable to
   * sample, the sampled hash changed, the periodic whole hash, forced by
   * a write over it (textures_invalidate); and the whole hashes done. */
  uint64_t hashed_new, hashed_unsampled, hashed_changed, hashed_periodic, hashed_forced;
  uint64_t full_hashes;
  /* Draw record bytes by part: per-draw data (constants and constant
   * buffers), pulled vertex streams, vertices, indices. */
  uint64_t draw_data_bytes, pulled_bytes, vertex_bytes, index_bytes;
} Raster3d_Gpu_Stats;

typedef struct Raster3d_Stats {
  uint64_t draws;
  uint64_t clears;
  uint64_t triangles;
  uint64_t pixels;
  uint64_t skipped_draws;   /* no programs / no targets / unsupported */
  uint64_t shader_faults;
  uint64_t compute_dispatches; /* compute launches run (raster3d_compute) */
  uint64_t compute_threads;
  uint64_t compute_faults;
  uint64_t unknown_ops;     /* undecodable words seen in loaded programs */
  uint64_t texture_misses;  /* pool full / unreadable */
} Raster3d_Stats;

typedef struct Raster3d {
  bool ready;
  uint32_t submission;
  uint32_t tick;
  Raster3d_Surface surfaces[RASTER_SURFACES];
  uint8_t *staging;         /* RASTER_MAX_SURFACE_BYTES, guest-layout copy */
  Raster3d_Program *programs; /* RASTER_PROGRAMS */
  uint8_t *program_bytes;   /* RASTER_PROGRAM_READ_BYTES */
  Raster3d_Texture textures[RASTER_TEXTURES];
  /* Render targets sampled in place: built once per (surface, descriptor)
   * per draw and never rewritten while the draw's workers may read them. */
  Raster3d_Texture surface_views[RASTER_SURFACE_VIEWS];
  uint32_t surface_view_count;
  uint8_t *texture_pool;    /* RASTER_TEXTURE_POOL_BYTES: decoded texels, LRU-evicted blocks */
  uint8_t *texture_raw;     /* RASTER_TEXTURE_RAW_BYTES: one texture's guest bytes while it decodes */
  uint32_t draw_serial;
  uint32_t texture_epoch;    /* decoded textures re-validate against guest memory once per epoch (frame) */
  uint8_t *cbuf_data;       /* 2 stages x SM_CBUF_SLOTS x 64 KiB */
  Sm_Thread *thread;        /* one invocation's state (vertex work; band 0) */
  /* Pixel work runs in parallel over interleaved row bands (§13): one
   * shader state per worker; band_threads[0] is `thread`. */
  Workers workers;
  Sm_Thread *band_threads[WORKERS_MAX];
  Raster3d_Stats stats;
  bool trace_draws;          /* log every draw's state and result (diagnostics) */
  bool skip_draws;           /* frame skip: draws and clears are not rasterised */
  /* Diagnostics: called with every newly decoded texture (may be NULL). */
  void (*on_texture_decoded)(void *user, const Tex_Image *image, uint64_t address);
  void *on_texture_user;
  /* Diagnostics: called with every newly decoded shader program (may be NULL). */
  void (*on_program_decoded)(void *user, const Sm_Program *program);
  void *on_program_user;
  /* Diagnostics: called at every frame end (raster3d_end_frame), before
   * the epoch advances (may be NULL). */
  void (*on_frame_end)(void *user, const struct Raster3d *r);
  void *on_frame_end_user;
  /* GPU mode (§13 WebGPU renderer): set by raster3d_set_gpu. */
  struct Gpu_Stream *gpu;
  uint32_t gpu_next_id;
  Raster3d_Gpu_Surface gpu_surfaces[RASTER_GPU_SURFACES];
  Raster3d_Gpu_Shader *gpu_shaders;  /* RASTER_GPU_SHADERS, open addressing */
  char *gpu_wgsl;                    /* translation buffer */
  uint8_t *gpu_vertices;             /* the current draw's vertices */
  uint32_t *gpu_data;                /* the current draw's constants + constant buffers */
  Raster3d_Gpu_Stats gpu_stats;
  /* GPU mode: mipmapped textures get mip chains and pixel programs pick
   * the level from derivatives (a deliberate improvement over the
   * reference's level 0, §13). Off: level 0 only, as the reference. */
  bool gpu_mipmaps;
  /* GPU mode: vertex programs run on the CPU for every draw (the path
   * before GPU vertex stages; diagnostics and comparison). */
  bool cpu_vertices;
  bool no_vertex_pull; /* GPU vertex stages take decoded inputs (the CPU fetches them) */
} Raster3d;

/* Bytes of backing storage raster3d_init needs (one allocation). */
size_t raster3d_storage_bytes(void);
void raster3d_init(Raster3d *r, uint8_t *storage, size_t bytes);

/* Sets how many workers shade pixels (1 = serial; clamped to the host's
 * cores and WORKERS_MAX). raster3d_init starts workers_default_count().
 * Output is identical for every count. */
void raster3d_set_workers(Raster3d *r, uint32_t count);

/* After fork(): the parent's helper threads do not exist in this process;
 * forget them (without joining) and start new ones. Diagnostics only. */
void raster3d_restart_workers_after_fork(Raster3d *r);

/* Joins the workers (emulator tear-down). */
void raster3d_shutdown(Raster3d *r);

/* Per-draw inputs the register file does not hold. */
typedef enum Raster3d_Draw_Kind {
  RASTER_DRAW_ARRAYS,   /* first, count */
  RASTER_DRAW_INDEXED,  /* first (index buffer element), count, index_size */
  RASTER_DRAW_INLINE,   /* inline indices */
} Raster3d_Draw_Kind;

typedef struct Raster3d_Draw {
  Raster3d_Draw_Kind kind;
  uint32_t topology;
  uint32_t first;
  uint32_t count;
  uint32_t index_size;       /* bytes: 1, 2, 4 */
  uint32_t instance;
  const uint32_t *inline_indices;
} Raster3d_Draw;

void raster3d_begin_submission(Raster3d *r);
void raster3d_clear(Raster3d *r, const uint32_t *regs, const struct Gpu_Memory *mem, uint32_t clear);
/* A compute dispatch (gpu/compute.h) on the interpreter: every block of
 * the grid, its groups interleaved at barriers, global memory through
 * `mem`. `regs` is the compute class's register file (program region,
 * texture pools, bindless slot). */
void raster3d_compute(Raster3d *r, const Compute_Launch *launch, const uint32_t *regs, const struct Gpu_Memory *mem);
void raster3d_draw(Raster3d *r, const uint32_t *regs, const Raster3d_Bindings *bindings,
                   const struct Gpu_Memory *mem, const Raster3d_Draw *draw);
/* A copy engine is about to read (write = false) or overwrite (write =
 * true) guest GPU memory [address, address + bytes): cached render targets
 * over it are written back, and when it is written, they reload and
 * decoded textures over it re-validate at their next use. */
void raster3d_sync_range(Raster3d *r, const struct Gpu_Memory *mem, uint64_t address, uint64_t bytes, bool write);

/* The guest presented a frame: decoded textures re-validate (re-read and
 * hash their guest bytes) at their first use after it - catching what the
 * CPU wrote. GPU-side writes (render targets, copy engines via
 * raster3d_sync_range) invalidate exactly what they touch at once. */
void raster3d_end_frame(Raster3d *r);

/* Writes every dirty render target back to guest memory (GPU mode:
 * publishes the stream). */
void raster3d_flush(Raster3d *r, const struct Gpu_Memory *mem);

/* GPU mode: draws, clears and copies become records on `stream` for the
 * GPU worker's WebGPU renderer instead of being rasterised here; vertex
 * work, assembly, clipping and culling stay on this side, pixel programs
 * are translated to WGSL (gpu/wgsl.h). NULL returns to software. */
void raster3d_set_gpu(Raster3d *r, struct Gpu_Stream *stream);

/* One vertex attribute decoded from its element's bytes (raw: the bytes
 * at the attribute's offset) under VERTEX_ATTRIB word `attrib`: the four
 * words a vertex program reads (float bits, or integers), defaults (0, 0,
 * 0, 1) where the format has fewer components. The vertex-pulling WGSL
 * (wgsl.c vfetch) must agree with it. */
#define RASTER_ATTRIBUTE_BYTES 16u
void raster3d_decode_attribute(uint32_t attrib, const uint8_t raw[RASTER_ATTRIBUTE_BYTES], uint32_t out[4]);

/* GPU mode: the guest presents the buffer at `address` (width x height,
 * QueueBuffer's crop rectangle x, y, w, h - w 0 for all - and
 * GPU_PRESENT_FLIP_* flags). True when a GPU surface holds it and a
 * PRESENT record went out; false: the caller presents guest memory. */
bool raster3d_gpu_present(Raster3d *r, uint64_t cpu_address, uint32_t width, uint32_t height, const int32_t crop[4],
                          uint32_t flags);

/* A surface a copy engine names (the 2D engine's source / destination). */
typedef struct Raster3d_Surface_Ref {
  uint64_t address;
  uint32_t width, height;
  uint32_t format;        /* render-target colour format */
  bool block_linear;
  uint32_t block_height_log2;
  uint32_t pitch;
} Raster3d_Surface_Ref;

/* GPU mode: a 2D-engine blit. True when the source is a GPU surface and a
 * COPY record went out (the destination becomes one); false: the caller
 * copies guest memory. Rectangles are x, y, w, h. */
bool raster3d_gpu_copy(Raster3d *r, const struct Gpu_Memory *mem, const Raster3d_Surface_Ref *src,
                       const Raster3d_Surface_Ref *dst, const int32_t src_rect[4], const int32_t dst_rect[4],
                       bool linear);

#endif /* SWITCH_GPU_RASTER3D_H */
