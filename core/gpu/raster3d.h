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

#include "gpu/maxwell_shader.h"
#include "gpu/texture.h"

struct Gpu_Memory;

#define RASTER_BIND_GROUPS 5u
#define RASTER_SURFACES 4u
#define RASTER_MAX_SURFACE_BYTES ((size_t)2048 * 1280 * 4)
#define RASTER_PROGRAMS 16u
#define RASTER_TEXTURES 64u
#define RASTER_TEXTURE_POOL_BYTES ((size_t)32 * 1024 * 1024)
#define RASTER_INLINE_INDICES 0x10000u
#define RASTER_STREAMS 32u
#define RASTER_STREAM_WINDOW 0x1000u
#define RASTER_PROGRAM_READ_BYTES (SM_SPH_BYTES + SM_MAX_WORDS * 8u)

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
  uint32_t validated;    /* submission it was last checked against guest memory */
  uint64_t address;      /* guest bytes it was decoded from */
  uint64_t raw_bytes;
  Tex_Image image;
} Raster3d_Texture;

typedef struct Raster3d_Stats {
  uint64_t draws;
  uint64_t clears;
  uint64_t triangles;
  uint64_t pixels;
  uint64_t skipped_draws;   /* no programs / no targets / unsupported */
  uint64_t shader_faults;
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
  uint32_t texture_count;
  uint8_t *texture_pool;    /* RASTER_TEXTURE_POOL_BYTES */
  size_t texture_pool_used;
  uint8_t *cbuf_data;       /* 2 stages x SM_CBUF_SLOTS x 64 KiB */
  Sm_Thread *thread;        /* one invocation's state */
  Raster3d_Stats stats;
} Raster3d;

/* Bytes of backing storage raster3d_init needs (one allocation). */
size_t raster3d_storage_bytes(void);
void raster3d_init(Raster3d *r, uint8_t *storage, size_t bytes);

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
void raster3d_draw(Raster3d *r, const uint32_t *regs, const Raster3d_Bindings *bindings,
                   const struct Gpu_Memory *mem, const Raster3d_Draw *draw);
/* Writes every dirty render target back to guest memory. */
void raster3d_flush(Raster3d *r, const struct Gpu_Memory *mem);

#endif /* SWITCH_GPU_RASTER3D_H */
