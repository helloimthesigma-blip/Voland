/**
 * Maxwell 3D reference renderer. See raster3d.h.
 *
 * Register words are B197 method byte offsets / 4 (NVIDIA clb197.h).
 */
#include "gpu/raster3d.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "common/log.h"
#include "gpu/block_linear.h"
#include "gpu/gpu_channel.h"
#include "gpu/gpu_records.h"
#include "gpu/gpu_stream.h"
#include "gpu/wgsl.h"

/* ---- B197 register words ------------------------------------------ */

#define REG_RASTER_ENABLE 0x0dfu
#define REG_RT 0x200u               /* + 16 * target: A, B, WIDTH, HEIGHT, FORMAT, MEMORY */
#define REG_RT_STRIDE 16u
#define REG_VIEWPORT 0x280u         /* scale x/y/z, offset x/y/z */
#define REG_VIEWPORT_CLIP 0x300u    /* horizontal, vertical */
#define REG_CLEAR_RECT_H 0x35bu
#define REG_CLEAR_RECT_V 0x35cu
#define REG_VERTEX_ARRAY_START 0x35du
#define REG_Z_CLIP_RANGE 0x35fu
#define REG_CLEAR_COLOR 0x360u
#define REG_Z_CLEAR 0x364u
#define REG_STENCIL_CLEAR 0x368u
#define REG_BACK_STENCIL_REF 0x3d5u      /* ref, write mask, func mask */
#define REG_STENCIL_ENABLE 0x4e0u
#define REG_STENCIL_FRONT 0x4e1u         /* op fail, zfail, zpass, func, ref, func mask, write mask */
#define REG_STENCIL_TWO_SIDED 0x565u
#define REG_STENCIL_BACK 0x566u          /* op fail, zfail, zpass, func */
#define REG_SCISSOR 0x380u          /* enable, horizontal, vertical */
#define REG_SINGLE_CT_WRITE 0x3e4u
#define REG_CT_MRT_ENABLE 0x3ebu
#define REG_ZT_A 0x3f8u
#define REG_ZT_B 0x3f9u
#define REG_ZT_FORMAT 0x3fau
#define REG_ZT_BLOCK 0x3fbu
#define REG_SURFACE_CLIP_H 0x3fdu
#define REG_SURFACE_CLIP_V 0x3feu
#define REG_CLEAR_CONTROL 0x43eu
#define REG_VERTEX_ATTRIB 0x458u
#define REG_CT_SELECT 0x487u
#define REG_ZT_SIZE_A 0x48au
#define REG_ZT_SIZE_B 0x48bu
#define REG_SAMPLER_BINDING 0x48du
#define REG_DEPTH_TEST 0x4b3u
#define REG_BLEND_PER_TARGET 0x4b9u
#define REG_DEPTH_WRITE 0x4bau
#define REG_ALPHA_TEST 0x4bbu
#define REG_DEPTH_FUNC 0x4c3u
#define REG_ALPHA_REF 0x4c4u
#define REG_ALPHA_FUNC 0x4c5u
#define REG_BLEND_CONST 0x4c7u
#define REG_BLEND_SEPARATE 0x4cfu
#define REG_BLEND_COLOR_OP 0x4d0u
#define REG_BLEND_COLOR_SRC 0x4d1u
#define REG_BLEND_COLOR_DST 0x4d2u
#define REG_BLEND_ALPHA_OP 0x4d3u
#define REG_BLEND_ALPHA_SRC 0x4d4u
#define REG_BLEND_ALPHA_DST 0x4d6u
#define REG_BLEND_ENABLE 0x4d8u
#define REG_WINDOW_ORIGIN 0x4ebu
#define REG_BASE_VERTEX 0x50du
#define REG_BASE_INSTANCE 0x50eu
#define REG_POINT_SIZE 0x546u
#define REG_ZT_SELECT 0x54eu
#define REG_SAMPLER_POOL 0x557u
#define REG_TEX_HEADER_POOL 0x55du
#define REG_PROGRAM_REGION 0x582u
#define REG_PRIMITIVE_RESTART 0x591u
#define REG_PRIMITIVE_RESTART_INDEX 0x592u
#define REG_PROVOKING_VERTEX 0x5a1u
#define REG_INDEX_BUFFER 0x5f2u     /* A hi, B lo, C/D limit, E size, F first */
#define REG_STREAM_INSTANCED 0x620u
#define REG_CULL_ENABLE 0x646u
#define REG_FRONT_FACE 0x647u
#define REG_CULL_FACE 0x648u
#define REG_VIEWPORT_SCALE_OFFSET 0x64bu
#define REG_CT_WRITE 0x680u
#define REG_STREAM 0x700u           /* + 4j: format, address hi, lo, frequency */
#define REG_BLEND_TARGET 0x780u     /* + 8j: separate, color op/src/dst, alpha op/src/dst */
#define REG_STREAM_LIMIT 0x7c0u
#define REG_PIPELINE 0x800u         /* + 16j: shader, program, -, regs, binding */
#define REG_PIPELINE_STRIDE 16u
#define REG_BINDLESS_TEXTURE 0x982u

#define PIPELINE_STAGES 6u
#define MAX_TARGETS 8u
#define SPAN_MIN_PIXELS 1024 /* bounding box at which a blend table pays off */
#define SUBPIXEL_BITS 8
#define SUBPIXEL_ONE (1 << SUBPIXEL_BITS)
#define GUARD_BAND 4096.0f          /* clip-space x/y limit, in w units */
#define MAX_POLY 12u
#define VERTEX_CACHE 1024u /* direct-mapped by index; covers a prefetched chunk */
#define INDEX_BATCH 1024u
#define CBUF_SLOT_BYTES 0x10000u
#define MAX_VARYINGS (SM_ATTR_GENERIC_COUNT * 4u)
#define TOPOLOGY_POINTS 0u
#define TOPOLOGY_LINES 1u
#define TOPOLOGY_LINE_LOOP 2u
#define TOPOLOGY_LINE_STRIP 3u
#define TOPOLOGY_TRIANGLES 4u
#define TOPOLOGY_TRIANGLE_STRIP 5u
#define TOPOLOGY_TRIANGLE_FAN 6u
#define TOPOLOGY_QUADS 7u
#define TOPOLOGY_QUAD_STRIP 8u
#define TOPOLOGY_POLYGON 9u

static float f32(uint32_t v) {
  float f;
  memcpy(&f, &v, sizeof(f));
  return f;
}

static uint32_t u32f(float f) {
  uint32_t v;
  memcpy(&v, &f, sizeof(v));
  return v;
}

static uint64_t addr40(uint32_t hi, uint32_t lo) { return ((uint64_t)(hi & 0xffu) << 32) | lo; }

static float g_unorm8[256]; /* byte -> [0,1] */

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* ---- storage ------------------------------------------------------ */

#define STAGING_OFFSET 0u
#define SURFACE_OFFSET(i) (RASTER_MAX_SURFACE_BYTES * (1u + (i)))
#define PROGRAMS_OFFSET (RASTER_MAX_SURFACE_BYTES * (1u + RASTER_SURFACES))
#define PROGRAM_BYTES_OFFSET (PROGRAMS_OFFSET + sizeof(Raster3d_Program) * RASTER_PROGRAMS)
#define TEXTURE_POOL_OFFSET (PROGRAM_BYTES_OFFSET + RASTER_PROGRAM_READ_BYTES)
#define TEXTURE_RAW_OFFSET (TEXTURE_POOL_OFFSET + RASTER_TEXTURE_POOL_BYTES)
#define CBUF_OFFSET (TEXTURE_RAW_OFFSET + RASTER_TEXTURE_RAW_BYTES)
#define CBUF_BYTES ((size_t)2 * SM_CBUF_SLOTS * CBUF_SLOT_BYTES)
#define THREAD_OFFSET (CBUF_OFFSET + CBUF_BYTES)
#define THREAD_BYTES ((sizeof(Sm_Thread) + 63u) & ~(size_t)63u)
#define GPU_SHADERS_OFFSET (THREAD_OFFSET + THREAD_BYTES * WORKERS_MAX)
#define GPU_SHADERS_BYTES (((sizeof(Raster3d_Gpu_Shader) * RASTER_GPU_SHADERS) + 63u) & ~(size_t)63u)
#define GPU_WGSL_OFFSET (GPU_SHADERS_OFFSET + GPU_SHADERS_BYTES)
#define GPU_WGSL_STORAGE ((size_t)1 << 20)
#define GPU_VERTICES_OFFSET (GPU_WGSL_OFFSET + GPU_WGSL_STORAGE)
#define GPU_VERTICES_STORAGE ((size_t)768 * 1024)
#define GPU_DATA_OFFSET (GPU_VERTICES_OFFSET + GPU_VERTICES_STORAGE)
#define GPU_DATA_STORAGE (((size_t)WGSL_DRAW_CONSTANT_WORDS + (size_t)SM_CBUF_SLOTS * (CBUF_SLOT_BYTES / 4u) + 2u) * 4u)
#define STORAGE_BYTES (GPU_DATA_OFFSET + GPU_DATA_STORAGE + 64u)

size_t raster3d_storage_bytes(void) { return STORAGE_BYTES; }

void raster3d_init(Raster3d *r, uint8_t *storage, size_t bytes) {
  memset(r, 0, sizeof(*r));
  if (!storage || bytes < STORAGE_BYTES) return;
  /* Align the base for the structs carved out of it. */
  const uintptr_t base = ((uintptr_t)storage + 63u) & ~(uintptr_t)63u;
  uint8_t *s = (uint8_t *)base;
  r->staging = s + STAGING_OFFSET;
  for (uint32_t i = 0; i < RASTER_SURFACES; i++) r->surfaces[i].pixels = s + SURFACE_OFFSET(i);
  r->programs = (Raster3d_Program *)(void *)(s + PROGRAMS_OFFSET);
  for (uint32_t i = 0; i < RASTER_PROGRAMS; i++) r->programs[i].valid = false;
  r->program_bytes = s + PROGRAM_BYTES_OFFSET;
  r->texture_pool = s + TEXTURE_POOL_OFFSET;
  r->texture_raw = s + TEXTURE_RAW_OFFSET;
  r->cbuf_data = s + CBUF_OFFSET;
  for (uint32_t i = 0; i < WORKERS_MAX; i++) r->band_threads[i] = (Sm_Thread *)(void *)(s + THREAD_OFFSET + THREAD_BYTES * i);
  r->thread = r->band_threads[0];
  r->gpu_shaders = (Raster3d_Gpu_Shader *)(void *)(s + GPU_SHADERS_OFFSET);
  memset(r->gpu_shaders, 0, GPU_SHADERS_BYTES);
  r->gpu_wgsl = (char *)(s + GPU_WGSL_OFFSET);
  r->gpu_vertices = s + GPU_VERTICES_OFFSET;
  r->gpu_data = (uint32_t *)(void *)(s + GPU_DATA_OFFSET);
  for (uint32_t i = 0; i < 256u; i++) g_unorm8[i] = (float)i / 255.0f;
  tex_init_tables(); /* before any worker samples */
  workers_start(&r->workers, workers_default_count());
  log_info("[gpu] reference renderer: %u pixel worker(s)", r->workers.count);
  r->gpu_mipmaps = true;
  r->ready = true;
}

void raster3d_set_workers(Raster3d *r, uint32_t count) {
  if (count == r->workers.count) return;
  workers_stop(&r->workers);
  workers_start(&r->workers, count);
}

void raster3d_shutdown(Raster3d *r) { workers_stop(&r->workers); }

void raster3d_restart_workers_after_fork(Raster3d *r) {
  r->workers.impl = NULL; /* the parent's: no threads here to join */
  r->workers.count = 1u;
  workers_start(&r->workers, workers_default_count());
}

void raster3d_begin_submission(Raster3d *r) {
  if (!r->ready) return;
  r->submission++; /* programs re-validate on first use in a submission; textures per frame (raster3d_end_frame) */
}

void raster3d_end_frame(Raster3d *r) {
  if (r && r->ready) r->texture_epoch++;
}

/* ---- colour formats ----------------------------------------------- */

typedef enum Color_Kind { KIND_UNORM, KIND_SNORM, KIND_UINT, KIND_SINT, KIND_FLOAT, KIND_SRGB, KIND_R11G11B10F } Color_Kind;

#define CH_PAD 4u

typedef struct Color_Format {
  uint8_t format;
  uint8_t bytes;
  uint8_t kind;
  uint8_t count;
  uint8_t channel[4]; /* low bits first; CH_PAD = unused padding */
  uint8_t bits[4];
} Color_Format;

static const Color_Format k_color_formats[] = {
    {0xC0, 16, KIND_FLOAT, 4, {0, 1, 2, 3}, {32, 32, 32, 32}}, {0xC1, 16, KIND_SINT, 4, {0, 1, 2, 3}, {32, 32, 32, 32}},
    {0xC2, 16, KIND_UINT, 4, {0, 1, 2, 3}, {32, 32, 32, 32}}, {0xC3, 16, KIND_FLOAT, 4, {0, 1, 2, CH_PAD}, {32, 32, 32, 32}},
    {0xC4, 16, KIND_SINT, 4, {0, 1, 2, CH_PAD}, {32, 32, 32, 32}}, {0xC5, 16, KIND_UINT, 4, {0, 1, 2, CH_PAD}, {32, 32, 32, 32}},
    {0xC6, 8, KIND_UNORM, 4, {0, 1, 2, 3}, {16, 16, 16, 16}}, {0xC7, 8, KIND_SNORM, 4, {0, 1, 2, 3}, {16, 16, 16, 16}},
    {0xC8, 8, KIND_SINT, 4, {0, 1, 2, 3}, {16, 16, 16, 16}}, {0xC9, 8, KIND_UINT, 4, {0, 1, 2, 3}, {16, 16, 16, 16}},
    {0xCA, 8, KIND_FLOAT, 4, {0, 1, 2, 3}, {16, 16, 16, 16}}, {0xCB, 8, KIND_FLOAT, 2, {0, 1}, {32, 32}},
    {0xCC, 8, KIND_SINT, 2, {0, 1}, {32, 32}}, {0xCD, 8, KIND_UINT, 2, {0, 1}, {32, 32}},
    {0xCE, 8, KIND_FLOAT, 4, {0, 1, 2, CH_PAD}, {16, 16, 16, 16}}, {0xCF, 4, KIND_UNORM, 4, {2, 1, 0, 3}, {8, 8, 8, 8}},
    {0xD0, 4, KIND_SRGB, 4, {2, 1, 0, 3}, {8, 8, 8, 8}}, {0xD1, 4, KIND_UNORM, 4, {0, 1, 2, 3}, {10, 10, 10, 2}},
    {0xD2, 4, KIND_UINT, 4, {0, 1, 2, 3}, {10, 10, 10, 2}}, {0xD5, 4, KIND_UNORM, 4, {0, 1, 2, 3}, {8, 8, 8, 8}},
    {0xD6, 4, KIND_SRGB, 4, {0, 1, 2, 3}, {8, 8, 8, 8}}, {0xD7, 4, KIND_SNORM, 4, {0, 1, 2, 3}, {8, 8, 8, 8}},
    {0xD8, 4, KIND_SINT, 4, {0, 1, 2, 3}, {8, 8, 8, 8}}, {0xD9, 4, KIND_UINT, 4, {0, 1, 2, 3}, {8, 8, 8, 8}},
    {0xDA, 4, KIND_UNORM, 2, {0, 1}, {16, 16}}, {0xDB, 4, KIND_SNORM, 2, {0, 1}, {16, 16}},
    {0xDC, 4, KIND_SINT, 2, {0, 1}, {16, 16}}, {0xDD, 4, KIND_UINT, 2, {0, 1}, {16, 16}},
    {0xDE, 4, KIND_FLOAT, 2, {0, 1}, {16, 16}}, {0xDF, 4, KIND_UNORM, 4, {2, 1, 0, 3}, {10, 10, 10, 2}},
    {0xE0, 4, KIND_R11G11B10F, 3, {0, 1, 2}, {11, 11, 10}},
    {0xE3, 4, KIND_SINT, 1, {0}, {32}}, {0xE4, 4, KIND_UINT, 1, {0}, {32}}, {0xE5, 4, KIND_FLOAT, 1, {0}, {32}},
    {0xE6, 4, KIND_UNORM, 4, {2, 1, 0, CH_PAD}, {8, 8, 8, 8}}, {0xE7, 4, KIND_SRGB, 4, {2, 1, 0, CH_PAD}, {8, 8, 8, 8}},
    {0xE8, 2, KIND_UNORM, 3, {2, 1, 0}, {5, 6, 5}}, {0xE9, 2, KIND_UNORM, 4, {2, 1, 0, 3}, {5, 5, 5, 1}},
    {0xEA, 2, KIND_UNORM, 2, {0, 1}, {8, 8}}, {0xEB, 2, KIND_SNORM, 2, {0, 1}, {8, 8}},
    {0xEC, 2, KIND_SINT, 2, {0, 1}, {8, 8}}, {0xED, 2, KIND_UINT, 2, {0, 1}, {8, 8}},
    {0xEE, 2, KIND_UNORM, 1, {0}, {16}}, {0xEF, 2, KIND_SNORM, 1, {0}, {16}}, {0xF0, 2, KIND_SINT, 1, {0}, {16}},
    {0xF1, 2, KIND_UINT, 1, {0}, {16}}, {0xF2, 2, KIND_FLOAT, 1, {0}, {16}},
    {0xF3, 1, KIND_UNORM, 1, {0}, {8}}, {0xF4, 1, KIND_SNORM, 1, {0}, {8}}, {0xF5, 1, KIND_SINT, 1, {0}, {8}},
    {0xF6, 1, KIND_UINT, 1, {0}, {8}}, {0xF7, 1, KIND_UNORM, 1, {3}, {8}},
    {0xF8, 2, KIND_UNORM, 4, {2, 1, 0, CH_PAD}, {5, 5, 5, 1}}, {0xF9, 4, KIND_UNORM, 4, {0, 1, 2, CH_PAD}, {8, 8, 8, 8}},
    {0xFA, 4, KIND_SRGB, 4, {0, 1, 2, CH_PAD}, {8, 8, 8, 8}}, {0xFB, 2, KIND_UNORM, 4, {2, 1, 0, CH_PAD}, {5, 5, 5, 1}},
    {0xFC, 2, KIND_UNORM, 4, {2, 1, 0, CH_PAD}, {5, 5, 5, 1}}, {0xFD, 4, KIND_UNORM, 4, {2, 1, 0, CH_PAD}, {8, 8, 8, 8}},
    {0xFE, 4, KIND_UNORM, 4, {2, 1, 0, CH_PAD}, {8, 8, 8, 8}}, {0xFF, 4, KIND_FLOAT, 1, {0}, {32}},
};

static const Color_Format *color_format(uint32_t format) {
  for (uint32_t i = 0; i < sizeof(k_color_formats) / sizeof(k_color_formats[0]); i++)
    if (k_color_formats[i].format == format) return &k_color_formats[i];
  return NULL;
}

/* Zeta formats (SET_ZT_FORMAT). */
#define ZT_ZF32 0x0Au
#define ZT_Z16 0x13u
#define ZT_Z24S8 0x14u
#define ZT_X8Z24 0x15u
#define ZT_S8Z24 0x16u
#define ZT_V8Z24 0x18u
#define ZT_ZF32_X24S8 0x19u
#define ZT_S8 0x17u
#define ZT_NO_STENCIL 0xffu

static uint32_t zeta_bytes(uint32_t format) {
  switch (format) {
  case ZT_S8: return 1;
  case ZT_Z16: return 2;
  case ZT_ZF32_X24S8: return 8;
  case ZT_ZF32: case ZT_Z24S8: case ZT_X8Z24: case ZT_S8Z24: case ZT_V8Z24: return 4;
  default: return 0;
  }
}

static float read_depth(uint32_t format, const uint8_t *p) {
  uint32_t v = 0;
  switch (format) {
  case ZT_Z16: memcpy(&v, p, 2); return (float)v / 65535.0f;
  case ZT_Z24S8: memcpy(&v, p, 4); return (float)(v >> 8) / 16777215.0f;
  case ZT_ZF32: case ZT_ZF32_X24S8: memcpy(&v, p, 4); return f32(v);
  default: memcpy(&v, p, 4); return (float)(v & 0xffffffu) / 16777215.0f;
  }
}

static void write_depth(uint32_t format, uint8_t *p, float z) {
  uint32_t v = 0;
  z = clamp01(z);
  switch (format) {
  case ZT_Z16: v = (uint32_t)lrintf(z * 65535.0f); memcpy(p, &v, 2); return;
  case ZT_Z24S8: memcpy(&v, p, 4); v = (v & 0xffu) | ((uint32_t)lrintf(z * 16777215.0f) << 8); memcpy(p, &v, 4); return;
  case ZT_ZF32: case ZT_ZF32_X24S8: v = u32f(z); memcpy(p, &v, 4); return;
  default: memcpy(&v, p, 4); v = (v & 0xff000000u) | ((uint32_t)lrintf(z * 16777215.0f) & 0xffffffu); memcpy(p, &v, 4); return;
  }
}

/* Byte of a zeta texel holding stencil, or ZT_NO_STENCIL. */
static uint32_t stencil_byte(uint32_t format) {
  switch (format) {
  case ZT_Z24S8: case ZT_S8: return 0;
  case ZT_S8Z24: return 3;
  case ZT_ZF32_X24S8: return 4;
  default: return ZT_NO_STENCIL;
  }
}

/* Stencil operations, OGL and D3D enums (NVB197_SET_STENCIL_OP_*). */
static uint8_t stencil_apply(uint32_t op, uint8_t value, uint8_t ref) {
  switch (op) {
  case 0x0000: case 2: return 0;                                     /* ZERO */
  case 0x1e01: case 3: return ref;                                   /* REPLACE */
  case 0x1e02: case 4: return value == 0xffu ? value : (uint8_t)(value + 1u); /* INCR_SAT */
  case 0x1e03: case 5: return value == 0 ? value : (uint8_t)(value - 1u);    /* DECR_SAT */
  case 0x150a: case 6: return (uint8_t)~value;                       /* INVERT */
  case 0x8507: case 7: return (uint8_t)(value + 1u);                 /* INCR (wrap) */
  case 0x8508: case 8: return (uint8_t)(value - 1u);                 /* DECR (wrap) */
  default: return value;                                             /* KEEP */
  }
}

static float half_to_float(uint32_t h) {
  const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, mant = h & 0x3ffu;
  float v;
  if (exp == 0) v = ldexpf((float)mant, -24);
  else if (exp == 31) v = mant ? NAN : INFINITY;
  else v = ldexpf((float)(mant | 0x400u), (int)exp - 25);
  return sign ? -v : v;
}

static uint32_t float_to_half(float f) {
  const uint32_t v = u32f(f);
  const uint32_t sign = (v >> 16) & 0x8000u;
  const int32_t exp = (int32_t)((v >> 23) & 0xffu) - 127 + 15;
  const uint32_t mant = v & 0x7fffffu;
  if (((v >> 23) & 0xffu) == 0xffu) return sign | 0x7c00u | (mant ? 0x200u : 0u);
  if (exp >= 31) return sign | 0x7c00u;
  if (exp <= 0) {
    if (exp < -10) return sign;
    const uint32_t m = mant | 0x800000u;
    return sign | (m >> (uint32_t)(14 - exp));
  }
  return sign | ((uint32_t)exp << 10) | (mant >> 13);
}

static float srgb_encode(float c) {
  c = clamp01(c);
  return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static float srgb_decode(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

/* n (<= 32) bits starting at bit `bit` of a little-endian byte string. */
static uint64_t bits_at(const uint8_t *p, uint32_t bit, uint32_t n) {
  if ((bit % 8u) == 0) {
    const uint8_t *b = p + bit / 8u;
    switch (n) {
    case 8: return b[0];
    case 16: return (uint64_t)b[0] | ((uint64_t)b[1] << 8);
    case 32: return (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24);
    default: break;
    }
  }
  uint64_t v = 0;
  for (uint32_t k = 0; k < n; k++) v |= (uint64_t)((p[(bit + k) / 8u] >> ((bit + k) % 8u)) & 1u) << k;
  return v;
}

static void put_bits(uint8_t *p, uint32_t bit, uint32_t n, uint64_t v) {
  if ((bit % 8u) == 0 && (n % 8u) == 0 && n <= 32u) {
    for (uint32_t k = 0; k < n / 8u; k++) p[bit / 8u + k] = (uint8_t)(v >> (8u * k));
    return;
  }
  for (uint32_t k = 0; k < n; k++) {
    const uint32_t at = bit + k;
    if ((v >> k) & 1u) p[at / 8u] |= (uint8_t)(1u << (at % 8u));
    else p[at / 8u] &= (uint8_t)~(1u << (at % 8u));
  }
}

static bool is_unorm8x4(const Color_Format *f) {
  return f->kind == KIND_UNORM && f->bytes == 4u && f->count == 4u && f->bits[0] == 8u && f->bits[1] == 8u &&
         f->bits[2] == 8u && f->bits[3] == 8u;
}

static uint8_t to_unorm8(float v) {
  if (!(v > 0.0f)) return 0;
  if (v >= 1.0f) return 255;
  return (uint8_t)(v * 255.0f + 0.5f);
}

/* Packs a colour (floats, or integer bits for integer kinds). */
static void encode_color(const Color_Format *f, const uint32_t color[4], uint8_t *out) {
  if (is_unorm8x4(f)) {
    for (uint32_t i = 0; i < 4u; i++) out[i] = f->channel[i] == CH_PAD ? 0xffu : to_unorm8(f32(color[f->channel[i]]));
    return;
  }
  if (f->kind == KIND_R11G11B10F) {
    /* Unsigned small floats: drop to half, then trim mantissas. */
    const uint32_t r = float_to_half(fmaxf(f32(color[0]), 0.0f)) >> 4;
    const uint32_t g = float_to_half(fmaxf(f32(color[1]), 0.0f)) >> 4;
    const uint32_t b = float_to_half(fmaxf(f32(color[2]), 0.0f)) >> 5;
    const uint32_t v = (r & 0x7ffu) | ((g & 0x7ffu) << 11) | ((b & 0x3ffu) << 22);
    memcpy(out, &v, 4);
    return;
  }
  uint8_t buf[16];
  memset(buf, 0, sizeof(buf));
  uint32_t bit = 0;
  for (uint32_t i = 0; i < f->count; i++) {
    const uint32_t n = f->bits[i], ch = f->channel[i];
    uint64_t v = 0;
    if (ch != CH_PAD) {
      const uint32_t raw = color[ch];
      const uint64_t max = n >= 32u ? 0xffffffffull : ((1ull << n) - 1ull);
      switch (f->kind) {
      case KIND_UNORM: v = (uint64_t)llrintf(clamp01(f32(raw)) * (float)max); break;
      case KIND_SRGB: v = (uint64_t)llrintf((ch == 3u ? clamp01(f32(raw)) : srgb_encode(f32(raw))) * (float)max); break;
      case KIND_SNORM: {
        float x = f32(raw);
        x = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
        v = (uint64_t)(int64_t)llrintf(x * (float)(max >> 1)) & max;
        break;
      }
      case KIND_FLOAT: v = n == 32u ? raw : (n == 16u ? float_to_half(f32(raw)) : 0u); break;
      default: v = raw & max; break; /* UINT / SINT */
      }
    }
    put_bits(buf, bit, n, v);
    bit += n;
  }
  memcpy(out, buf, f->bytes);
}

static void decode_color(const Color_Format *f, const uint8_t *p, float out[4]) {
  out[0] = out[1] = out[2] = 0.0f;
  out[3] = 1.0f;
  if (is_unorm8x4(f)) {
    for (uint32_t i = 0; i < 4u; i++)
      if (f->channel[i] != CH_PAD) out[f->channel[i]] = g_unorm8[p[i]];
    return;
  }
  if (f->kind == KIND_R11G11B10F) {
    uint32_t v;
    memcpy(&v, p, 4);
    out[0] = half_to_float((v & 0x7ffu) << 4);
    out[1] = half_to_float(((v >> 11) & 0x7ffu) << 4);
    out[2] = half_to_float(((v >> 22) & 0x3ffu) << 5);
    return;
  }
  uint32_t bit = 0;
  for (uint32_t i = 0; i < f->count; i++) {
    const uint32_t n = f->bits[i], ch = f->channel[i];
    const uint64_t v = bits_at(p, bit, n);
    bit += n;
    if (ch == CH_PAD) continue;
    const uint64_t max = n >= 32u ? 0xffffffffull : ((1ull << n) - 1ull);
    switch (f->kind) {
    case KIND_UNORM: out[ch] = (float)v / (float)max; break;
    case KIND_SRGB: out[ch] = ch == 3u ? (float)v / (float)max : srgb_decode((float)v / (float)max); break;
    case KIND_SNORM: {
      const int64_t s = (int64_t)(v << (64u - n)) >> (64u - n);
      const float x = (float)s / (float)(max >> 1);
      out[ch] = x < -1.0f ? -1.0f : x;
      break;
    }
    case KIND_FLOAT: out[ch] = n == 32u ? f32((uint32_t)v) : half_to_float((uint32_t)v); break;
    default: out[ch] = f32((uint32_t)v); break;
    }
  }
}

/* ---- surfaces ----------------------------------------------------- */

typedef struct Surface_Desc {
  uint64_t address;
  uint32_t width, height;
  uint32_t format;
  uint32_t bytes_per_pixel;
  bool block_linear;
  uint32_t block_height_log2;
  uint32_t pitch;
  bool depth;
} Surface_Desc;

static uint64_t surface_guest_bytes(const Surface_Desc *d) {
  if (d->block_linear) return block_linear_size(d->width * d->bytes_per_pixel, d->height, d->block_height_log2);
  return (uint64_t)d->pitch * d->height;
}

static bool color_target_desc(const uint32_t *regs, uint32_t target, Surface_Desc *out) {
  const uint32_t *rt = regs + REG_RT + target * REG_RT_STRIDE;
  memset(out, 0, sizeof(*out));
  out->address = addr40(rt[0], rt[1]);
  out->format = rt[4] & 0xffu;
  const Color_Format *f = color_format(out->format);
  if (!out->address || !f) return false;
  out->bytes_per_pixel = f->bytes;
  out->block_linear = ((rt[5] >> 12) & 1u) == 0;
  out->block_height_log2 = (rt[5] >> 4) & 0xfu;
  if (out->block_linear) {
    out->width = rt[2] & 0xfffffffu;
  } else {
    out->pitch = rt[2] & 0xfffffffu;
    out->width = out->pitch / f->bytes;
  }
  out->height = rt[3] & 0x1ffffu;
  if (out->block_height_log2 > 5u) out->block_height_log2 = 5u;
  if (!out->width || !out->height) return false;
  return surface_guest_bytes(out) <= RASTER_MAX_SURFACE_BYTES &&
         (uint64_t)out->width * out->height * out->bytes_per_pixel <= RASTER_MAX_SURFACE_BYTES;
}

static bool zeta_desc(const uint32_t *regs, Surface_Desc *out) {
  memset(out, 0, sizeof(*out));
  if (!(regs[REG_ZT_SELECT] & 1u)) return false;
  out->address = addr40(regs[REG_ZT_A], regs[REG_ZT_B]);
  out->format = regs[REG_ZT_FORMAT] & 0x1fu;
  out->bytes_per_pixel = zeta_bytes(out->format);
  out->block_linear = true;
  out->block_height_log2 = (regs[REG_ZT_BLOCK] >> 4) & 0xfu;
  if (out->block_height_log2 > 5u) out->block_height_log2 = 5u;
  out->width = regs[REG_ZT_SIZE_A] & 0xfffffffu;
  out->height = regs[REG_ZT_SIZE_B] & 0x1ffffu;
  out->depth = true;
  if (!out->address || !out->bytes_per_pixel || !out->width || !out->height) return false;
  return surface_guest_bytes(out) <= RASTER_MAX_SURFACE_BYTES &&
         (uint64_t)out->width * out->height * out->bytes_per_pixel <= RASTER_MAX_SURFACE_BYTES;
}

static void surface_write_back(Raster3d *r, Raster3d_Surface *s, const Gpu_Memory *mem) {
  if (!s->in_use || !s->dirty) return;
  const uint32_t row = s->width * s->bytes_per_pixel;
  if (s->block_linear) {
    /* Keep the padding the guest had: start from its bytes when known. */
    pitch_to_block_linear(s->pixels, row, r->staging, row, s->height, s->block_height_log2);
    (void)mem->write(mem->user, s->address, r->staging, s->guest_bytes);
  } else {
    for (uint32_t y = 0; y < s->height; y++)
      (void)mem->write(mem->user, s->address + (uint64_t)y * s->pitch, s->pixels + (uint64_t)y * row, row);
  }
  s->dirty = false;
}

static void surface_load(Raster3d *r, Raster3d_Surface *s, const Gpu_Memory *mem) {
  if (s->loaded) return;
  const uint32_t row = s->width * s->bytes_per_pixel;
  if (s->block_linear) {
    if (mem->read(mem->user, s->address, r->staging, s->guest_bytes))
      block_linear_to_pitch(r->staging, s->pixels, row, row, s->height, s->block_height_log2);
    else
      memset(s->pixels, 0, (size_t)row * s->height);
  } else {
    for (uint32_t y = 0; y < s->height; y++)
      if (!mem->read(mem->user, s->address + (uint64_t)y * s->pitch, s->pixels + (uint64_t)y * row, row))
        memset(s->pixels + (uint64_t)y * row, 0, row);
  }
  s->loaded = true;
}

/* Render-to-texture: a decoded texture over memory the GPU is about to
 * draw into is re-checked against guest memory at its next use, even
 * within the submission that already validated it. */
static void textures_invalidate(Raster3d *r, uint64_t address, uint64_t bytes) {
  for (uint32_t i = 0; i < RASTER_TEXTURES; i++) {
    Raster3d_Texture *t = &r->textures[i];
    if (t->valid && t->address < address + bytes && address < t->address + t->raw_bytes) t->validated = r->texture_epoch - 1u;
  }
}

/* Finds or creates the cached copy of a surface, which the caller then
 * draws into. `load`: fetch guest contents now (false when the caller
 * overwrites all of it). */
static Raster3d_Surface g_gpu_placeholder; /* GPU mode: setup_state's targets (present, never rasterised here) */

static Raster3d_Surface *surface_get(Raster3d *r, const Surface_Desc *d, const Gpu_Memory *mem, bool load) {
  if (r->gpu) return &g_gpu_placeholder;
  Raster3d_Surface *victim = NULL;
  r->tick++;
  textures_invalidate(r, d->address, surface_guest_bytes(d));
  for (uint32_t i = 0; i < RASTER_SURFACES; i++) {
    Raster3d_Surface *s = &r->surfaces[i];
    if (s->in_use && s->address == d->address) {
      if (s->width == d->width && s->height == d->height && s->format == d->format && s->depth == d->depth &&
          s->block_linear == d->block_linear && s->block_height_log2 == d->block_height_log2) {
        s->last_used = r->tick;
        if (load) surface_load(r, s, mem);
        return s;
      }
      surface_write_back(r, s, mem); /* re-described: retire the old view */
      s->in_use = false;
    }
  }
  for (uint32_t i = 0; i < RASTER_SURFACES; i++) {
    Raster3d_Surface *s = &r->surfaces[i];
    if (!s->in_use) {
      victim = s;
      break;
    }
    if (!victim || s->last_used < victim->last_used) victim = s;
  }
  surface_write_back(r, victim, mem);
  uint8_t *pixels = victim->pixels;
  memset(victim, 0, sizeof(*victim));
  victim->pixels = pixels;
  victim->in_use = true;
  victim->address = d->address;
  victim->width = d->width;
  victim->height = d->height;
  victim->format = d->format;
  victim->bytes_per_pixel = d->bytes_per_pixel;
  victim->block_linear = d->block_linear;
  victim->block_height_log2 = d->block_height_log2;
  victim->pitch = d->pitch;
  victim->depth = d->depth;
  victim->guest_bytes = surface_guest_bytes(d);
  victim->last_used = r->tick;
  if (load) surface_load(r, victim, mem);
  return victim;
}

void raster3d_flush(Raster3d *r, const Gpu_Memory *mem) {
  if (!r || !r->ready) return;
  if (r->gpu) {
    gpu_stream_publish(r->gpu);
    return;
  }
  for (uint32_t i = 0; i < RASTER_SURFACES; i++) surface_write_back(r, &r->surfaces[i], mem);
  /* The guest may change these before the next submission. */
  for (uint32_t i = 0; i < RASTER_SURFACES; i++) r->surfaces[i].loaded = r->surfaces[i].in_use ? false : r->surfaces[i].loaded;
}

void raster3d_sync_range(Raster3d *r, const Gpu_Memory *mem, uint64_t address, uint64_t bytes, bool write) {
  if (!r || !r->ready || !bytes) return;
  if (r->gpu) {
    /* GPU surfaces cannot be read back; written ones re-upload at their
     * next use. */
    if (!write) return;
    for (uint32_t i = 0; i < RASTER_GPU_SURFACES; i++) {
      Raster3d_Gpu_Surface *s = &r->gpu_surfaces[i];
      if (s->in_use && s->address < address + bytes && address < s->address + s->guest_bytes) s->stale = true;
    }
    textures_invalidate(r, address, bytes);
    return;
  }
  for (uint32_t i = 0; i < RASTER_SURFACES; i++) {
    Raster3d_Surface *s = &r->surfaces[i];
    if (!s->in_use || s->address >= address + bytes || address >= s->address + s->guest_bytes) continue;
    surface_write_back(r, s, mem);
    if (write) s->loaded = false; /* reload what the copy leaves there */
  }
  if (write) textures_invalidate(r, address, bytes);
}

/* ---- clip rectangle ----------------------------------------------- */

typedef struct Rect {
  int32_t x0, y0, x1, y1; /* [x0, x1) x [y0, y1) */
} Rect;

static void rect_intersect(Rect *a, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  if (x0 > a->x0) a->x0 = x0;
  if (y0 > a->y0) a->y0 = y0;
  if (x1 < a->x1) a->x1 = x1;
  if (y1 < a->y1) a->y1 = y1;
}

static void rect_scissor(Rect *rect, const uint32_t *regs) {
  if (!(regs[REG_SCISSOR] & 1u)) return;
  const uint32_t h = regs[REG_SCISSOR + 1u], v = regs[REG_SCISSOR + 2u];
  rect_intersect(rect, (int32_t)(h & 0xffffu), (int32_t)(v & 0xffffu), (int32_t)(h >> 16), (int32_t)(v >> 16));
}

static void rect_viewport_clip(Rect *rect, const uint32_t *regs) {
  const uint32_t h = regs[REG_VIEWPORT_CLIP], v = regs[REG_VIEWPORT_CLIP + 1u];
  if (!h && !v) return;
  const int32_t x0 = (int32_t)(h & 0xffffu), y0 = (int32_t)(v & 0xffffu);
  rect_intersect(rect, x0, y0, x0 + (int32_t)(h >> 16), y0 + (int32_t)(v >> 16));
}

/* ---- clears ------------------------------------------------------- */

static void gpu_clear(Raster3d *r, const uint32_t *regs, const Gpu_Memory *mem, uint32_t clear);

void raster3d_clear(Raster3d *r, const uint32_t *regs, const Gpu_Memory *mem, uint32_t clear) {
  if (!r || !r->ready || r->skip_draws) return;
  r->stats.clears++;
  if (r->gpu) {
    gpu_clear(r, regs, mem, clear);
    return;
  }
  const uint32_t control = regs[REG_CLEAR_CONTROL];
  const bool color = (clear & 0x3cu) != 0;
  const bool depth = (clear & 1u) != 0;
  const bool stencil = (clear & 2u) != 0;
  if (color) {
    const uint32_t mrt = (clear >> 6) & 0xfu;
    const uint32_t select = regs[REG_CT_SELECT];
    const uint32_t target = mrt < 8u ? (select >> (4u + 3u * mrt)) & 7u : 0u;
    Surface_Desc d;
    const Color_Format *f;
    if (color_target_desc(regs, target, &d) && (f = color_format(d.format)) != NULL) {
      Rect rect = {0, 0, (int32_t)d.width, (int32_t)d.height};
      if (control & 0x100u) rect_scissor(&rect, regs);
      if (control & 0x10u) {
        const uint32_t h = regs[REG_CLEAR_RECT_H], v = regs[REG_CLEAR_RECT_V];
        rect_intersect(&rect, (int32_t)(h & 0xffffu), (int32_t)(v & 0xffffu), (int32_t)(h >> 16), (int32_t)(v >> 16));
      }
      if (control & 0x1000u) rect_viewport_clip(&rect, regs);
      const uint32_t write = (regs[REG_SINGLE_CT_WRITE] & 1u) ? regs[REG_CT_WRITE] : regs[REG_CT_WRITE + target];
      uint32_t comps = 0;
      for (uint32_t c = 0; c < 4; c++)
        if ((clear >> (2u + c)) & 1u && (write >> (4u * c)) & 1u) comps |= 1u << c;
      if (rect.x0 < rect.x1 && rect.y0 < rect.y1 && comps) {
        const bool full = rect.x0 == 0 && rect.y0 == 0 && rect.x1 == (int32_t)d.width && rect.y1 == (int32_t)d.height &&
                          comps == 0xfu;
        Raster3d_Surface *s = surface_get(r, &d, mem, !full);
        if (full) s->loaded = true;
        uint32_t value[4];
        for (uint32_t c = 0; c < 4; c++) value[c] = regs[REG_CLEAR_COLOR + c];
        uint8_t px[16];
        const uint32_t bpp = s->bytes_per_pixel, row = s->width * bpp;
        if (comps == 0xfu) {
          /* Whole pixels: build the first row (doubling copies), then
           * copy it down the rectangle. */
          encode_color(f, value, px);
          const size_t span = (size_t)(rect.x1 - rect.x0) * bpp;
          uint8_t *first = s->pixels + (uint64_t)rect.y0 * row + (uint64_t)rect.x0 * bpp;
          memcpy(first, px, bpp);
          for (size_t done = bpp; done < span;) {
            const size_t n = done < span - done ? done : span - done;
            memcpy(first + done, first, n);
            done += n;
          }
          for (int32_t y = rect.y0 + 1; y < rect.y1; y++) memcpy(first + (uint64_t)(y - rect.y0) * row, first, span);
        } else {
          for (int32_t y = rect.y0; y < rect.y1; y++) {
            uint8_t *line = s->pixels + (uint64_t)y * row;
            for (int32_t x = rect.x0; x < rect.x1; x++) {
              uint8_t *p = line + (uint64_t)x * bpp;
              float cur[4];
              decode_color(f, p, cur);
              uint32_t merged[4];
              for (uint32_t c = 0; c < 4; c++) merged[c] = (comps >> c) & 1u ? value[c] : u32f(cur[c]);
              encode_color(f, merged, p);
            }
          }
        }
        s->dirty = true;
      }
    }
  }
  if (depth || stencil) {
    Surface_Desc d;
    if (zeta_desc(regs, &d)) {
      Rect rect = {0, 0, (int32_t)d.width, (int32_t)d.height};
      if (control & 0x100u) rect_scissor(&rect, regs);
      const uint32_t sbyte = stencil_byte(d.format);
      const bool has_depth = d.format != ZT_S8;
      const bool clear_depth = depth && has_depth;
      /* Stencil clears go through the front write mask. */
      const uint8_t smask = (uint8_t)regs[REG_STENCIL_FRONT + 6u];
      const bool clear_stencil = stencil && sbyte != ZT_NO_STENCIL && smask;
      const bool full = rect.x0 == 0 && rect.y0 == 0 && rect.x1 == (int32_t)d.width && rect.y1 == (int32_t)d.height &&
                        (clear_depth || !has_depth) && (clear_stencil ? smask == 0xffu : sbyte == ZT_NO_STENCIL);
      if (clear_depth || clear_stencil) {
        Raster3d_Surface *s = surface_get(r, &d, mem, !full);
        if (full) s->loaded = true;
        const float z = f32(regs[REG_Z_CLEAR]);
        const uint8_t sv = (uint8_t)regs[REG_STENCIL_CLEAR];
        for (int32_t y = rect.y0; y < rect.y1; y++)
          for (int32_t x = rect.x0; x < rect.x1; x++) {
            uint8_t *p = s->pixels + ((uint64_t)y * s->width + (uint64_t)x) * s->bytes_per_pixel;
            if (clear_depth) write_depth(s->format, p, z);
            if (clear_stencil) p[sbyte] = (uint8_t)((p[sbyte] & ~smask) | (sv & smask));
          }
        s->dirty = true;
      }
    }
  }
}

/* ---- programs ----------------------------------------------------- */

static const Sm_Program *program_get(Raster3d *r, uint64_t address, const Gpu_Memory *mem) {
  Raster3d_Program *slot = NULL;
  r->tick++;
  for (uint32_t i = 0; i < RASTER_PROGRAMS; i++) {
    Raster3d_Program *p = &r->programs[i];
    if (p->valid && p->program.address == address) {
      slot = p;
      break;
    }
  }
  if (slot && slot->validated == r->submission) {
    slot->last_used = r->tick;
    return &slot->program;
  }
  /* (Re)read: grow the read until the end marker or the limit. */
  uint32_t got = 0;
  uint32_t extent = 0;
  for (uint32_t want = 0x800u; want <= RASTER_PROGRAM_READ_BYTES; want *= 2u) {
    if (want > got && !mem->read(mem->user, address + got, r->program_bytes + got, want - got)) break;
    got = want;
    extent = sm_program_extent(r->program_bytes, got);
    if (extent < got) break;
  }
  if (!got) return NULL;
  if (!extent) extent = got;
  const uint32_t hash = sm_hash(r->program_bytes, extent);
  if (slot && slot->program.hash == hash && slot->program.byte_size == extent) {
    slot->validated = r->submission;
    slot->last_used = r->tick;
    return &slot->program;
  }
  if (!slot) {
    for (uint32_t i = 0; i < RASTER_PROGRAMS; i++) {
      Raster3d_Program *p = &r->programs[i];
      if (!p->valid) {
        slot = p;
        break;
      }
      if (!slot || p->last_used < slot->last_used) slot = p;
    }
  }
  sm_program_decode(r->program_bytes, extent, address, &slot->program);
  if (r->on_program_decoded) r->on_program_decoded(r->on_program_user, &slot->program);
  slot->valid = true;
  slot->validated = r->submission;
  slot->last_used = r->tick;
  r->stats.unknown_ops += slot->program.unknown_ops;
  if (slot->program.unknown_ops) {
    for (uint32_t i = 0; i < slot->program.word_count; i++) {
      if (slot->program.insns[i].op == SM_OP_INVALID) {
        log_warn("[gpu] shader @%llx: %u undecoded instruction(s), first %016llx at word %u",
                 (unsigned long long)address, slot->program.unknown_ops,
                 (unsigned long long)slot->program.insns[i].raw, i);
        break;
      }
    }
  }
  return &slot->program;
}

/* ---- textures ----------------------------------------------------- */

#define RESOLVED_TEXTURES 16u

/* Per-draw handle -> texture resolution, one per shading thread (the
 * vertex stage's in the Draw_Context, each pixel band's in its state):
 * hits need no lock; a miss loads under the workers' lock. Shaders' Sm_Env
 * `user` points here. */
typedef struct Tex_Resolver {
  struct Draw_Context *ctx;
  uint32_t handle[RESOLVED_TEXTURES];
  Raster3d_Texture *texture[RESOLVED_TEXTURES];
  Tex_Sampler sampler[RESOLVED_TEXTURES];
  uint32_t count;
} Tex_Resolver;

typedef struct Draw_Context {
  Raster3d *r;
  const uint32_t *regs;
  const Gpu_Memory *mem;
  Sm_Env env[2];         /* 0 vertex, 1 pixel */
  const Sm_Program *vs;
  const Sm_Program *ps;
  uint32_t instance;
  Tex_Resolver resolver; /* the vertex stage's (and the pixel template's) */
  /* vertex fetch windows */
  uint64_t window_base[RASTER_STREAMS];
  uint32_t window_size[RASTER_STREAMS];
  uint8_t window[RASTER_STREAMS][RASTER_STREAM_WINDOW];
} Draw_Context;

/* Change detection for decoded textures (not cryptographic): four
 * independent multiply-xorshift lanes over 32-byte blocks, so the CPU
 * overlaps them; streamed, so guest memory is hashed in place-sized
 * chunks without staging the whole texture. */
#define HASH_MUL 0xFF51AFD7ED558CCDull
#define HASH_CHUNK_BYTES 0x10000u

typedef struct Content_Hash {
  uint64_t lane[4];
  uint64_t length;
} Content_Hash;

static void content_hash_begin(Content_Hash *h, uint64_t length) {
  h->lane[0] = 0x9E3779B97F4A7C15ull ^ length;
  h->lane[1] = 0xC2B2AE3D27D4EB4Full;
  h->lane[2] = 0x165667B19E3779F9ull;
  h->lane[3] = 0x27D4EB2F165667C5ull;
  h->length = length;
}

/* `n` must be a multiple of 32 except for the last call. */
static void content_hash_update(Content_Hash *h, const uint8_t *p, uint64_t n) {
  uint64_t a = h->lane[0], b = h->lane[1], c = h->lane[2], d = h->lane[3];
  uint64_t i = 0;
  for (; i + 32u <= n; i += 32u) {
    uint64_t v[4];
    memcpy(v, p + i, sizeof(v));
    a = (a ^ v[0]) * HASH_MUL;
    b = (b ^ v[1]) * HASH_MUL;
    c = (c ^ v[2]) * HASH_MUL;
    d = (d ^ v[3]) * HASH_MUL;
    a ^= a >> 32;
    b ^= b >> 32;
    c ^= c >> 32;
    d ^= d >> 32;
  }
  for (; i < n; i++) a = (a ^ p[i]) * 0x100000001B3ull;
  h->lane[0] = a;
  h->lane[1] = b;
  h->lane[2] = c;
  h->lane[3] = d;
}

static uint64_t content_hash_end(const Content_Hash *h) {
  uint64_t x = h->lane[0] ^ (h->lane[1] * 31u) ^ (h->lane[2] * 1009u) ^ (h->lane[3] * 65537u) ^ h->length;
  x = (x ^ (x >> 33)) * HASH_MUL;
  return x ^ (x >> 33);
}

/* Hashes guest GPU memory [va, va + n) in chunks (callers hold the pool
 * lock, so one chunk buffer serves). */
static bool guest_hash(const Gpu_Memory *mem, uint64_t va, uint64_t n, uint64_t *out) {
  static uint8_t chunk[HASH_CHUNK_BYTES];
  Content_Hash h;
  content_hash_begin(&h, n);
  for (uint64_t done = 0; done < n;) {
    const uint64_t len = n - done < HASH_CHUNK_BYTES ? n - done : HASH_CHUNK_BYTES;
    if (!mem->read(mem->user, va + done, chunk, len)) return false;
    content_hash_update(&h, chunk, len);
    done += len;
  }
  *out = content_hash_end(&h);
  return true;
}

#define RT_FORMAT_A8B8G8R8_UNORM 0xD5u
#define TEX_FORMAT_A8B8G8R8 0x08u

/* Render-to-texture without the round trip: a texture that is exactly a
 * render target the renderer holds (same address and size, RGBA8 unorm
 * 2D, one level) samples its host pixels in place - no write-back, read,
 * hash and decode per pass. NULL: not such a texture. */
static Raster3d_Texture *surface_view(Raster3d *r, const uint32_t tic[8]) {
  Tex_Header h;
  tex_header_parse(tic, &h);
  if (h.format != TEX_FORMAT_A8B8G8R8 || h.srgb || h.type != TEX_TYPE_2D || h.levels > 1u) return NULL;
  for (uint32_t c = 0; c < 4u; c++)
    if (h.data_type[c] != TEX_DATA_UNORM) return NULL;
  for (uint32_t i = 0; i < RASTER_SURFACES; i++) {
    const Raster3d_Surface *s = &r->surfaces[i];
    if (!s->in_use || !s->loaded || s->depth || s->address != h.address || s->format != RT_FORMAT_A8B8G8R8_UNORM ||
        s->width != h.width || s->height != h.height)
      continue;
    for (uint32_t k = 0; k < r->surface_view_count; k++) {
      Raster3d_Texture *v = &r->surface_views[k];
      if (v->image.texels == s->pixels && memcmp(v->tic, tic, sizeof(v->tic)) == 0) return v;
    }
    if (r->surface_view_count >= RASTER_SURFACE_VIEWS) return NULL; /* the round trip instead */
    /* Filled before any worker can see it (callers hold the pool lock and
     * publish it through their own resolver). */
    Raster3d_Texture *v = &r->surface_views[r->surface_view_count++];
    memset(v, 0, sizeof(*v));
    memcpy(v->tic, tic, sizeof(v->tic));
    v->address = h.address;
    v->valid = true;
    v->image.header = h;
    v->image.format = TEX_FORMAT_A8B8G8R8;
    v->image.bytes_per_texel = 4u;
    v->image.width = h.width;
    v->image.height = h.height;
    v->image.layers = 1u;
    v->image.row_bytes = h.width * 4u;
    v->image.layer_bytes = (uint64_t)h.width * h.height * 4u;
    v->image.texels = s->pixels;
    v->image.rgba8 = true;
    v->image.valid = true;
    return v;
  }
  return NULL;
}

/* ---- texture cache ------------------------------------------------ */

/* The least recently used texture the current draw has not touched (its
 * workers may be sampling every texture it has), or NULL. */
static Raster3d_Texture *texture_victim(Raster3d *r) {
  Raster3d_Texture *victim = NULL;
  for (uint32_t i = 0; i < RASTER_TEXTURES; i++) {
    Raster3d_Texture *t = &r->textures[i];
    if (t->valid && t->last_used != r->draw_serial && (!victim || t->last_used < victim->last_used)) victim = t;
  }
  return victim;
}

/* First-fit block of `bytes` in the pool, evicting least recently used
 * textures until one exists. SIZE_MAX: none (the current draw holds the
 * rest). */
static size_t pool_allocate(Raster3d *r, size_t bytes) {
  if (bytes > RASTER_TEXTURE_POOL_BYTES) return SIZE_MAX;
  for (;;) {
    /* Blocks in use, by offset (insertion sort; at most RASTER_TEXTURES). */
    size_t start[RASTER_TEXTURES], size[RASTER_TEXTURES];
    uint32_t n = 0;
    for (uint32_t i = 0; i < RASTER_TEXTURES; i++) {
      const Raster3d_Texture *t = &r->textures[i];
      if (!t->valid) continue;
      uint32_t k = n++;
      for (; k > 0 && start[k - 1u] > t->pool_offset; k--) {
        start[k] = start[k - 1u];
        size[k] = size[k - 1u];
      }
      start[k] = t->pool_offset;
      size[k] = t->pool_bytes;
    }
    size_t cursor = 0;
    for (uint32_t k = 0; k <= n; k++) {
      const size_t next = k < n ? start[k] : RASTER_TEXTURE_POOL_BYTES;
      if (next >= cursor && next - cursor >= bytes) return cursor;
      if (k < n && start[k] + size[k] > cursor) cursor = start[k] + size[k];
    }
    Raster3d_Texture *victim = texture_victim(r);
    if (!victim) return SIZE_MAX;
    victim->valid = false;
  }
}

static void texture_miss(Raster3d *r, const Tex_Header *h, const char *why) {
  if (!r->stats.texture_misses)
    log_warn("[gpu] texture %ux%u format 0x%02x layout %u type %u at %llx: %s", h->width, h->height, h->format, h->layout,
             h->type, (unsigned long long)h->address, why);
  r->stats.texture_misses++;
}

static Raster3d_Texture *texture_load(Raster3d *r, const uint32_t tic[8], const Gpu_Memory *mem) {
  Raster3d_Texture *view = surface_view(r, tic);
  if (view) return view;
  Raster3d_Texture *t = NULL;
  for (uint32_t i = 0; i < RASTER_TEXTURES; i++) {
    if (r->textures[i].valid && memcmp(r->textures[i].tic, tic, sizeof(r->textures[i].tic)) == 0) {
      t = &r->textures[i];
      break;
    }
  }
  if (t) t->last_used = r->draw_serial;
  if (t && t->validated == r->texture_epoch) return t;
  Tex_Header h;
  tex_header_parse(tic, &h);
  const uint64_t raw_bytes = tex_read_bytes(&h);
  const uint64_t decoded = tex_decoded_bytes(&h);
  const size_t decoded_aligned = (size_t)((decoded + 63u) & ~63ull);
  if (!raw_bytes || !decoded) {
    texture_miss(r, &h, "unsupported format");
    return NULL;
  }
  if (raw_bytes > RASTER_TEXTURE_RAW_BYTES || decoded_aligned > RASTER_TEXTURE_POOL_BYTES) {
    texture_miss(r, &h, "larger than the texture cache");
    return NULL;
  }
  /* A texture the GPU just rendered: write those pixels back first. */
  for (uint32_t i = 0; i < RASTER_SURFACES; i++) {
    Raster3d_Surface *s = &r->surfaces[i];
    if (s->in_use && s->dirty && s->address < h.address + raw_bytes && h.address < s->address + s->guest_bytes)
      surface_write_back(r, s, mem);
  }
  /* Unchanged since it was decoded? Hashed in place: the whole texture is
   * staged only to decode it. */
  uint64_t hash = 0;
  if (!guest_hash(mem, h.address, raw_bytes, &hash)) {
    texture_miss(r, &h, "unreadable");
    return NULL;
  }
  if (t && t->raw_hash == hash) {
    t->validated = r->texture_epoch;
    return t;
  }
  uint8_t *raw = r->texture_raw;
  if (!mem->read(mem->user, h.address, raw, raw_bytes)) {
    texture_miss(r, &h, "unreadable");
    return NULL;
  }
  /* A changed texture decodes into its own block (same descriptor, same
   * size); a new one takes a free slot and block, evicting as needed. */
  if (!t) {
    for (uint32_t i = 0; i < RASTER_TEXTURES && !t; i++)
      if (!r->textures[i].valid) t = &r->textures[i];
    if (!t) t = texture_victim(r);
    if (!t) {
      texture_miss(r, &h, "every cached texture is in use");
      return NULL;
    }
    t->valid = false;
    const size_t offset = pool_allocate(r, decoded_aligned);
    if (offset == SIZE_MAX) {
      texture_miss(r, &h, "texture cache full");
      return NULL;
    }
    t->pool_offset = offset;
    t->pool_bytes = decoded_aligned;
  }
  if (!tex_decode(&h, raw, r->texture_pool + t->pool_offset, &t->image)) {
    t->valid = false;
    r->stats.texture_misses++;
    return NULL;
  }
  log_debug("[gpu] texture %ux%u fmt 0x%02x types %u%u%u%u swizzle %u%u%u%u layout %u type %u srgb %d @%llx",
            h.width, h.height, h.format, h.data_type[0], h.data_type[1], h.data_type[2], h.data_type[3], h.swizzle[0],
            h.swizzle[1], h.swizzle[2], h.swizzle[3], h.layout, h.type, h.srgb, (unsigned long long)h.address);
  memcpy(t->tic, tic, sizeof(t->tic));
  t->address = h.address;
  t->raw_bytes = raw_bytes;
  t->raw_hash = hash;
  t->validated = r->texture_epoch;
  t->last_used = r->draw_serial;
  t->valid = true;
  if (r->on_texture_decoded) r->on_texture_decoded(r->on_texture_user, &t->image, h.address);
  return t;
}

/* Finds `handle`'s texture and sampler; caches it in `res`. A full cache
 * still resolves (uncached). */
static bool resolve_texture(Tex_Resolver *res, uint32_t handle, Raster3d_Texture **tex, Tex_Sampler *sampler) {
  for (uint32_t i = 0; i < res->count; i++) {
    if (res->handle[i] == handle) {
      *tex = res->texture[i];
      *sampler = res->sampler[i];
      return *tex != NULL;
    }
  }
  Draw_Context *ctx = res->ctx;
  const uint32_t *regs = ctx->regs;
  const uint32_t tic_index = handle & 0xfffffu;
  const uint32_t tsc_index = (regs[REG_SAMPLER_BINDING] & 1u) ? tic_index : (handle >> 20) & 0xfffu;
  const uint64_t tic_pool = addr40(regs[REG_TEX_HEADER_POOL], regs[REG_TEX_HEADER_POOL + 1u]);
  const uint64_t tsc_pool = addr40(regs[REG_SAMPLER_POOL], regs[REG_SAMPLER_POOL + 1u]);
  uint32_t tic[8], tsc[8];
  Raster3d_Texture *t = NULL;
  Tex_Sampler s;
  memset(&s, 0, sizeof(s));
  s.mag_filter = 1;
  /* The texture pool and guest reads are shared with the other bands. */
  workers_lock(&ctx->r->workers);
  if (tic_pool && ctx->mem->read(ctx->mem->user, tic_pool + (uint64_t)tic_index * TEX_HEADER_BYTES, tic, sizeof(tic)))
    t = texture_load(ctx->r, tic, ctx->mem);
  if (tsc_pool && ctx->mem->read(ctx->mem->user, tsc_pool + (uint64_t)tsc_index * TEX_SAMPLER_BYTES, tsc, sizeof(tsc)))
    tex_sampler_parse(tsc, &s);
  workers_unlock(&ctx->r->workers);
  if (res->count < RESOLVED_TEXTURES) {
    res->handle[res->count] = handle;
    res->texture[res->count] = t;
    res->sampler[res->count] = s;
    res->count++;
  }
  *tex = t;
  *sampler = s;
  return t != NULL;
}

/* One texture request against a resolved texture (NULL: unresolved). */
static void texture_request(const Raster3d_Texture *t, const Tex_Sampler *s, const Sm_Tex_Request *req,
                            uint32_t out[4]) {
  if (!t) {
    out[0] = out[1] = out[2] = 0;
    out[3] = u32f(1.0f);
    return;
  }
  const Tex_Image *img = &t->image;
  switch (req->kind) {
  case SM_TEX_FETCH:
    tex_fetch(img, req->icoords, (int32_t)req->layer, out);
    break;
  case SM_TEX_GATHER:
    tex_gather(img, s, req->coords, req->layer, req->gather_component, req->dref, req->shadow,
               req->has_offset ? req->offset : NULL, out);
    break;
  case SM_TEX_QUERY_DIMS:
    out[0] = img->width;
    out[1] = img->height;
    out[2] = img->header.type == TEX_TYPE_3D ? img->header.depth : img->layers;
    out[3] = img->header.levels;
    break;
  case SM_TEX_QUERY_LOD:
    out[0] = out[1] = out[2] = out[3] = 0;
    break;
  default:
    tex_sample(img, s, req->coords, req->layer, req->dref, req->shadow, req->has_offset ? req->offset : NULL, out);
    break;
  }
}

static void env_texture(void *user, const Sm_Tex_Request *req, uint32_t out[4]) {
  Raster3d_Texture *t = NULL;
  Tex_Sampler sampler;
  const bool ok = resolve_texture((Tex_Resolver *)user, req->handle, &t, &sampler);
  texture_request(ok ? t : NULL, &sampler, req, out);
}

/* A texture instruction's lanes: the handle is resolved once per run of
 * equal handles (usually the whole instruction). */
/* A plain 2D sample (layer 0, no offset, no compare): tex_sample_batch's. */
static bool plain_2d_sample(const Sm_Tex_Request *req) {
  return req->kind == SM_TEX_SAMPLE && req->dims == 2u && !req->array && !req->cube && !req->shadow && !req->has_offset;
}

static void env_texture_batch(void *user, const Sm_Tex_Request *requests, Sm_Mask lanes, uint32_t (*out)[4]) {
  Tex_Resolver *res = (Tex_Resolver *)user;
  bool have = false, ok = false;
  uint32_t handle = 0;
  Raster3d_Texture *t = NULL;
  Tex_Sampler sampler;
  /* The common case: every lane a plain 2D sample of one texture. */
  uint32_t first = 0;
  while (first < SM_LANES && !((lanes >> first) & 1u)) first++;
  if (first < SM_LANES) {
    const uint32_t h = requests[first].handle;
    bool uniform = true;
    for (uint32_t l = first; l < SM_LANES && uniform; l++)
      if ((lanes >> l) & 1u) uniform = requests[l].handle == h && plain_2d_sample(&requests[l]);
    if (uniform) {
      if (!resolve_texture(res, h, &t, &sampler)) {
        for (uint32_t l = first; l < SM_LANES; l++) {
          if (!((lanes >> l) & 1u)) continue;
          out[l][0] = out[l][1] = out[l][2] = 0;
          out[l][3] = u32f(1.0f);
        }
        return;
      }
      float u[SM_LANES], v[SM_LANES];
      uint32_t lane[SM_LANES], n = 0;
      uint32_t result[SM_LANES][4];
      for (uint32_t l = first; l < SM_LANES; l++) {
        if (!((lanes >> l) & 1u)) continue;
        u[n] = requests[l].coords[0];
        v[n] = requests[l].coords[1];
        lane[n++] = l;
      }
      tex_sample_batch(&t->image, &sampler, u, v, n, result);
      for (uint32_t i = 0; i < n; i++) memcpy(out[lane[i]], result[i], sizeof(result[i]));
      return;
    }
  }
  for (uint32_t l = 0; l < SM_LANES; l++) {
    if (!((lanes >> l) & 1u)) continue;
    const Sm_Tex_Request *req = &requests[l];
    if (!have || req->handle != handle) {
      handle = req->handle;
      ok = resolve_texture(res, handle, &t, &sampler);
      have = true;
    }
    texture_request(ok ? t : NULL, &sampler, req, out[l]);
  }
}

/* Shader global memory (LDG/STG): guest memory is shared with the other
 * bands, so accesses are serialised. */
static bool env_global_read(void *user, uint64_t va, void *out, uint32_t size) {
  const Draw_Context *ctx = ((const Tex_Resolver *)user)->ctx;
  workers_lock(&ctx->r->workers);
  const bool ok = ctx->mem->read(ctx->mem->user, va, out, size);
  workers_unlock(&ctx->r->workers);
  return ok;
}

static bool env_global_write(void *user, uint64_t va, const void *src, uint32_t size) {
  const Draw_Context *ctx = ((const Tex_Resolver *)user)->ctx;
  workers_lock(&ctx->r->workers);
  const bool ok = ctx->mem->write(ctx->mem->user, va, src, size);
  workers_unlock(&ctx->r->workers);
  return ok;
}

/* Binds the constant buffers `program` reads from `group`. */
static void env_setup(Draw_Context *ctx, uint32_t stage, const Sm_Program *program, const Raster3d_Bindings *b,
                      uint32_t group) {
  Sm_Env *env = &ctx->env[stage];
  memset(env, 0, sizeof(*env));
  env->user = &ctx->resolver;
  env->texture = env_texture;
  env->texture_batch = env_texture_batch;
  env->global_read = env_global_read;
  env->global_write = env_global_write;
  env->texture_cbuf_slot = ctx->regs[REG_BINDLESS_TEXTURE] & 0x1fu;
  uint32_t used = program->cbuf_used | (1u << env->texture_cbuf_slot);
  for (uint32_t slot = 0; slot < SM_CBUF_SLOTS; slot++) {
    if (!(used & (1u << slot)) || group >= RASTER_BIND_GROUPS || !b->address[group][slot]) continue;
    uint32_t size = b->size[group][slot];
    if (size > CBUF_SLOT_BYTES) size = CBUF_SLOT_BYTES;
    uint32_t want = program->cbuf_extent[slot];
    if (slot == env->texture_cbuf_slot || !want || want > size) want = size;
    uint8_t *dst = ctx->r->cbuf_data + ((size_t)stage * SM_CBUF_SLOTS + slot) * CBUF_SLOT_BYTES;
    if (!ctx->mem->read(ctx->mem->user, b->address[group][slot], dst, want)) continue;
    env->cbuf[slot] = dst;
    env->cbuf_size[slot] = want;
  }
}

/* ---- vertex fetch ------------------------------------------------- */

static bool stream_read(Draw_Context *ctx, uint32_t stream, uint64_t address, uint8_t *out, uint32_t size) {
  if (stream >= RASTER_STREAMS) return false;
  if (address >= ctx->window_base[stream] && address + size <= ctx->window_base[stream] + ctx->window_size[stream]) {
    memcpy(out, ctx->window[stream] + (address - ctx->window_base[stream]), size);
    return true;
  }
  /* Refill: the window starts at this element. */
  uint32_t want = RASTER_STREAM_WINDOW;
  if (!ctx->mem->read(ctx->mem->user, address, ctx->window[stream], want)) {
    /* Near the end of a mapping: try exactly what is needed. */
    if (!ctx->mem->read(ctx->mem->user, address, ctx->window[stream], size)) return false;
    want = size;
  }
  ctx->window_base[stream] = address;
  ctx->window_size[stream] = want;
  memcpy(out, ctx->window[stream], size);
  return true;
}

typedef struct Attrib_Layout {
  uint8_t count;
  uint8_t bits[4];
  uint8_t bytes;
} Attrib_Layout;

static bool attrib_layout(uint32_t size, Attrib_Layout *out) {
  memset(out, 0, sizeof(*out));
  switch (size) {
  case 0x01: *out = (Attrib_Layout){4, {32, 32, 32, 32}, 16}; return true;
  case 0x02: *out = (Attrib_Layout){3, {32, 32, 32, 0}, 12}; return true;
  case 0x03: *out = (Attrib_Layout){4, {16, 16, 16, 16}, 8}; return true;
  case 0x04: *out = (Attrib_Layout){2, {32, 32, 0, 0}, 8}; return true;
  case 0x05: *out = (Attrib_Layout){3, {16, 16, 16, 0}, 6}; return true;
  case 0x0A: case 0x2F: *out = (Attrib_Layout){4, {8, 8, 8, 8}, 4}; return true;
  case 0x0F: *out = (Attrib_Layout){2, {16, 16, 0, 0}, 4}; return true;
  case 0x12: *out = (Attrib_Layout){1, {32, 0, 0, 0}, 4}; return true;
  case 0x13: case 0x33: *out = (Attrib_Layout){3, {8, 8, 8, 0}, size == 0x33 ? 4 : 3}; return true;
  case 0x18: case 0x32: *out = (Attrib_Layout){2, {8, 8, 0, 0}, 2}; return true;
  case 0x1B: *out = (Attrib_Layout){1, {16, 0, 0, 0}, 2}; return true;
  case 0x1D: *out = (Attrib_Layout){1, {8, 0, 0, 0}, 1}; return true;
  case 0x30: *out = (Attrib_Layout){4, {10, 10, 10, 2}, 4}; return true;
  case 0x31: *out = (Attrib_Layout){3, {11, 11, 10, 0}, 4}; return true;
  case 0x34: *out = (Attrib_Layout){1, {8, 0, 0, 0}, 1}; return true;
  default: return false;
  }
}

#define NUM_SNORM 1u
#define NUM_UNORM 2u
#define NUM_SINT 3u
#define NUM_UINT 4u
#define NUM_USCALED 5u
#define NUM_SSCALED 6u
#define NUM_FLOAT 7u

static void fetch_attribute(Draw_Context *ctx, uint32_t attrib, uint32_t vertex, uint32_t out[4]) {
  const uint32_t *regs = ctx->regs;
  const uint32_t a = regs[REG_VERTEX_ATTRIB + attrib];
  const uint32_t type = (a >> 27) & 7u;
  const bool integer = type == NUM_SINT || type == NUM_UINT;
  out[0] = out[1] = out[2] = 0;
  out[3] = integer ? 1u : u32f(1.0f);
  if ((a >> 6) & 1u) return; /* inactive: constant default */
  const uint32_t stream = a & 0x1fu;
  const uint32_t *st = regs + REG_STREAM + 4u * stream;
  if (!((st[0] >> 12) & 1u)) return;
  Attrib_Layout layout;
  if (!attrib_layout((a >> 21) & 0x3fu, &layout)) return;
  const uint32_t stride = st[0] & 0xfffu;
  uint32_t element = vertex;
  if (regs[REG_STREAM_INSTANCED + stream] & 1u) {
    const uint32_t divisor = st[3] ? st[3] : 1u;
    element = ctx->instance / divisor + regs[REG_BASE_INSTANCE];
  }
  const uint64_t address = addr40(st[1], st[2]) + (uint64_t)element * stride + ((a >> 7) & 0x3fffu);
  uint8_t raw[16];
  memset(raw, 0, sizeof(raw));
  if (!stream_read(ctx, stream, address, raw, layout.bytes)) return;
  uint32_t bit = 0;
  for (uint32_t c = 0; c < layout.count; c++) {
    const uint32_t n = layout.bits[c];
    const uint64_t v = bits_at(raw, bit, n);
    bit += n;
    const uint64_t max = n >= 32u ? 0xffffffffull : ((1ull << n) - 1ull);
    const int64_t sv = n >= 64u ? (int64_t)v : (int64_t)(v << (64u - n)) >> (64u - n);
    uint32_t value;
    switch (type) {
    case NUM_SNORM: {
      const float x = (float)sv / (float)(max >> 1);
      value = u32f(x < -1.0f ? -1.0f : x);
      break;
    }
    case NUM_UNORM: value = u32f((float)((double)v / (double)max)); break;
    case NUM_SINT: value = (uint32_t)sv; break;
    case NUM_UINT: value = (uint32_t)v; break;
    case NUM_USCALED: value = u32f((float)v); break;
    case NUM_SSCALED: value = u32f((float)sv); break;
    default: /* FLOAT */
      if (n == 32u) value = (uint32_t)v;
      else if (n == 16u) value = u32f(half_to_float((uint32_t)v));
      else if (n == 11u) value = u32f(half_to_float((uint32_t)v << 4));
      else if (n == 10u) value = u32f(half_to_float((uint32_t)v << 5));
      else value = 0;
      break;
    }
    out[c] = value;
  }
  if (((a >> 21) & 0x3fu) == 0x34u) { /* A8: alpha only */
    out[3] = out[0];
    out[0] = 0;
  }
  if ((a >> 31) & 1u) {
    const uint32_t t = out[0];
    out[0] = out[2];
    out[2] = t;
  }
}

/* ---- vertices ----------------------------------------------------- */

typedef struct Vertex {
  float pos[4];                  /* clip space */
  uint32_t varying[MAX_VARYINGS];
} Vertex;

typedef struct Varying_Set {
  uint32_t count;
  uint8_t word[MAX_VARYINGS];    /* generic component index (vector * 4 + comp) */
  uint8_t interp[MAX_VARYINGS];
} Varying_Set;

typedef struct Vertex_Cache {
  uint32_t index[VERTEX_CACHE];
  bool valid[VERTEX_CACHE];
  Vertex vertex[VERTEX_CACHE];
} Vertex_Cache;

/* Shades `n` (<= SM_LANES) vertices at once, one per lane. */
static bool run_vertices(Draw_Context *ctx, const uint32_t *indices, uint32_t n, Vertex *const *out) {
  Sm_Thread *t = ctx->r->thread;
  sm_thread_reset(t, n);
  const Sm_Header *h = &ctx->vs->header;
  for (uint32_t v = 0; v < SM_ATTR_GENERIC_COUNT; v++) {
    const uint32_t mask = (h->input_generic[v / 8u] >> ((v % 8u) * 4u)) & 0xfu;
    const uint32_t base = (SM_ATTR_GENERIC / 4u) + v * 4u;
    if (!mask) continue;
    for (uint32_t l = 0; l < n; l++) {
      uint32_t value[4];
      fetch_attribute(ctx, v, indices[l], value);
      for (uint32_t c = 0; c < 4; c++) t->attr_in[base + c][l] = value[c];
    }
  }
  memset(t->attr_out, 0, sizeof(t->attr_out));
  for (uint32_t l = 0; l < n; l++) {
    t->attr_out[(SM_ATTR_POSITION / 4u) + 3u][l] = u32f(1.0f);
    t->vertex_id[l] = indices[l];
    t->instance_id[l] = ctx->instance;
  }
  if (!sm_run(ctx->vs, &ctx->env[0], t)) {
    ctx->r->stats.shader_faults++;
    return false;
  }
  for (uint32_t l = 0; l < n; l++) {
    for (uint32_t c = 0; c < 4; c++) out[l]->pos[c] = f32(t->attr_out[SM_ATTR_POSITION / 4u + c][l]);
    for (uint32_t i = 0; i < MAX_VARYINGS; i++) out[l]->varying[i] = t->attr_out[SM_ATTR_GENERIC / 4u + i][l];
  }
  return true;
}

static const Vertex *vertex_get(Draw_Context *ctx, Vertex_Cache *cache, uint32_t index, bool *ok) {
  const uint32_t slot = index % VERTEX_CACHE;
  if (cache->valid[slot] && cache->index[slot] == index) return &cache->vertex[slot];
  Vertex *out = &cache->vertex[slot];
  cache->valid[slot] = run_vertices(ctx, &index, 1u, &out);
  cache->index[slot] = index;
  *ok = cache->valid[slot];
  return &cache->vertex[slot];
}

/* Shades the not-yet-cached vertices among `indices` (a chunk the
 * assembler is about to consume) SM_LANES at a time. Indices that collide
 * in the cache are left to vertex_get. */
static void vertex_prefetch(Draw_Context *ctx, Vertex_Cache *cache, const uint32_t *indices, uint32_t count) {
  uint32_t batch[SM_LANES];
  Vertex *out[SM_LANES];
  uint32_t n = 0;
  for (uint32_t i = 0; i <= count; i++) {
    if (i < count) {
      const uint32_t index = indices[i], slot = index % VERTEX_CACHE;
      if (cache->valid[slot] && cache->index[slot] == index) continue;
      bool queued = false;
      for (uint32_t k = 0; k < n && !queued; k++) queued = batch[k] == index || batch[k] % VERTEX_CACHE == slot;
      if (queued) continue;
      batch[n] = index;
      out[n] = &cache->vertex[slot];
      n++;
    }
    if (n == SM_LANES || (i == count && n)) {
      const bool ok = run_vertices(ctx, batch, n, out);
      for (uint32_t k = 0; k < n; k++) {
        const uint32_t slot = batch[k] % VERTEX_CACHE;
        cache->valid[slot] = ok;
        cache->index[slot] = batch[k];
      }
      n = 0;
    }
  }
}

/* ---- per-draw raster state ---------------------------------------- */

typedef struct Target {
  Raster3d_Surface *surface;
  const Color_Format *format;
  bool rgba8;                    /* 4 x 8-bit UNORM */
  bool alpha_blend;              /* colour = src*srcA + dst*(1-srcA) */
  uint32_t write_mask;           /* bit per component */
  bool blend;
  uint32_t color_op, color_src, color_dst, alpha_op, alpha_src, alpha_dst;
  uint32_t rgba8_blend;          /* RGBA8_BLEND_*: a common blend on an RGBA8 target, done inline */
} Target;

typedef struct Raster_State {
  Draw_Context *ctx;
  Target targets[MAX_TARGETS];
  uint32_t target_count;
  bool mrt;
  Raster3d_Surface *depth;
  bool depth_test, depth_write;
  uint32_t depth_func;
  bool alpha_test;
  uint32_t alpha_func;
  float alpha_ref;
  /* Stencil (front = [0], back = [1]); `front` is the current triangle's facing. */
  bool stencil;
  uint32_t stencil_byte;
  uint32_t stencil_op_fail[2], stencil_op_zfail[2], stencil_op_zpass[2], stencil_func[2];
  uint8_t stencil_ref[2], stencil_func_mask[2], stencil_write_mask[2];
  bool front;
  float blend_const[4];
  Rect clip;
  int32_t surface_height;
  float vp_scale[3], vp_offset[3];
  bool viewport_transform;
  bool lower_left;
  bool z_zero_to_one;
  bool cull;
  uint32_t cull_face;
  bool front_ccw;
  uint32_t provoking_last;
  Varying_Set varyings;
  /* the pixel program's colour register layout */
  uint8_t color_reg[MAX_TARGETS][4]; /* 0xff = not written */
  uint32_t depth_reg;                /* 0xff = none */
  uint32_t out_regs;                 /* output registers R0..R(out_regs-1) */
  /* Flat triangles over plain RGBA8 targets fill whole spans (§13
   * reference renderer): one shader result, so every target's output
   * byte is a function of the destination byte alone - a lookup table
   * when blending, a constant otherwise. */
  bool span_ok;                      /* draw state allows span fills */
  bool span_blend[MAX_TARGETS];
  uint32_t span_word[MAX_TARGETS];
  uint8_t span_lut[MAX_TARGETS][4][256];
  /* Pixel work: the rows this state shades now ([row_begin, row_end), one
   * band at a time), its shader state, texture resolver and statistics.
   * Each worker has its own copy. */
  int64_t row_begin, row_end;
  Sm_Thread *thread;
  Sm_Env ps_env;
  Tex_Resolver resolver;
  Raster3d_Stats stats;
} Raster_State;

/* One pixel-program result, reused across a triangle whose inputs do not
 * vary. */
typedef struct Frag_Result {
  bool valid;
  bool killed;
  uint32_t regs[MAX_TARGETS * 4u + 1u];
} Frag_Result;

/* Blend factors and ops, OGL and D3D enums folded together. */
typedef enum Blend_Factor {
  BF_ZERO, BF_ONE, BF_SRC_COLOR, BF_INV_SRC_COLOR, BF_SRC_ALPHA, BF_INV_SRC_ALPHA, BF_DST_ALPHA, BF_INV_DST_ALPHA,
  BF_DST_COLOR, BF_INV_DST_COLOR, BF_SRC_ALPHA_SAT, BF_CONST_COLOR, BF_INV_CONST_COLOR, BF_CONST_ALPHA,
  BF_INV_CONST_ALPHA,
} Blend_Factor;

static Blend_Factor blend_factor(uint32_t v) {
  switch (v) {
  case 0x4000: case 1: return BF_ZERO;
  case 0x4001: case 2: return BF_ONE;
  case 0x4300: case 3: return BF_SRC_COLOR;
  case 0x4301: case 4: return BF_INV_SRC_COLOR;
  case 0x4302: case 5: return BF_SRC_ALPHA;
  case 0x4303: case 6: return BF_INV_SRC_ALPHA;
  case 0x4304: case 7: return BF_DST_ALPHA;
  case 0x4305: case 8: return BF_INV_DST_ALPHA;
  case 0x4306: case 9: return BF_DST_COLOR;
  case 0x4307: case 10: return BF_INV_DST_COLOR;
  case 0x4308: case 11: return BF_SRC_ALPHA_SAT;
  case 0xC001: case 14: return BF_CONST_COLOR;
  case 0xC002: case 15: return BF_INV_CONST_COLOR;
  case 0xC003: return BF_CONST_ALPHA;
  case 0xC004: return BF_INV_CONST_ALPHA;
  default: return BF_ONE;
  }
}

#define BOP_ADD 0u
#define BOP_SUB 1u
#define BOP_REV_SUB 2u
#define BOP_MIN 3u
#define BOP_MAX 4u

static uint32_t blend_op(uint32_t v) {
  switch (v) {
  case 0x800A: case 2: return BOP_SUB;
  case 0x800B: case 3: return BOP_REV_SUB;
  case 0x8007: case 4: return BOP_MIN;
  case 0x8008: case 5: return BOP_MAX;
  default: return BOP_ADD;
  }
}

static float factor_value(Blend_Factor f, uint32_t c, const float src[4], const float dst[4], const float k[4]) {
  switch (f) {
  case BF_ZERO: return 0.0f;
  case BF_ONE: return 1.0f;
  case BF_SRC_COLOR: return src[c];
  case BF_INV_SRC_COLOR: return 1.0f - src[c];
  case BF_SRC_ALPHA: return src[3];
  case BF_INV_SRC_ALPHA: return 1.0f - src[3];
  case BF_DST_ALPHA: return dst[3];
  case BF_INV_DST_ALPHA: return 1.0f - dst[3];
  case BF_DST_COLOR: return dst[c];
  case BF_INV_DST_COLOR: return 1.0f - dst[c];
  case BF_SRC_ALPHA_SAT: return c == 3u ? 1.0f : fminf(src[3], 1.0f - dst[3]);
  case BF_CONST_COLOR: return k[c];
  case BF_INV_CONST_COLOR: return 1.0f - k[c];
  case BF_CONST_ALPHA: return k[3];
  default: return 1.0f - k[3];
  }
}

static float apply_op(uint32_t op, float s, float d) {
  switch (op) {
  case BOP_SUB: return s - d;
  case BOP_REV_SUB: return d - s;
  case BOP_MIN: return fminf(s, d);
  case BOP_MAX: return fmaxf(s, d);
  default: return s + d;
  }
}

static bool depth_compare(uint32_t func, float incoming, float stored) {
  /* OGL 0x200-0x207 (NEVER..ALWAYS) or D3D 1-8. */
  uint32_t f = func >= 0x200u ? func - 0x200u : (func >= 1u ? func - 1u : 7u);
  switch (f & 7u) {
  case 0: return false;
  case 1: return incoming < stored;
  case 2: return incoming == stored;
  case 3: return incoming <= stored;
  case 4: return incoming > stored;
  case 5: return incoming != stored;
  case 6: return incoming >= stored;
  default: return true;
  }
}

/* Blends output_pixel does inline on RGBA8 targets: the same float
 * operations as its generic path (products and sum as separate
 * expressions, so nothing contracts differently), without the per-channel
 * factor and op dispatch. Unity draws sprites premultiplied. */
#define RGBA8_BLEND_GENERIC 0u
#define RGBA8_BLEND_PREMULTIPLIED 1u /* ONE, ONE_MINUS_SRC_ALPHA, ADD (colour and alpha) */
#define RGBA8_BLEND_ADDITIVE 2u      /* ONE, ONE, ADD */

static uint32_t rgba8_blend_kind(const Target *t) {
  if (!t->rgba8 || !t->blend || t->write_mask != 0xfu || t->color_op != BOP_ADD || t->alpha_op != BOP_ADD) return RGBA8_BLEND_GENERIC;
  if (t->color_src != BF_ONE || t->alpha_src != BF_ONE) return RGBA8_BLEND_GENERIC;
  if (t->color_dst == BF_INV_SRC_ALPHA && t->alpha_dst == BF_INV_SRC_ALPHA) return RGBA8_BLEND_PREMULTIPLIED;
  if (t->color_dst == BF_ONE && t->alpha_dst == BF_ONE) return RGBA8_BLEND_ADDITIVE;
  return RGBA8_BLEND_GENERIC;
}

static void output_rgba8_blend(const Target *tg, const uint32_t color[4], uint8_t *p) {
  float dst[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  for (uint32_t i2 = 0; i2 < 4u; i2++)
    if (tg->format->channel[i2] != CH_PAD) dst[tg->format->channel[i2]] = g_unorm8[p[i2]];
  float out[4];
  const float df = tg->rgba8_blend == RGBA8_BLEND_PREMULTIPLIED ? 1.0f - f32(color[3]) : 1.0f;
  for (uint32_t c = 0; c < 4u; c++) {
    const float s = f32(color[c]) * 1.0f;
    const float d = dst[c] * df;
    out[c] = s + d;
  }
  for (uint32_t i2 = 0; i2 < 4u; i2++) p[i2] = tg->format->channel[i2] == CH_PAD ? 0xffu : to_unorm8(out[tg->format->channel[i2]]);
}

static bool setup_state(Draw_Context *ctx, Raster_State *rs) {
  Raster3d *r = ctx->r;
  const uint32_t *regs = ctx->regs;
  memset(rs, 0, offsetof(Raster_State, span_lut)); /* the tables are rebuilt per triangle */
  rs->ctx = ctx;
  const uint32_t select = regs[REG_CT_SELECT];
  const uint32_t count = select & 0xfu;
  rs->mrt = (regs[REG_CT_MRT_ENABLE] & 1u) != 0;
  const bool per_target = (regs[REG_BLEND_PER_TARGET] & 1u) != 0;
  int32_t width = 0x7fffffff, height = 0x7fffffff;
  for (uint32_t i = 0; i < count && i < MAX_TARGETS; i++) {
    const uint32_t target = (select >> (4u + 3u * i)) & 7u;
    Surface_Desc d;
    Target *t = &rs->targets[i];
    if (!color_target_desc(regs, target, &d)) continue;
    t->format = color_format(d.format);
    const uint32_t write = (regs[REG_SINGLE_CT_WRITE] & 1u) ? regs[REG_CT_WRITE] : regs[REG_CT_WRITE + i];
    for (uint32_t c = 0; c < 4; c++)
      if ((write >> (4u * c)) & 1u) t->write_mask |= 1u << c;
    if (!t->write_mask) continue;
    t->surface = surface_get(r, &d, ctx->mem, true);
    t->blend = (regs[REG_BLEND_ENABLE + i] & 1u) != 0 && t->format->kind != KIND_UINT && t->format->kind != KIND_SINT;
    if (per_target) {
      const uint32_t *b = regs + REG_BLEND_TARGET + 8u * i;
      const bool separate = (b[0] & 1u) != 0;
      t->color_op = blend_op(b[1]);
      t->color_src = blend_factor(b[2]);
      t->color_dst = blend_factor(b[3]);
      t->alpha_op = separate ? blend_op(b[4]) : t->color_op;
      t->alpha_src = separate ? blend_factor(b[5]) : t->color_src;
      t->alpha_dst = separate ? blend_factor(b[6]) : t->color_dst;
    } else {
      const bool separate = (regs[REG_BLEND_SEPARATE] & 1u) != 0;
      t->color_op = blend_op(regs[REG_BLEND_COLOR_OP]);
      t->color_src = blend_factor(regs[REG_BLEND_COLOR_SRC]);
      t->color_dst = blend_factor(regs[REG_BLEND_COLOR_DST]);
      t->alpha_op = separate ? blend_op(regs[REG_BLEND_ALPHA_OP]) : t->color_op;
      t->alpha_src = separate ? blend_factor(regs[REG_BLEND_ALPHA_SRC]) : t->color_src;
      t->alpha_dst = separate ? blend_factor(regs[REG_BLEND_ALPHA_DST]) : t->color_dst;
    }
    t->rgba8 = is_unorm8x4(t->format);
    t->alpha_blend = t->blend && t->color_op == BOP_ADD && t->color_src == BF_SRC_ALPHA && t->color_dst == BF_INV_SRC_ALPHA;
    t->rgba8_blend = rgba8_blend_kind(t);
    if ((int32_t)d.width < width) width = (int32_t)d.width;
    if ((int32_t)d.height < height) height = (int32_t)d.height;
    rs->target_count = i + 1u;
  }
  Surface_Desc zd;
  if (zeta_desc(regs, &zd)) {
    rs->depth_test = (regs[REG_DEPTH_TEST] & 1u) != 0;
    rs->depth_write = (regs[REG_DEPTH_WRITE] & 1u) != 0;
    rs->depth_func = regs[REG_DEPTH_FUNC];
    rs->stencil_byte = stencil_byte(zd.format);
    rs->stencil = (regs[REG_STENCIL_ENABLE] & 1u) != 0 && rs->stencil_byte != ZT_NO_STENCIL;
    if (rs->stencil) {
      const bool two_sided = (regs[REG_STENCIL_TWO_SIDED] & 1u) != 0;
      for (uint32_t f = 0; f < 2u; f++) {
        const uint32_t *ops = (f == 1u && two_sided) ? regs + REG_STENCIL_BACK : regs + REG_STENCIL_FRONT;
        rs->stencil_op_fail[f] = ops[0];
        rs->stencil_op_zfail[f] = ops[1];
        rs->stencil_op_zpass[f] = ops[2];
        rs->stencil_func[f] = ops[3];
        const bool back = f == 1u && two_sided;
        rs->stencil_ref[f] = (uint8_t)(back ? regs[REG_BACK_STENCIL_REF] : regs[REG_STENCIL_FRONT + 4u]);
        rs->stencil_write_mask[f] = (uint8_t)(back ? regs[REG_BACK_STENCIL_REF + 1u] : regs[REG_STENCIL_FRONT + 6u]);
        rs->stencil_func_mask[f] = (uint8_t)(back ? regs[REG_BACK_STENCIL_REF + 2u] : regs[REG_STENCIL_FRONT + 5u]);
      }
    }
    if (rs->depth_test || rs->depth_write || rs->stencil) {
      rs->depth = surface_get(r, &zd, ctx->mem, true);
      if ((int32_t)zd.width < width) width = (int32_t)zd.width;
      if ((int32_t)zd.height < height) height = (int32_t)zd.height;
    }
  }
  if (!rs->target_count && !rs->depth) return false;
  rs->alpha_test = (regs[REG_ALPHA_TEST] & 1u) != 0;
  rs->alpha_func = regs[REG_ALPHA_FUNC];
  rs->alpha_ref = f32(regs[REG_ALPHA_REF]);
  for (uint32_t c = 0; c < 4; c++) rs->blend_const[c] = f32(regs[REG_BLEND_CONST + c]);
  rs->surface_height = height;
  rs->clip = (Rect){0, 0, width, height};
  rect_scissor(&rs->clip, regs);
  rect_viewport_clip(&rs->clip, regs);
  {
    const uint32_t h = regs[REG_SURFACE_CLIP_H], v = regs[REG_SURFACE_CLIP_V];
    if (h >> 16 && v >> 16)
      rect_intersect(&rs->clip, (int32_t)(h & 0xffffu), (int32_t)(v & 0xffffu), (int32_t)(h & 0xffffu) + (int32_t)(h >> 16),
                     (int32_t)(v & 0xffffu) + (int32_t)(v >> 16));
  }
  if (rs->clip.x0 >= rs->clip.x1 || rs->clip.y0 >= rs->clip.y1) return false;
  for (uint32_t c = 0; c < 3; c++) {
    rs->vp_scale[c] = f32(regs[REG_VIEWPORT + c]);
    rs->vp_offset[c] = f32(regs[REG_VIEWPORT + 3u + c]);
  }
  rs->viewport_transform = (regs[REG_VIEWPORT_SCALE_OFFSET] & 1u) != 0;
  rs->lower_left = (regs[REG_WINDOW_ORIGIN] & 1u) != 0;
  rs->z_zero_to_one = (regs[REG_Z_CLIP_RANGE] & 1u) != 0;
  rs->cull = (regs[REG_CULL_ENABLE] & 1u) != 0;
  rs->cull_face = regs[REG_CULL_FACE];
  rs->front_ccw = regs[REG_FRONT_FACE] == 0x901u;
  rs->provoking_last = regs[REG_PROVOKING_VERTEX] & 1u;
  /* Varyings the pixel program reads, and how. */
  const Sm_Header *ph = &ctx->ps->header;
  for (uint32_t i = 0; i < MAX_VARYINGS; i++) {
    if (ph->input_interp[i] == SM_INTERP_UNUSED) continue;
    rs->varyings.word[rs->varyings.count] = (uint8_t)i;
    rs->varyings.interp[rs->varyings.count] = ph->input_interp[i];
    rs->varyings.count++;
  }
  /* Output registers: colour components in target order, then depth. */
  uint32_t reg = 0;
  memset(rs->color_reg, 0xff, sizeof(rs->color_reg));
  for (uint32_t t = 0; t < MAX_TARGETS; t++)
    for (uint32_t c = 0; c < 4; c++)
      if ((ph->omap_target >> (4u * t + c)) & 1u) rs->color_reg[t][c] = (uint8_t)reg++;
  rs->depth_reg = ph->omap_depth ? reg : 0xffu;
  rs->out_regs = reg + (ph->omap_depth ? 1u : 0u);
  return true;
}

/* ---- triangle rasterization --------------------------------------- */

typedef struct Screen_Vertex {
  float x, y, z, inv_w;
  int64_t fx, fy;
  const Vertex *src;
  float varying[MAX_VARYINGS];   /* pre-divided per interpolation mode */
} Screen_Vertex;

typedef struct Plane {
  float a, b, c; /* value = a*(x - x0) + b*(y - y0) + c */
} Plane;

static void plane_setup(Plane *p, const Screen_Vertex *v0, const Screen_Vertex *v1, const Screen_Vertex *v2, float f0,
                        float f1, float f2, float inv_area2) {
  const float dx1 = v1->x - v0->x, dy1 = v1->y - v0->y, dx2 = v2->x - v0->x, dy2 = v2->y - v0->y;
  const float df1 = f1 - f0, df2 = f2 - f0;
  p->a = (df1 * dy2 - df2 * dy1) * inv_area2;
  p->b = (df2 * dx1 - df1 * dx2) * inv_area2;
  p->c = f0;
}

static float plane_at(const Plane *p, float dx, float dy) { return p->a * dx + p->b * dy + p->c; }

/* Loads one pixel's inputs into lane `l`. */
static void setup_lane(const Raster_State *rs, Sm_Thread *t, uint32_t l, int32_t px, int32_t py, float depth,
                       const Plane *planes, const Plane *inv_w, float x0, float y0, const Screen_Vertex *provoking) {
  const float cx = (float)px + 0.5f, cy = (float)py + 0.5f;
  const float dx = cx - x0, dy = cy - y0;
  const float fy = rs->lower_left ? (float)rs->surface_height - cy : cy;
  t->attr_in[SM_ATTR_POSITION / 4u + 0u][l] = u32f(cx);
  t->attr_in[SM_ATTR_POSITION / 4u + 1u][l] = u32f(fy);
  t->attr_in[SM_ATTR_POSITION / 4u + 2u][l] = u32f(depth);
  t->attr_in[SM_ATTR_POSITION / 4u + 3u][l] = u32f(plane_at(inv_w, dx, dy));
  for (uint32_t i = 0; i < rs->varyings.count; i++) {
    const uint32_t word = SM_ATTR_GENERIC / 4u + rs->varyings.word[i];
    if (rs->varyings.interp[i] == SM_INTERP_CONSTANT)
      t->attr_in[word][l] = provoking->src->varying[rs->varyings.word[i]];
    else
      t->attr_in[word][l] = u32f(plane_at(&planes[i], dx, dy));
  }
}

static bool early_depth_reject(const Raster_State *rs, int32_t px, int32_t py, float depth) {
  if (!rs->depth || !rs->depth_test || rs->depth_reg != 0xffu || rs->stencil) return false;
  const uint8_t *zp = rs->depth->pixels + ((uint64_t)py * rs->depth->width + (uint64_t)px) * rs->depth->bytes_per_pixel;
  return !depth_compare(rs->depth_func, depth, read_depth(rs->depth->format, zp));
}

static void output_pixel(Raster_State *rs, int32_t px, int32_t py, float depth, const uint32_t *out_regs);

/* Runs the pixel program once for a flat triangle, into `shared`. */
static void shade_shared(Raster_State *rs, int32_t px, int32_t py, float depth, const Plane *planes, const Plane *inv_w,
                         float x0, float y0, bool front, const Screen_Vertex *provoking, Frag_Result *shared) {
  Sm_Thread *t = rs->thread;
  /* A program that reads its quad neighbours gets a whole quad of the
   * same pixel: its derivatives are then exactly zero, as they are on a
   * triangle whose inputs do not vary. */
  const uint32_t lanes = rs->ctx->ps->uses_quads ? 4u : 1u;
  sm_thread_reset_light(t, lanes);
  t->front_facing = front ? SM_ALL_LANES : 0;
  for (uint32_t l = 0; l < lanes; l++)
    setup_lane(rs, t, l, px, py, depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth), planes, inv_w, x0, y0, provoking);
  const bool ok = sm_run(rs->ctx->ps, &rs->ps_env, t);
  if (!ok) rs->stats.shader_faults++;
  shared->killed = !ok || (t->killed & 1u);
  for (uint32_t i = 0; i < rs->out_regs; i++) shared->regs[i] = t->r[i][0];
  shared->valid = true;
}

/* Shades one pixel, or reuses `shared` (a flat triangle's one result). */
static void shade_pixel(Raster_State *rs, int32_t px, int32_t py, float depth, const Plane *planes, const Plane *inv_w,
                        float x0, float y0, bool front, const Screen_Vertex *provoking, Frag_Result *shared) {
  if (early_depth_reject(rs, px, py, depth)) return;
  if (!shared->valid) shade_shared(rs, px, py, depth, planes, inv_w, x0, y0, front, provoking, shared);
  if (!shared->killed) output_pixel(rs, px, py, depth, shared->regs);
}

/* Pixels waiting to be shaded together. */
typedef struct Pixel_Batch {
  uint32_t count;
  uint32_t live;                 /* lanes that are covered pixels (others: quad helpers) */
  int32_t x[SM_LANES], y[SM_LANES];
  float z[SM_LANES];
} Pixel_Batch;

static void shade_batch(Raster_State *rs, Pixel_Batch *b, const Plane *planes, const Plane *inv_w, float x0, float y0,
                        bool front, const Screen_Vertex *provoking) {
  if (!b->count) return;
  Sm_Thread *t = rs->thread;
  sm_thread_reset_light(t, b->count);
  t->front_facing = front ? SM_ALL_LANES : 0;
  for (uint32_t l = 0; l < b->count; l++) setup_lane(rs, t, l, b->x[l], b->y[l], b->z[l], planes, inv_w, x0, y0, provoking);
  const bool ok = sm_run(rs->ctx->ps, &rs->ps_env, t);
  if (!ok) rs->stats.shader_faults++;
  for (uint32_t l = 0; ok && l < b->count; l++) {
    if ((t->killed >> l) & 1u || !((b->live >> l) & 1u)) continue;
    uint32_t regs[MAX_TARGETS * 4u + 1u];
    for (uint32_t i = 0; i < rs->out_regs; i++) regs[i] = t->r[i][l];
    output_pixel(rs, b->x[l], b->y[l], b->z[l], regs);
  }
  b->count = 0;
  b->live = 0;
}

static bool target_is_plain_rgba8(const Target *tg) {
  return tg->rgba8 && tg->write_mask == 0xfu && (!tg->blend || tg->alpha_blend);
}

/* RGBA8 colour write, unblended or alpha-blended (target_is_plain_rgba8).
 * Each output byte depends only on the same byte of `p`: colour channels
 * blend with their own destination value, alpha with destination alpha. */
static void output_rgba8(const Raster_State *rs, const Target *tg, const uint32_t color[4], uint8_t *p) {
  float src[4], dst[4];
  for (uint32_t c = 0; c < 4; c++) src[c] = f32(color[c]);
  if (tg->blend) {
    for (uint32_t i2 = 0; i2 < 4u; i2++)
      if (tg->format->channel[i2] != CH_PAD) dst[tg->format->channel[i2]] = g_unorm8[p[i2]];
    dst[3] = tg->format->channel[3] == CH_PAD ? 1.0f : dst[3];
    const float a = src[3], ia = 1.0f - src[3];
    for (uint32_t c = 0; c < 3u; c++) src[c] = src[c] * a + dst[c] * ia;
    const float sf = factor_value((Blend_Factor)tg->alpha_src, 3, src, dst, rs->blend_const);
    const float df = factor_value((Blend_Factor)tg->alpha_dst, 3, src, dst, rs->blend_const);
    src[3] = (tg->alpha_op == BOP_MIN || tg->alpha_op == BOP_MAX) ? apply_op(tg->alpha_op, a, dst[3])
                                                                   : apply_op(tg->alpha_op, a * sf, dst[3] * df);
  }
  for (uint32_t i2 = 0; i2 < 4u; i2++)
    p[i2] = tg->format->channel[i2] == CH_PAD ? 0xffu : to_unorm8(src[tg->format->channel[i2]]);
}

/* The colour a target receives from one shader result (output_pixel's
 * register selection). */
static void target_color(const Raster_State *rs, uint32_t target, const uint32_t *out_regs, uint32_t color[4]) {
  const uint32_t src_target = rs->mrt ? target : 0u;
  for (uint32_t c = 0; c < 4; c++) {
    const uint8_t reg = rs->color_reg[src_target][c];
    color[c] = reg == 0xffu ? (c == 3u ? u32f(1.0f) : 0u) : out_regs[reg];
  }
}

/* Whether this draw's fixed-function state lets flat triangles fill spans:
 * no depth or alpha test, no depth write, every bound target plain RGBA8. */
static bool span_state_ok(const Raster_State *rs) {
  if (rs->alpha_test || rs->stencil || (rs->depth && (rs->depth_test || rs->depth_write))) return false;
  for (uint32_t i = 0; i < rs->target_count; i++)
    if (rs->targets[i].surface && !target_is_plain_rgba8(&rs->targets[i])) return false;
  return true;
}

/* Builds the per-target fill for one flat triangle's shader result. */
static void span_prepare(Raster_State *rs, const uint32_t *out_regs) {
  for (uint32_t i = 0; i < rs->target_count; i++) {
    const Target *tg = &rs->targets[i];
    if (!tg->surface) continue;
    uint32_t color[4];
    target_color(rs, i, out_regs, color);
    rs->span_blend[i] = tg->blend;
    if (!tg->blend) {
      uint8_t bytes[4] = {0, 0, 0, 0};
      output_rgba8(rs, tg, color, bytes);
      memcpy(&rs->span_word[i], bytes, 4);
      continue;
    }
    for (uint32_t v = 0; v < 256u; v++) {
      uint8_t bytes[4] = {(uint8_t)v, (uint8_t)v, (uint8_t)v, (uint8_t)v};
      output_rgba8(rs, tg, color, bytes);
      for (uint32_t b = 0; b < 4u; b++) rs->span_lut[i][b][v] = bytes[b];
    }
  }
}

/* Pixels [x0, x1] of row y from the prepared fill. */
static void span_fill(Raster_State *rs, int32_t y, int32_t x0, int32_t x1) {
  const uint32_t n = (uint32_t)(x1 - x0 + 1);
  rs->stats.pixels += n;
  for (uint32_t i = 0; i < rs->target_count; i++) {
    Target *tg = &rs->targets[i];
    if (!tg->surface) continue;
    uint8_t *p = tg->surface->pixels + ((uint64_t)y * tg->surface->width + (uint64_t)x0) * 4u;
    if (!rs->span_blend[i]) {
      for (uint32_t k = 0; k < n; k++) memcpy(p + 4u * k, &rs->span_word[i], 4);
    } else {
      const uint8_t (*lut)[256] = rs->span_lut[i];
      for (uint32_t k = 0; k < n; k++, p += 4) {
        p[0] = lut[0][p[0]];
        p[1] = lut[1][p[1]];
        p[2] = lut[2][p[2]];
        p[3] = lut[3][p[3]];
      }
    }
    tg->surface->dirty = true;
  }
}

/* Depth, alpha test, blending and the colour write for one shaded pixel. */
static void output_pixel(Raster_State *rs, int32_t px, int32_t py, float depth, const uint32_t *out_regs) {
  uint8_t *zp = rs->depth ? rs->depth->pixels + ((uint64_t)py * rs->depth->width + (uint64_t)px) * rs->depth->bytes_per_pixel
                          : NULL;
  if (rs->alpha_test) {
    const uint8_t reg = rs->color_reg[0][3];
    const float alpha = reg == 0xffu ? 1.0f : f32(out_regs[reg]);
    if (!depth_compare(rs->alpha_func, alpha, rs->alpha_ref)) return;
  }
  if (rs->depth_reg != 0xffu) depth = f32(out_regs[rs->depth_reg]);
  if (rs->stencil) {
    /* Stencil test, then depth; each outcome applies its op through the
     * write mask, and a failure discards the fragment. */
    const uint32_t f = rs->front ? 0u : 1u;
    uint8_t *sp = zp + rs->stencil_byte;
    const uint8_t stored = *sp, mask = rs->stencil_func_mask[f], ref = rs->stencil_ref[f];
    uint32_t op;
    bool pass = depth_compare(rs->stencil_func[f], (float)(ref & mask), (float)(stored & mask));
    if (!pass) {
      op = rs->stencil_op_fail[f];
    } else if (rs->depth_test && !depth_compare(rs->depth_func, depth, read_depth(rs->depth->format, zp))) {
      op = rs->stencil_op_zfail[f];
      pass = false;
    } else {
      op = rs->stencil_op_zpass[f];
    }
    const uint8_t wmask = rs->stencil_write_mask[f];
    const uint8_t next = (uint8_t)((stored & ~wmask) | (stencil_apply(op, stored, ref) & wmask));
    if (next != stored) {
      *sp = next;
      rs->depth->dirty = true;
    }
    if (!pass) return;
    if (rs->depth_write && zeta_bytes(rs->depth->format) > 1u) {
      write_depth(rs->depth->format, zp, depth);
      rs->depth->dirty = true;
    }
  } else if (rs->depth) {
    if (rs->depth_test && rs->depth_reg != 0xffu &&
        !depth_compare(rs->depth_func, depth, read_depth(rs->depth->format, zp)))
      return;
    if (rs->depth_write) {
      write_depth(rs->depth->format, zp, depth);
      rs->depth->dirty = true;
    }
  }
  rs->stats.pixels++;
  for (uint32_t i = 0; i < rs->target_count; i++) {
    Target *tg = &rs->targets[i];
    if (!tg->surface) continue;
    uint32_t color[4];
    target_color(rs, i, out_regs, color);
    uint8_t *p = tg->surface->pixels + ((uint64_t)py * tg->surface->width + (uint64_t)px) * tg->surface->bytes_per_pixel;
    if (target_is_plain_rgba8(tg)) {
      output_rgba8(rs, tg, color, p);
      tg->surface->dirty = true;
      continue;
    }
    if (tg->rgba8_blend != RGBA8_BLEND_GENERIC) {
      output_rgba8_blend(tg, color, p);
      tg->surface->dirty = true;
      continue;
    }
    if (tg->blend || tg->write_mask != 0xfu) {
      float dst[4];
      decode_color(tg->format, p, dst);
      float src[4];
      for (uint32_t c = 0; c < 4; c++) src[c] = f32(color[c]);
      float out[4];
      for (uint32_t c = 0; c < 4; c++) {
        if (tg->blend) {
          const bool alpha = c == 3u;
          const float sf = factor_value((Blend_Factor)(alpha ? tg->alpha_src : tg->color_src), c, src, dst, rs->blend_const);
          const float df = factor_value((Blend_Factor)(alpha ? tg->alpha_dst : tg->color_dst), c, src, dst, rs->blend_const);
          const uint32_t op = alpha ? tg->alpha_op : tg->color_op;
          out[c] = (op == BOP_MIN || op == BOP_MAX) ? apply_op(op, src[c], dst[c]) : apply_op(op, src[c] * sf, dst[c] * df);
        } else {
          out[c] = src[c];
        }
        if (!((tg->write_mask >> c) & 1u)) out[c] = dst[c];
      }
      if (tg->format->kind == KIND_UINT || tg->format->kind == KIND_SINT) {
        for (uint32_t c = 0; c < 4; c++) color[c] = ((tg->write_mask >> c) & 1u) ? color[c] : u32f(dst[c]);
      } else {
        for (uint32_t c = 0; c < 4; c++) color[c] = u32f(out[c]);
      }
    }
    encode_color(tg->format, color, p);
    tg->surface->dirty = true;
  }
}

static void to_screen(const Raster_State *rs, const Vertex *v, Screen_Vertex *out) {
  const float w = v->pos[3];
  const float iw = 1.0f / w;
  float x = v->pos[0] * iw, y = v->pos[1] * iw, z = v->pos[2] * iw;
  if (rs->viewport_transform) {
    x = x * rs->vp_scale[0] + rs->vp_offset[0];
    y = y * rs->vp_scale[1] + rs->vp_offset[1];
    z = z * rs->vp_scale[2] + rs->vp_offset[2];
  }
  if (rs->lower_left) y = (float)rs->surface_height - y;
  out->x = x;
  out->y = y;
  out->z = z;
  out->inv_w = iw;
  out->fx = (int64_t)llrintf(x * (float)SUBPIXEL_ONE);
  out->fy = (int64_t)llrintf(y * (float)SUBPIXEL_ONE);
  out->src = v;
  for (uint32_t i = 0; i < rs->varyings.count; i++) {
    const float a = f32(v->varying[rs->varyings.word[i]]);
    out->varying[i] = rs->varyings.interp[i] == SM_INTERP_PERSPECTIVE ? a * iw : a;
  }
}

/* Pixel work is split into bands of BAND_ROWS rows that workers take in
 * turn (dynamically: host cores differ in speed). Even-sized and
 * even-aligned, so a 2x2 quad never straddles two bands; a band runs
 * every queued triangle in order, so each pixel still sees the draw's
 * triangles in order - the result does not depend on the worker count. */
#define BAND_ROWS 8

static bool row_owned(const Raster_State *rs, int64_t y) { return y >= rs->row_begin && y < rs->row_end; }

/* Whether any row in [y0, y1] is in this state's band. */
static bool rows_owned(const Raster_State *rs, int64_t y0, int64_t y1) { return y1 >= rs->row_begin && y0 < rs->row_end; }

static void raster_triangle_now(Raster_State *rs, const Vertex *a, const Vertex *b, const Vertex *c,
                                const Vertex *provoking) {
  Screen_Vertex sv[3];
  to_screen(rs, a, &sv[0]);
  to_screen(rs, b, &sv[1]);
  to_screen(rs, c, &sv[2]);
  int64_t area = (sv[1].fx - sv[0].fx) * (sv[2].fy - sv[0].fy) - (sv[1].fy - sv[0].fy) * (sv[2].fx - sv[0].fx);
  if (area == 0) return;
  /* Facing: positive area is clockwise on screen (y down). */
  const bool clockwise = area > 0;
  const bool front = rs->front_ccw ? !clockwise : clockwise;
  rs->front = front;
  if (rs->cull) {
    if (rs->cull_face == 0x408u) return;
    if (rs->cull_face == 0x404u && front) return;
    if (rs->cull_face == 0x405u && !front) return;
  }
  if (area < 0) {
    const Screen_Vertex t = sv[1];
    sv[1] = sv[2];
    sv[2] = t;
    area = -area;
  }
  /* Bounding box in pixels whose centres may be covered. */
  int64_t minx = sv[0].fx, maxx = sv[0].fx, miny = sv[0].fy, maxy = sv[0].fy;
  for (uint32_t i = 1; i < 3; i++) {
    if (sv[i].fx < minx) minx = sv[i].fx;
    if (sv[i].fx > maxx) maxx = sv[i].fx;
    if (sv[i].fy < miny) miny = sv[i].fy;
    if (sv[i].fy > maxy) maxy = sv[i].fy;
  }
  const int64_t half = SUBPIXEL_ONE / 2;
  int64_t x0 = (minx - half + SUBPIXEL_ONE - 1) >> SUBPIXEL_BITS, x1 = (maxx - half) >> SUBPIXEL_BITS;
  int64_t y0 = (miny - half + SUBPIXEL_ONE - 1) >> SUBPIXEL_BITS, y1 = (maxy - half) >> SUBPIXEL_BITS;
  if (x0 < rs->clip.x0) x0 = rs->clip.x0;
  if (y0 < rs->clip.y0) y0 = rs->clip.y0;
  if (x1 > rs->clip.x1 - 1) x1 = rs->clip.x1 - 1;
  if (y1 > rs->clip.y1 - 1) y1 = rs->clip.y1 - 1;
  if (x0 > x1 || y0 > y1 || !rows_owned(rs, y0, y1)) return;
  if (row_owned(rs, y0)) rs->stats.triangles++; /* once: in the band of its first row */
  /* Edge functions E_ij(p) = (xj-xi)(py-yi) - (yj-yi)(px-xi), all >= 0
   * inside; a pixel exactly on an edge belongs to it when the edge is a
   * "top-left" one (here: dy < 0, or dy == 0 and dx > 0). */
  int64_t ex[3], ey[3], bias[3], e_row[3];
  const int64_t pcx = x0 * SUBPIXEL_ONE + half, pcy = y0 * SUBPIXEL_ONE + half;
  for (uint32_t i = 0; i < 3; i++) {
    const Screen_Vertex *p = &sv[i], *q = &sv[(i + 1u) % 3u];
    const int64_t dx = q->fx - p->fx, dy = q->fy - p->fy;
    ex[i] = -dy * SUBPIXEL_ONE; /* step +1 pixel in x */
    ey[i] = dx * SUBPIXEL_ONE;  /* step +1 pixel in y */
    bias[i] = (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1;
    e_row[i] = dx * (pcy - p->fy) - dy * (pcx - p->fx) + bias[i];
  }
  /* Interpolation planes (relative to vertex 0). */
  const float area2 = (sv[1].x - sv[0].x) * (sv[2].y - sv[0].y) - (sv[1].y - sv[0].y) * (sv[2].x - sv[0].x);
  if (area2 == 0.0f) return;
  const float inv_area2 = 1.0f / area2;
  Plane planes[MAX_VARYINGS];
  for (uint32_t i = 0; i < rs->varyings.count; i++)
    plane_setup(&planes[i], &sv[0], &sv[1], &sv[2], sv[0].varying[i], sv[1].varying[i], sv[2].varying[i], inv_area2);
  Plane zp, wp;
  plane_setup(&zp, &sv[0], &sv[1], &sv[2], sv[0].z, sv[1].z, sv[2].z, inv_area2);
  plane_setup(&wp, &sv[0], &sv[1], &sv[2], sv[0].inv_w, sv[1].inv_w, sv[2].inv_w, inv_area2);
  Screen_Vertex prov;
  prov.src = provoking;
  /* Inputs constant over the triangle: shade once. */
  bool uniform = !rs->ctx->ps->reads_fragcoord_xy && wp.a == 0.0f && wp.b == 0.0f &&
                 (!rs->ctx->ps->reads_fragcoord_z || (zp.a == 0.0f && zp.b == 0.0f));
  for (uint32_t i = 0; uniform && i < rs->varyings.count; i++)
    if (planes[i].a != 0.0f || planes[i].b != 0.0f) uniform = false;
  Frag_Result shared;
  shared.valid = false;
  Pixel_Batch batch;
  batch.count = 0;
  /* Flat and large enough to amortise a blend table: fill spans. */
  const int64_t box = (x1 - x0 + 1) * (y1 - y0 + 1);
  if (uniform && rs->span_ok && box >= SPAN_MIN_PIXELS) {
    bool prepared = false;
    for (int64_t y = y0; y <= y1; y++) {
      int64_t e0 = e_row[0], e1 = e_row[1], e2 = e_row[2];
      int64_t first = -1, last = -1;
      e_row[0] += ey[0];
      e_row[1] += ey[1];
      e_row[2] += ey[2];
      if (!row_owned(rs, y)) continue;
      for (int64_t x = x0; x <= x1; x++) {
        if ((e0 | e1 | e2) >= 0) {
          if (first < 0) first = x;
          last = x;
        } else if (first >= 0) {
          break; /* convex: the run has ended */
        }
        e0 += ex[0];
        e1 += ex[1];
        e2 += ex[2];
      }
      if (first < 0) continue;
      if (!shared.valid) {
        const float dx = (float)first + 0.5f - sv[0].x, dy = (float)y + 0.5f - sv[0].y;
        shade_shared(rs, (int32_t)first, (int32_t)y, plane_at(&zp, dx, dy), planes, &wp, sv[0].x, sv[0].y, front, &prov,
                     &shared);
      }
      if (shared.killed) return;
      if (!prepared) {
        span_prepare(rs, shared.regs);
        prepared = true;
      }
      span_fill(rs, (int32_t)y, (int32_t)first, (int32_t)last);
    }
    return;
  }
  batch.live = 0;
  if (!uniform && rs->ctx->ps->uses_quads) {
    /* 2x2 quads: every quad with a live pixel shades all four lanes. */
    const int64_t e_base[3] = {e_row[0], e_row[1], e_row[2]};
    for (int64_t by = y0 & ~(int64_t)1; by <= y1; by += 2) {
      if (!row_owned(rs, by)) continue;
      for (int64_t bx = x0 & ~(int64_t)1; bx <= x1; bx += 2) {
        uint32_t live = 0;
        float zq[4];
        for (uint32_t q = 0; q < 4u; q++) {
          const int64_t px = bx + (int64_t)(q & 1u), py = by + (int64_t)(q >> 1);
          const float dx = (float)px + 0.5f - sv[0].x, dy = (float)py + 0.5f - sv[0].y;
          float z = plane_at(&zp, dx, dy);
          zq[q] = z < 0.0f ? 0.0f : (z > 1.0f ? 1.0f : z);
          if (px < x0 || px > x1 || py < y0 || py > y1) continue;
          const int64_t e0 = e_base[0] + (px - x0) * ex[0] + (py - y0) * ey[0];
          const int64_t e1 = e_base[1] + (px - x0) * ex[1] + (py - y0) * ey[1];
          const int64_t e2 = e_base[2] + (px - x0) * ex[2] + (py - y0) * ey[2];
          if ((e0 | e1 | e2) >= 0 && !early_depth_reject(rs, (int32_t)px, (int32_t)py, zq[q])) live |= 1u << q;
        }
        if (!live) continue;
        for (uint32_t q = 0; q < 4u; q++) {
          batch.x[batch.count] = (int32_t)(bx + (int64_t)(q & 1u));
          batch.y[batch.count] = (int32_t)(by + (int64_t)(q >> 1));
          batch.z[batch.count] = zq[q];
          if ((live >> q) & 1u) batch.live |= 1u << batch.count;
          batch.count++;
        }
        if (batch.count == SM_LANES) shade_batch(rs, &batch, planes, &wp, sv[0].x, sv[0].y, front, &prov);
      }
    }
    shade_batch(rs, &batch, planes, &wp, sv[0].x, sv[0].y, front, &prov);
    return;
  }
  for (int64_t y = y0; y <= y1; y++) {
    int64_t e0 = e_row[0], e1 = e_row[1], e2 = e_row[2];
    e_row[0] += ey[0];
    e_row[1] += ey[1];
    e_row[2] += ey[2];
    if (!row_owned(rs, y)) continue;
    for (int64_t x = x0; x <= x1; x++) {
      if ((e0 | e1 | e2) >= 0) {
        const float dx = (float)x + 0.5f - sv[0].x, dy = (float)y + 0.5f - sv[0].y;
        float z = plane_at(&zp, dx, dy);
        z = z < 0.0f ? 0.0f : (z > 1.0f ? 1.0f : z);
        if (uniform) {
          shade_pixel(rs, (int32_t)x, (int32_t)y, z, planes, &wp, sv[0].x, sv[0].y, front, &prov, &shared);
        } else if (!early_depth_reject(rs, (int32_t)x, (int32_t)y, z)) {
          batch.x[batch.count] = (int32_t)x;
          batch.y[batch.count] = (int32_t)y;
          batch.z[batch.count] = z;
          batch.live |= 1u << batch.count;
          if (++batch.count == SM_LANES) shade_batch(rs, &batch, planes, &wp, sv[0].x, sv[0].y, front, &prov);
        }
      }
      e0 += ex[0];
      e1 += ex[1];
      e2 += ex[2];
    }
  }
  shade_batch(rs, &batch, planes, &wp, sv[0].x, sv[0].y, front, &prov);
}

/* ---- parallel pixel work ------------------------------------------ */

/* Clipped triangles wait here until the draw ends (or the queue fills);
 * then every worker rasterises all of them over its own bands. Small
 * batches stay on the caller - a fork-join costs more than they do. */
#define TRIANGLE_QUEUE 256u
#define PARALLEL_MIN_PIXELS 4096u

typedef struct Queued_Triangle {
  Vertex v[3];
  Vertex provoking;
  int64_t y_min, y_max; /* conservative screen rows it can touch */
} Queued_Triangle;

static Queued_Triangle g_queue[TRIANGLE_QUEUE];
static uint32_t g_queue_count;
static uint64_t g_queue_pixels; /* bounding-box estimate of the queued work */
static Raster_State g_band_state[WORKERS_MAX];

typedef struct Band_Job {
  const Raster_State *master;
  int64_t first_row;  /* band 0's first row (BAND_ROWS-aligned) */
  uint32_t bands;
  uint32_t next;      /* the next band to take (workers_take) */
} Band_Job;

static void band_task(void *user, uint32_t index, uint32_t count) {
  Band_Job *job = (Band_Job *)user;
  const Raster_State *master = job->master;
  Raster3d *r = master->ctx->r;
  Raster_State *w = &g_band_state[index];
  memcpy(w, master, sizeof(*w));
  w->thread = r->band_threads[index];
  w->ps_env.user = &w->resolver;
  memset(&w->stats, 0, sizeof(w->stats));
  for (;;) {
    const uint32_t band = count > 1u ? workers_take(&r->workers, &job->next) : job->next++;
    if (band >= job->bands) break;
    w->row_begin = job->first_row + (int64_t)band * BAND_ROWS;
    w->row_end = job->bands == 1u ? INT64_MAX : w->row_begin + BAND_ROWS;
    for (uint32_t i = 0; i < g_queue_count; i++) {
      const Queued_Triangle *q = &g_queue[i];
      if (q->y_max < w->row_begin || q->y_min >= w->row_end) continue;
      raster_triangle_now(w, &q->v[0], &q->v[1], &q->v[2], &q->provoking);
    }
  }
}

static void flush_triangles(Raster_State *rs) {
  if (!g_queue_count) return;
  Raster3d *r = rs->ctx->r;
  const uint32_t n = g_queue_pixels >= PARALLEL_MIN_PIXELS ? r->workers.count : 1u;
  Band_Job job;
  job.master = rs;
  job.next = 0;
  if (n == 1u) {
    job.first_row = INT64_MIN / 2;
    job.bands = 1u; /* every row in one pass */
  } else {
    job.first_row = ((int64_t)rs->clip.y0 / BAND_ROWS) * BAND_ROWS;
    job.bands = (uint32_t)(((int64_t)rs->clip.y1 - job.first_row + BAND_ROWS - 1) / BAND_ROWS);
  }
  workers_run(&r->workers, n, band_task, &job);
  for (uint32_t i = 0; i < n && i < WORKERS_MAX; i++) {
    r->stats.triangles += g_band_state[i].stats.triangles;
    r->stats.pixels += g_band_state[i].stats.pixels;
    r->stats.shader_faults += g_band_state[i].stats.shader_faults;
  }
  g_queue_count = 0;
  g_queue_pixels = 0;
}

/* Screen-space bounding box of a clipped triangle, clamped to the clip
 * rectangle: its area in pixels (the parallel-or-not estimate) and a
 * conservative row range (a row of margin each side). */
static uint64_t triangle_extent(const Raster_State *rs, const Vertex *const v[3], int64_t *y_min, int64_t *y_max) {
  float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
  for (uint32_t i = 0; i < 3u; i++) {
    const float iw = 1.0f / v[i]->pos[3];
    float x = v[i]->pos[0] * iw, y = v[i]->pos[1] * iw;
    if (rs->viewport_transform) {
      x = x * rs->vp_scale[0] + rs->vp_offset[0];
      y = y * rs->vp_scale[1] + rs->vp_offset[1];
    }
    if (rs->lower_left) y = (float)rs->surface_height - y;
    if (i == 0 || x < x0) x0 = x;
    if (i == 0 || x > x1) x1 = x;
    if (i == 0 || y < y0) y0 = y;
    if (i == 0 || y > y1) y1 = y;
  }
  /* NaN or huge coordinates: claim every row. */
  *y_min = (y0 > (float)rs->clip.y0) ? (int64_t)y0 - 1 : rs->clip.y0;
  *y_max = (y1 < (float)rs->clip.y1) ? (int64_t)y1 + 1 : rs->clip.y1;
  if (!(y0 > -1e9f && y1 < 1e9f)) {
    *y_min = rs->clip.y0;
    *y_max = rs->clip.y1;
  }
  const float w = fminf(x1, (float)rs->clip.x1) - fmaxf(x0, (float)rs->clip.x0);
  const float h = fminf(y1, (float)rs->clip.y1) - fmaxf(y0, (float)rs->clip.y0);
  if (!(w > 0.0f) || !(h > 0.0f)) return 0;
  return (uint64_t)(w + 1.0f) * (uint64_t)(h + 1.0f);
}

static void gpu_triangle(Raster_State *rs, const Vertex *a, const Vertex *b, const Vertex *c, const Vertex *provoking);

static void raster_triangle(Raster_State *rs, const Vertex *a, const Vertex *b, const Vertex *c, const Vertex *provoking) {
  if (rs->ctx->r->gpu) {
    gpu_triangle(rs, a, b, c, provoking);
    return;
  }
  if (g_queue_count == TRIANGLE_QUEUE) flush_triangles(rs);
  Queued_Triangle *q = &g_queue[g_queue_count++];
  q->v[0] = *a;
  q->v[1] = *b;
  q->v[2] = *c;
  q->provoking = *provoking;
  const Vertex *const v[3] = {a, b, c};
  g_queue_pixels += triangle_extent(rs, v, &q->y_min, &q->y_max);
}

/* ---- clipping ----------------------------------------------------- */

static float plane_distance(const Raster_State *rs, const Vertex *v, uint32_t plane) {
  const float x = v->pos[0], y = v->pos[1], z = v->pos[2], w = v->pos[3];
  switch (plane) {
  case 0: return rs->z_zero_to_one ? z : z + w;   /* near */
  case 1: return w - z;                          /* far */
  case 2: return GUARD_BAND * w - x;
  case 3: return GUARD_BAND * w + x;
  case 4: return GUARD_BAND * w - y;
  case 5: return GUARD_BAND * w + y;
  default: return w - 1e-6f;                     /* w > 0 */
  }
}

static void lerp_vertex(const Vertex *a, const Vertex *b, float t, Vertex *out) {
  for (uint32_t c = 0; c < 4; c++) out->pos[c] = a->pos[c] + (b->pos[c] - a->pos[c]) * t;
  for (uint32_t i = 0; i < MAX_VARYINGS; i++) {
    const float va = f32(a->varying[i]), vb = f32(b->varying[i]);
    out->varying[i] = u32f(va + (vb - va) * t);
  }
}

static bool needs_clip(const Raster_State *rs, const Vertex *v) {
  for (uint32_t p = 0; p < 7u; p++)
    if (plane_distance(rs, v, p) < 0.0f) return true;
  return false;
}

static void draw_triangle(Raster_State *rs, const Vertex *a, const Vertex *b, const Vertex *c, const Vertex *provoking) {
  if (!needs_clip(rs, a) && !needs_clip(rs, b) && !needs_clip(rs, c)) {
    raster_triangle(rs, a, b, c, provoking);
    return;
  }
  static Vertex extra[MAX_POLY * 7u];
  uint32_t extra_used = 0;
  const Vertex *in[MAX_POLY];
  const Vertex *out[MAX_POLY];
  uint32_t n = 3;
  in[0] = a;
  in[1] = b;
  in[2] = c;
  for (uint32_t p = 0; p < 7u && n >= 3u; p++) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
      const Vertex *cur = in[i], *next = in[(i + 1u) % n];
      const float dc = plane_distance(rs, cur, p), dn = plane_distance(rs, next, p);
      if (dc >= 0.0f && m < MAX_POLY) out[m++] = cur;
      if ((dc >= 0.0f) != (dn >= 0.0f) && m < MAX_POLY && extra_used < MAX_POLY * 7u) {
        Vertex *v = &extra[extra_used++];
        lerp_vertex(cur, next, dc / (dc - dn), v);
        out[m++] = v;
      }
    }
    n = m;
    for (uint32_t i = 0; i < n; i++) in[i] = out[i];
  }
  for (uint32_t i = 1; i + 1u < n; i++) raster_triangle(rs, in[0], in[i], in[i + 1u], provoking);
}

/* Points and lines become small screen-aligned quads in clip space. */
static void draw_point(Raster_State *rs, const Vertex *v) {
  if (v->pos[3] <= 0.0f || !rs->viewport_transform) return;
  const float size = f32(rs->ctx->regs[REG_POINT_SIZE]);
  const float half = (size > 0.0f ? size : 1.0f) * 0.5f;
  const float hx = half / fabsf(rs->vp_scale[0] != 0.0f ? rs->vp_scale[0] : 1.0f) * v->pos[3];
  const float hy = half / fabsf(rs->vp_scale[1] != 0.0f ? rs->vp_scale[1] : 1.0f) * v->pos[3];
  Vertex q[4];
  for (uint32_t i = 0; i < 4; i++) {
    q[i] = *v;
    q[i].pos[0] += (i == 1u || i == 2u) ? hx : -hx;
    q[i].pos[1] += (i >= 2u) ? hy : -hy;
  }
  draw_triangle(rs, &q[0], &q[1], &q[2], v);
  draw_triangle(rs, &q[0], &q[2], &q[3], v);
}

static void draw_line(Raster_State *rs, const Vertex *a, const Vertex *b) {
  if (a->pos[3] <= 0.0f || b->pos[3] <= 0.0f || !rs->viewport_transform) return;
  const float sx = rs->vp_scale[0] != 0.0f ? fabsf(rs->vp_scale[0]) : 1.0f;
  const float sy = rs->vp_scale[1] != 0.0f ? fabsf(rs->vp_scale[1]) : 1.0f;
  float dx = (b->pos[0] / b->pos[3] - a->pos[0] / a->pos[3]) * sx;
  float dy = (b->pos[1] / b->pos[3] - a->pos[1] / a->pos[3]) * sy;
  const float len = sqrtf(dx * dx + dy * dy);
  if (len == 0.0f) return;
  /* Half-pixel offset along the normal, in NDC per vertex. */
  const float nx = -dy / len * 0.5f / sx, ny = dx / len * 0.5f / sy;
  Vertex q[4];
  q[0] = *a; q[1] = *b; q[2] = *b; q[3] = *a;
  q[0].pos[0] += nx * a->pos[3]; q[0].pos[1] += ny * a->pos[3];
  q[1].pos[0] += nx * b->pos[3]; q[1].pos[1] += ny * b->pos[3];
  q[2].pos[0] -= nx * b->pos[3]; q[2].pos[1] -= ny * b->pos[3];
  q[3].pos[0] -= nx * a->pos[3]; q[3].pos[1] -= ny * a->pos[3];
  draw_triangle(rs, &q[0], &q[1], &q[2], b);
  draw_triangle(rs, &q[0], &q[2], &q[3], b);
}

/* ---- primitive assembly ------------------------------------------- */

typedef struct Assembler {
  uint32_t topology;
  uint32_t n;            /* vertices seen since restart */
  uint32_t first;        /* first index (fans, loops, polygons) */
  uint32_t prev[3];
} Assembler;

static void emit_triangle(Raster_State *rs, Vertex_Cache *cache, uint32_t i0, uint32_t i1, uint32_t i2, uint32_t prov) {
  bool ok = true;
  const Vertex *a = vertex_get(rs->ctx, cache, i0, &ok);
  Vertex va = *a;
  const Vertex *b = vertex_get(rs->ctx, cache, i1, &ok);
  Vertex vb = *b;
  const Vertex *c = vertex_get(rs->ctx, cache, i2, &ok);
  Vertex vc = *c;
  if (!ok) return;
  const Vertex *p = prov == 0 ? &va : (prov == 1 ? &vb : &vc);
  draw_triangle(rs, &va, &vb, &vc, p);
}

static void emit_line(Raster_State *rs, Vertex_Cache *cache, uint32_t i0, uint32_t i1) {
  bool ok = true;
  Vertex a = *vertex_get(rs->ctx, cache, i0, &ok);
  Vertex b = *vertex_get(rs->ctx, cache, i1, &ok);
  if (ok) draw_line(rs, &a, &b);
}

static void assemble(Raster_State *rs, Vertex_Cache *cache, Assembler *as, uint32_t index) {
  const uint32_t last = rs->provoking_last ? 2u : 0u;
  const uint32_t n = as->n++;
  if (n == 0) as->first = index;
  switch (as->topology) {
  case TOPOLOGY_POINTS: {
    bool ok = true;
    Vertex v = *vertex_get(rs->ctx, cache, index, &ok);
    if (ok) draw_point(rs, &v);
    break;
  }
  case TOPOLOGY_LINES:
    if (n % 2u == 1u) emit_line(rs, cache, as->prev[0], index);
    break;
  case TOPOLOGY_LINE_STRIP:
  case TOPOLOGY_LINE_LOOP:
    if (n >= 1u) emit_line(rs, cache, as->prev[0], index);
    break;
  case TOPOLOGY_TRIANGLES:
    if (n % 3u == 2u) emit_triangle(rs, cache, as->prev[1], as->prev[0], index, last);
    break;
  case TOPOLOGY_TRIANGLE_STRIP:
    if (n >= 2u) {
      if (n % 2u == 0) emit_triangle(rs, cache, as->prev[1], as->prev[0], index, last);
      else emit_triangle(rs, cache, as->prev[0], as->prev[1], index, last);
    }
    break;
  case TOPOLOGY_TRIANGLE_FAN:
  case TOPOLOGY_POLYGON:
    if (n >= 2u) emit_triangle(rs, cache, as->first, as->prev[0], index, rs->provoking_last ? 2u : 1u);
    break;
  case TOPOLOGY_QUADS:
    if (n % 4u == 3u) {
      emit_triangle(rs, cache, as->prev[2], as->prev[1], as->prev[0], 2u);
      emit_triangle(rs, cache, as->prev[2], as->prev[0], index, 2u);
    }
    break;
  case TOPOLOGY_QUAD_STRIP:
    if (n >= 3u && n % 2u == 1u) {
      emit_triangle(rs, cache, as->prev[2], as->prev[1], index, 2u);
      emit_triangle(rs, cache, as->prev[2], index, as->prev[0], 2u);
    }
    break;
  default:
    break;
  }
  as->prev[2] = as->prev[1];
  as->prev[1] = as->prev[0];
  as->prev[0] = index;
}

static void assemble_end(Raster_State *rs, Vertex_Cache *cache, Assembler *as) {
  if (as->topology == TOPOLOGY_LINE_LOOP && as->n >= 2u) emit_line(rs, cache, as->prev[0], as->first);
  as->n = 0;
}

/* ---- GPU mode (§13: the WebGPU renderer) -------------------------- */

/* Everything up to the rasteriser runs here as in software mode; each
 * triangle that survives clipping and culling goes into the draw's vertex
 * list in WebGPU NDC (gpu_records.h), and the draw becomes one DRAW record:
 * attachments, fixed-function state, the pixel program as WGSL, its
 * textures and its constant buffers. Render targets exist only as GPU
 * textures (Raster3d_Gpu_Surface); textures are decoded here, uploaded
 * once and re-uploaded when their guest bytes change. */

#define GPU_SHADER_FAILED UINT32_MAX
#define GPU_WGSL_BYTES ((size_t)1 << 20)
#define GPU_VERTEX_BYTES ((size_t)768 * 1024)
#define GPU_DATA_WORDS (WGSL_DRAW_CONSTANT_WORDS + SM_CBUF_SLOTS * (CBUF_SLOT_BYTES / 4u))
#define GPU_VERTEX_HEADER_BYTES (GPU_VERTEX_HEADER_WORDS * 4u)

void raster3d_set_gpu(Raster3d *r, Gpu_Stream *stream) {
  if (!r->ready) return;
  r->gpu = stream;
  if (stream) log_info("[gpu] WebGPU renderer: draws stream to the GPU worker");
}

static uint32_t gpu_new_id(Raster3d *r) {
  if (++r->gpu_next_id == 0 || r->gpu_next_id == GPU_SHADER_FAILED) r->gpu_next_id = 1;
  return r->gpu_next_id;
}

/* The GPU format for a render-target format, and whether guest bytes
 * upload unchanged (same layout). */
static uint32_t gpu_color_format(uint32_t format, bool *direct) {
  *direct = true;
  switch (format) {
  case 0xC0: return GPU_FMT_RGBA32_FLOAT;
  case 0xC1: return GPU_FMT_RGBA32_SINT;
  case 0xC2: return GPU_FMT_RGBA32_UINT;
  case 0xC8: return GPU_FMT_RGBA16_SINT;
  case 0xC9: return GPU_FMT_RGBA16_UINT;
  case 0xCA: return GPU_FMT_RGBA16_FLOAT;
  case 0xCB: return GPU_FMT_RG32_FLOAT;
  case 0xCD: return GPU_FMT_RG32_UINT;
  case 0xCF: case 0xE6: return GPU_FMT_BGRA8_UNORM;
  case 0xD0: case 0xE7: return GPU_FMT_BGRA8_SRGB;
  case 0xD1: return GPU_FMT_RGB10A2_UNORM;
  case 0xD5: case 0xF9: return GPU_FMT_RGBA8_UNORM;
  case 0xD6: case 0xFA: return GPU_FMT_RGBA8_SRGB;
  case 0xD9: return GPU_FMT_RGBA8_UINT;
  case 0xDE: return GPU_FMT_RG16_FLOAT;
  case 0xE0: return GPU_FMT_RG11B10_UFLOAT;
  case 0xE3: return GPU_FMT_R32_SINT;
  case 0xE4: return GPU_FMT_R32_UINT;
  case 0xE5: case 0xFF: return GPU_FMT_R32_FLOAT;
  case 0xEA: return GPU_FMT_RG8_UNORM;
  case 0xF2: return GPU_FMT_R16_FLOAT;
  case 0xF3: return GPU_FMT_R8_UNORM;
  default: break;
  }
  *direct = false;
  const Color_Format *f = color_format(format);
  if (f && f->kind == KIND_UINT) return GPU_FMT_RGBA32_UINT;
  if (f && f->kind == KIND_SINT) return GPU_FMT_RGBA32_SINT;
  return GPU_FMT_RGBA16_FLOAT;
}

static uint32_t gpu_zeta_format(uint32_t format) {
  switch (format) {
  case ZT_ZF32: return GPU_FMT_DEPTH32F;
  case ZT_Z16: return GPU_FMT_DEPTH16;
  case ZT_ZF32_X24S8: return GPU_FMT_DEPTH32F_STENCIL8;
  case ZT_S8: return GPU_FMT_STENCIL8;
  default: return GPU_FMT_DEPTH24_STENCIL8;
  }
}

static bool gpu_format_is_int(uint32_t f, bool *is_signed) {
  *is_signed = f == GPU_FMT_RGBA16_SINT || f == GPU_FMT_RGBA32_SINT || f == GPU_FMT_R32_SINT;
  return *is_signed || f == GPU_FMT_RGBA32_UINT || f == GPU_FMT_RGBA16_UINT || f == GPU_FMT_RGBA8_UINT ||
         f == GPU_FMT_RG32_UINT || f == GPU_FMT_R32_UINT;
}

/* Formats a WebGPU filtering sampler accepts without optional features. */
static bool gpu_format_filterable(uint32_t f) {
  switch (f) {
  case GPU_FMT_RGBA8_UNORM: case GPU_FMT_RGBA8_SRGB: case GPU_FMT_BGRA8_UNORM: case GPU_FMT_BGRA8_SRGB:
  case GPU_FMT_RGBA16_FLOAT: case GPU_FMT_R8_UNORM: case GPU_FMT_RG8_UNORM: case GPU_FMT_R16_FLOAT:
  case GPU_FMT_RG16_FLOAT: case GPU_FMT_RG11B10_UFLOAT: case GPU_FMT_RGB10A2_UNORM:
    return true;
  default:
    return false;
  }
}

static void gpu_destroy(Raster3d *r, uint32_t id) {
  if (id) gpu_stream_write(r->gpu, GPU_REC_TEXTURE_DESTROY, &id, sizeof(id));
}

static void gpu_create(Raster3d *r, uint32_t id, uint32_t format, uint32_t width, uint32_t height, uint32_t layers,
                       uint32_t usage, uint32_t levels) {
  const Gpu_Rec_Texture_Create c = {id, format, width, height, layers, usage, levels, 0};
  gpu_stream_write(r->gpu, GPU_REC_TEXTURE_CREATE, &c, sizeof(c));
}

/* Uploads rows of tightly packed texels in records that fit the stream. */
static void gpu_write_rows(Raster3d *r, uint32_t id, uint32_t width, uint32_t height, uint32_t layer,
                           uint32_t bytes_per_row, const uint8_t *rows) {
  const uint64_t budget = gpu_stream_max_payload(r->gpu) - sizeof(Gpu_Rec_Texture_Write);
  uint32_t per_record = (uint32_t)(budget / bytes_per_row);
  if (per_record == 0) per_record = 1;
  for (uint32_t y = 0; y < height; y += per_record) {
    const uint32_t n = height - y < per_record ? height - y : per_record;
    const uint32_t data = n * bytes_per_row;
    const uint32_t padded = (data + 7u) & ~7u;
    uint8_t *p = gpu_stream_begin(r->gpu, GPU_REC_TEXTURE_WRITE, (uint32_t)sizeof(Gpu_Rec_Texture_Write) + padded);
    const Gpu_Rec_Texture_Write w = {id, 0, y, width, n, layer, bytes_per_row, data};
    memcpy(p, &w, sizeof(w));
    memcpy(p + sizeof(w), rows + (uint64_t)y * bytes_per_row, data);
    gpu_stream_end(r->gpu);
    r->gpu_stats.upload_bytes += data;
  }
}

/* Uploads a surface's guest contents (formats whose bytes WebGPU reads as
 * they are). */
static void gpu_surface_upload(Raster3d *r, Raster3d_Gpu_Surface *s, const Gpu_Memory *mem) {
  bool direct = false;
  if (s->depth) return;
  (void)gpu_color_format(s->format, &direct);
  if (!direct) return;
  const uint32_t row = s->width * s->bytes_per_pixel;
  uint8_t *pixels = r->surfaces[0].pixels; /* software surfaces are unused in GPU mode */
  if ((uint64_t)row * s->height > RASTER_MAX_SURFACE_BYTES) return;
  if (s->block_linear) {
    if (!mem->read(mem->user, s->address, r->staging, s->guest_bytes)) return;
    block_linear_to_pitch(r->staging, pixels, row, row, s->height, s->block_height_log2);
  } else {
    for (uint32_t y = 0; y < s->height; y++)
      if (!mem->read(mem->user, s->address + (uint64_t)y * s->pitch, pixels + (uint64_t)y * row, row))
        memset(pixels + (uint64_t)y * row, 0, row);
  }
  gpu_write_rows(r, s->id, s->width, s->height, 0, row, pixels);
}

/* The GPU surface for a render target, created on first use. `load`:
 * its current guest contents matter (not about to be fully overwritten). */
static Raster3d_Gpu_Surface *gpu_surface_get(Raster3d *r, const Surface_Desc *d, const Gpu_Memory *mem, bool load) {
  r->tick++;
  textures_invalidate(r, d->address, surface_guest_bytes(d));
  Raster3d_Gpu_Surface *victim = NULL;
  for (uint32_t i = 0; i < RASTER_GPU_SURFACES; i++) {
    Raster3d_Gpu_Surface *s = &r->gpu_surfaces[i];
    if (!s->in_use || s->address != d->address) continue;
    if (s->width == d->width && s->height == d->height && s->format == d->format && s->depth == d->depth) {
      s->last_used = r->tick;
      if (s->stale && load) gpu_surface_upload(r, s, mem);
      s->stale = false;
      return s;
    }
    gpu_destroy(r, s->id); /* re-described */
    s->in_use = false;
  }
  for (uint32_t i = 0; i < RASTER_GPU_SURFACES; i++) {
    Raster3d_Gpu_Surface *s = &r->gpu_surfaces[i];
    if (!s->in_use) {
      victim = s;
      break;
    }
    if (!victim || s->last_used < victim->last_used) victim = s;
  }
  if (victim->in_use) {
    log_warn("[gpu] GPU surface table full: dropping %ux%u @%llx", victim->width, victim->height,
             (unsigned long long)victim->address);
    gpu_destroy(r, victim->id);
  }
  memset(victim, 0, sizeof(*victim));
  victim->in_use = true;
  victim->id = gpu_new_id(r);
  victim->address = d->address;
  victim->cpu_address = d->address;
  if (mem->translate && !mem->translate(mem->user, d->address, &victim->cpu_address)) victim->cpu_address = 0;
  victim->width = d->width;
  victim->height = d->height;
  victim->format = d->format;
  victim->depth = d->depth;
  victim->bytes_per_pixel = d->bytes_per_pixel;
  victim->block_linear = d->block_linear;
  victim->block_height_log2 = d->block_height_log2;
  victim->pitch = d->pitch;
  victim->guest_bytes = surface_guest_bytes(d);
  victim->last_used = r->tick;
  bool direct = false;
  victim->gpu_format = d->depth ? gpu_zeta_format(d->format) : gpu_color_format(d->format, &direct);
  gpu_create(r, victim->id, victim->gpu_format, d->width, d->height, 1, GPU_USAGE_SAMPLED | GPU_USAGE_RENDER, 1);
  r->gpu_stats.surfaces++;
  log_debug("[gpu] GPU surface %u: %ux%u fmt 0x%02x%s @%llx", victim->id, d->width, d->height, d->format,
            d->depth ? " (zeta)" : "", (unsigned long long)d->address);
  if (load) gpu_surface_upload(r, victim, mem);
  return victim;
}

static Raster3d_Gpu_Surface *gpu_surface_at(Raster3d *r, uint64_t address, uint32_t width) {
  for (uint32_t i = 0; i < RASTER_GPU_SURFACES; i++) {
    Raster3d_Gpu_Surface *s = &r->gpu_surfaces[i];
    if (s->in_use && s->address == address && (!width || s->width == width)) return s;
  }
  return NULL;
}

/* Mip levels for a texture's GPU copy: the guest's count, within the
 * full chain, for float 2D (array) textures - the consumer builds them
 * from level 0 (gpu_records.h). Integer, cube and 3D textures: level 0. */
static uint32_t gpu_texture_levels(const Raster3d *r, const Tex_Image *img, bool is_int) {
  const Tex_Type type = img->header.type;
  if (!r->gpu_mipmaps || is_int || img->header.levels <= 1u || (type != TEX_TYPE_2D && type != TEX_TYPE_2D_ARRAY))
    return 1u;
  uint32_t full = 1;
  for (uint32_t size = img->width > img->height ? img->width : img->height; size > 1u; size >>= 1) full++;
  return img->header.levels < full ? img->header.levels : full;
}

/* A decoded texture's GPU copy: RGBA8 as it is, anything else as the
 * 32-bit RGBA texels sampling sees (tex_texel). */
static uint32_t gpu_texture(Raster3d *r, Raster3d_Texture *t) {
  const Tex_Image *img = &t->image;
  const uint32_t layers = img->layers ? img->layers : 1u;
  const bool is_int = img->header.data_type[0] == TEX_DATA_UINT || img->header.data_type[0] == TEX_DATA_SINT;
  const uint32_t format = img->rgba8 ? GPU_FMT_RGBA8_UNORM
                          : !is_int ? GPU_FMT_RGBA32_FLOAT
                          : img->header.data_type[0] == TEX_DATA_UINT ? GPU_FMT_RGBA32_UINT
                                                                     : GPU_FMT_RGBA32_SINT;
  const uint32_t levels = gpu_texture_levels(r, img, is_int);
  const bool same = t->gpu_id && t->gpu_width == img->width && t->gpu_height == img->height && t->gpu_layers == layers &&
                    t->gpu_format == format && t->gpu_levels == levels;
  if (same && t->gpu_hash == t->raw_hash) return t->gpu_id;
  if (!same) {
    gpu_destroy(r, t->gpu_id);
    t->gpu_id = gpu_new_id(r);
    gpu_create(r, t->gpu_id, format, img->width, img->height, layers, GPU_USAGE_SAMPLED, levels);
    t->gpu_width = img->width;
    t->gpu_height = img->height;
    t->gpu_layers = layers;
    t->gpu_format = format;
    t->gpu_levels = levels;
  }
  for (uint32_t l = 0; l < layers; l++) {
    if (img->rgba8) {
      gpu_write_rows(r, t->gpu_id, img->width, img->height, l, img->row_bytes,
                     img->texels + img->layer_bytes * l);
      continue;
    }
    /* Convert a band of rows at a time through the staging buffer. */
    const uint32_t row = img->width * 16u;
    const uint32_t band = (uint32_t)(RASTER_MAX_SURFACE_BYTES / row);
    for (uint32_t y0 = 0; y0 < img->height; y0 += band) {
      const uint32_t n = img->height - y0 < band ? img->height - y0 : band;
      uint32_t *out = (uint32_t *)(void *)r->staging;
      for (uint32_t y = 0; y < n; y++)
        for (uint32_t x = 0; x < img->width; x++) tex_texel(img, x, y0 + y, l, out + ((size_t)y * img->width + x) * 4u);
      /* gpu_write_rows writes from row 0 of what it is given: offset ids. */
      const uint64_t budget = gpu_stream_max_payload(r->gpu) - sizeof(Gpu_Rec_Texture_Write);
      uint32_t per_record = (uint32_t)(budget / row);
      if (per_record == 0) per_record = 1;
      for (uint32_t y = 0; y < n; y += per_record) {
        const uint32_t m = n - y < per_record ? n - y : per_record;
        uint8_t *p = gpu_stream_begin(r->gpu, GPU_REC_TEXTURE_WRITE, (uint32_t)sizeof(Gpu_Rec_Texture_Write) + m * row);
        const Gpu_Rec_Texture_Write w = {t->gpu_id, 0, y0 + y, img->width, m, l, row, m * row};
        memcpy(p, &w, sizeof(w));
        memcpy(p + sizeof(w), r->staging + (size_t)y * row, (size_t)m * row);
        gpu_stream_end(r->gpu);
        r->gpu_stats.upload_bytes += (uint64_t)m * row;
      }
    }
  }
  t->gpu_hash = t->raw_hash;
  r->gpu_stats.texture_uploads++;
  return t->gpu_id;
}

/* ---- the draw ---- */

#define GPU_MAX_PROBE_TEXTURES 64u

typedef struct Gpu_Probe {
  uint32_t count;
  uint32_t pc[GPU_MAX_PROBE_TEXTURES];
  uint32_t handle[GPU_MAX_PROBE_TEXTURES];
} Gpu_Probe;

static void gpu_probe_texture(void *user, const Sm_Tex_Request *requests, Sm_Mask lanes, uint32_t (*out)[4]) {
  Gpu_Probe *p = (Gpu_Probe *)user;
  for (uint32_t l = 0; l < SM_LANES; l++) {
    if (!((lanes >> l) & 1u)) continue;
    const Sm_Tex_Request *q = &requests[l];
    bool seen = false;
    for (uint32_t i = 0; i < p->count && !seen; i++) seen = p->pc[i] == q->pc;
    if (!seen && p->count < GPU_MAX_PROBE_TEXTURES) {
      p->pc[p->count] = q->pc;
      p->handle[p->count] = q->handle;
      p->count++;
    }
    (void)out;
  }
}

typedef struct Gpu_Draw {
  bool active;      /* raster3d_draw in GPU mode is assembling into this */
  bool prepared;    /* the shader and bindings are known (first triangle) */
  bool dead;        /* the draw cannot be expressed: its triangles are dropped */
  uint32_t width, height;
  uint32_t target_id[MAX_TARGETS];
  uint32_t target_format[MAX_TARGETS];
  uint32_t depth_id;
  uint32_t depth_format;
  /* varyings: word (vector * 4 + component) -> rs->varyings index; per location its vector */
  uint8_t varying_index[MAX_VARYINGS];
  uint8_t location_vector[WGSL_MAX_VARYINGS];
  uint32_t locations;
  uint32_t flat_mask;
  uint32_t stride;
  uint32_t vertex_bytes;
  uint32_t vertices;
  uint32_t shader_id;
  uint32_t texture_count;
  uint32_t texture_id[WGSL_MAX_TEXTURES];
  uint32_t sampler_state[WGSL_MAX_TEXTURES]; /* GPU_SAMPLER_*, for hardware-sampled bindings */
  uint32_t data_words;
  Wgsl_Program_Desc desc;
} Gpu_Draw;

static Gpu_Draw g_gpu_draw;

static uint32_t gpu_blend_factor(uint32_t f) {
  switch ((Blend_Factor)f) {
  case BF_ZERO: return GPU_BF_ZERO;
  case BF_ONE: return GPU_BF_ONE;
  case BF_SRC_COLOR: return GPU_BF_SRC;
  case BF_INV_SRC_COLOR: return GPU_BF_ONE_MINUS_SRC;
  case BF_SRC_ALPHA: return GPU_BF_SRC_ALPHA;
  case BF_INV_SRC_ALPHA: return GPU_BF_ONE_MINUS_SRC_ALPHA;
  case BF_DST_ALPHA: return GPU_BF_DST_ALPHA;
  case BF_INV_DST_ALPHA: return GPU_BF_ONE_MINUS_DST_ALPHA;
  case BF_DST_COLOR: return GPU_BF_DST;
  case BF_INV_DST_COLOR: return GPU_BF_ONE_MINUS_DST;
  case BF_SRC_ALPHA_SAT: return GPU_BF_SRC_ALPHA_SATURATED;
  case BF_CONST_COLOR: case BF_CONST_ALPHA: return GPU_BF_CONSTANT;
  default: return GPU_BF_ONE_MINUS_CONSTANT;
  }
}

/* OGL 0x200-0x207 or D3D 1-8 -> NEVER .. ALWAYS (depth_compare's rule). */
static uint32_t gpu_compare(uint32_t func) { return (func >= 0x200u ? func - 0x200u : (func >= 1u ? func - 1u : 7u)) & 7u; }

static uint32_t gpu_stencil_op(uint32_t op) {
  switch (op) {
  case 0x0000: case 2: return GPU_SOP_ZERO;
  case 0x1e01: case 3: return GPU_SOP_REPLACE;
  case 0x1e02: case 4: return GPU_SOP_INCREMENT_CLAMP;
  case 0x1e03: case 5: return GPU_SOP_DECREMENT_CLAMP;
  case 0x150a: case 6: return GPU_SOP_INVERT;
  case 0x8507: case 7: return GPU_SOP_INCREMENT_WRAP;
  case 0x8508: case 8: return GPU_SOP_DECREMENT_WRAP;
  default: return GPU_SOP_KEEP;
  }
}

/* Attachments for the draw; false when it cannot be expressed. */
static bool gpu_draw_begin(Draw_Context *ctx, Raster_State *rs) {
  Raster3d *r = ctx->r;
  Gpu_Draw *g = &g_gpu_draw;
  memset(g, 0, offsetof(Gpu_Draw, desc));
  const uint32_t *regs = ctx->regs;
  const uint32_t select = regs[REG_CT_SELECT];
  for (uint32_t i = 0; i < rs->target_count; i++) {
    if (!rs->targets[i].surface) continue;
    Surface_Desc d;
    if (!color_target_desc(regs, (select >> (4u + 3u * i)) & 7u, &d)) continue;
    if (g->width && (d.width != g->width || d.height != g->height)) {
      log_debug("[gpu] draw target %u is %ux%u, not %ux%u: left out", i, d.width, d.height, g->width, g->height);
      continue;
    }
    Raster3d_Gpu_Surface *s = gpu_surface_get(r, &d, ctx->mem, true);
    g->width = d.width;
    g->height = d.height;
    g->target_id[i] = s->id;
    g->target_format[i] = s->gpu_format;
  }
  if (rs->depth) {
    Surface_Desc zd;
    if (zeta_desc(regs, &zd) && (!g->width || (zd.width == g->width && zd.height == g->height))) {
      Raster3d_Gpu_Surface *s = gpu_surface_get(r, &zd, ctx->mem, true);
      g->width = zd.width;
      g->height = zd.height;
      g->depth_id = s->id;
      g->depth_format = s->gpu_format;
    }
  }
  if (!g->width) return false;
  /* Varying locations: each generic vector the pixel program reads. */
  memset(g->varying_index, 0xff, sizeof(g->varying_index));
  for (uint32_t i = 0; i < rs->varyings.count; i++) g->varying_index[rs->varyings.word[i]] = (uint8_t)i;
  wgsl_default_desc(ctx->ps, &g->desc);
  for (uint32_t v = 0; v < SM_ATTR_GENERIC_COUNT; v++) {
    const uint8_t loc = g->desc.varying_location[v];
    if (loc != 0xffu && loc < WGSL_MAX_VARYINGS) g->location_vector[loc] = (uint8_t)v;
  }
  g->locations = g->desc.varying_count;
  g->flat_mask = g->desc.flat_mask;
  g->stride = GPU_VERTEX_HEADER_BYTES + 16u * g->locations;
  g->active = true;
  return true;
}

/* Mip levels of the GPU texture `id` (a render target has one). */
static uint32_t gpu_texture_levels_of(const Raster3d *r, const Raster3d_Gpu_Surface *surface, uint32_t id) {
  if (surface || !id) return 1u;
  for (uint32_t i = 0; i < RASTER_TEXTURES; i++)
    if (r->textures[i].valid && r->textures[i].gpu_id == id) return r->textures[i].gpu_levels;
  return 1u;
}

/* Runs the pixel program once (one lane, at a vertex) to learn which
 * texture handle each texture instruction uses - bindless handles come
 * from registers - then builds the WGSL descriptor, the shader and the
 * draw's data. */
static bool gpu_prepare(Raster_State *rs, const Screen_Vertex *at, const Vertex *provoking, bool front) {
  Draw_Context *ctx = rs->ctx;
  Raster3d *r = ctx->r;
  Gpu_Draw *g = &g_gpu_draw;
  Sm_Thread *t = rs->thread;
  sm_thread_reset_light(t, 1u);
  t->front_facing = front ? SM_ALL_LANES : 0;
  t->attr_in[SM_ATTR_POSITION / 4u + 0u][0] = u32f(at->x);
  t->attr_in[SM_ATTR_POSITION / 4u + 1u][0] = u32f(rs->lower_left ? (float)rs->surface_height - at->y : at->y);
  t->attr_in[SM_ATTR_POSITION / 4u + 2u][0] = u32f(at->z);
  t->attr_in[SM_ATTR_POSITION / 4u + 3u][0] = u32f(at->inv_w);
  for (uint32_t i = 0; i < rs->varyings.count; i++) {
    const uint32_t word = SM_ATTR_GENERIC / 4u + rs->varyings.word[i];
    t->attr_in[word][0] = rs->varyings.interp[i] == SM_INTERP_CONSTANT ? provoking->varying[rs->varyings.word[i]]
                                                                       : u32f(at->varying[i]);
  }
  static Gpu_Probe probe;
  probe.count = 0;
  Sm_Env env = ctx->env[1];
  env.texture = NULL;
  env.texture_batch = gpu_probe_texture;
  env.user = &probe;
  (void)sm_run(ctx->ps, &env, t);
  /* One binding per distinct handle. */
  Wgsl_Program_Desc *desc = &g->desc;
  memset(desc->binding_of, WGSL_NO_BINDING, sizeof(desc->binding_of));
  desc->texture_count = 0;
  desc->hw_sample_mask = 0;
  uint32_t *data = r->gpu_data;
  memset(data, 0, WGSL_DRAW_CONSTANT_WORDS * 4u);
  uint32_t handles[WGSL_MAX_TEXTURES];
  const uint32_t *regs = ctx->regs;
  const uint64_t tic_pool = addr40(regs[REG_TEX_HEADER_POOL], regs[REG_TEX_HEADER_POOL + 1u]);
  const uint64_t tsc_pool = addr40(regs[REG_SAMPLER_POOL], regs[REG_SAMPLER_POOL + 1u]);
  for (uint32_t i = 0; i < probe.count; i++) {
    uint32_t b = 0;
    while (b < desc->texture_count && handles[b] != probe.handle[i]) b++;
    if (b == desc->texture_count) {
      if (b >= WGSL_MAX_TEXTURES) continue;
      const uint32_t handle = probe.handle[i];
      const uint32_t tic_index = handle & 0xfffffu;
      const uint32_t tsc_index = (regs[REG_SAMPLER_BINDING] & 1u) ? tic_index : (handle >> 20) & 0xfffu;
      uint32_t tic[8], tsc[8];
      if (!tic_pool || !ctx->mem->read(ctx->mem->user, tic_pool + (uint64_t)tic_index * TEX_HEADER_BYTES, tic, sizeof(tic)))
        continue;
      Tex_Header h;
      tex_header_parse(tic, &h);
      Tex_Sampler s;
      memset(&s, 0, sizeof(s));
      s.mag_filter = 1;
      if (tsc_pool && ctx->mem->read(ctx->mem->user, tsc_pool + (uint64_t)tsc_index * TEX_SAMPLER_BYTES, tsc, sizeof(tsc)))
        tex_sampler_parse(tsc, &s);
      uint32_t id = 0, sample_type = WGSL_SAMPLE_FLOAT, gpu_format = 0;
      Raster3d_Gpu_Surface *surf = gpu_surface_at(r, h.address, h.width);
      if (surf) {
        bool is_signed = false;
        id = surf->id;
        gpu_format = surf->gpu_format;
        if (gpu_format_is_int(surf->gpu_format, &is_signed)) sample_type = is_signed ? WGSL_SAMPLE_SINT : WGSL_SAMPLE_UINT;
      } else {
        Raster3d_Texture *tex = texture_load(r, tic, ctx->mem);
        if (!tex) continue;
        id = gpu_texture(r, tex);
        gpu_format = tex->gpu_format;
        if (h.data_type[0] == TEX_DATA_UINT) sample_type = WGSL_SAMPLE_UINT;
        if (h.data_type[0] == TEX_DATA_SINT) sample_type = WGSL_SAMPLE_SINT;
      }
      /* A hardware sampler where it gives the reference's result. */
      const bool cube_map = h.type == TEX_TYPE_CUBE || h.type == TEX_TYPE_CUBE_ARRAY;
      if (sample_type == WGSL_SAMPLE_FLOAT && !cube_map && gpu_format_filterable(gpu_format) && s.wrap[0] <= 2u &&
          s.wrap[1] <= 2u) {
        desc->hw_sample_mask |= 1u << b;
        /* Minification follows magnification on one level (the
         * reference's single filter); mip chains use the TSC's own. */
        const bool mips = gpu_texture_levels_of(r, surf, id) > 1u;
        const bool min_linear = mips ? s.min_filter == 2u : s.mag_filter == 2u;
        g->sampler_state[b] = (s.mag_filter == 2u ? GPU_SAMPLER_LINEAR : 0u) | (min_linear ? GPU_SAMPLER_MIN_LINEAR : 0u) |
                              (s.mip_filter == TEX_MIP_LINEAR ? GPU_SAMPLER_MIP_LINEAR : 0u) |
                              ((uint32_t)s.wrap[0] << GPU_SAMPLER_WRAP_SHIFT(0)) |
                              ((uint32_t)s.wrap[1] << GPU_SAMPLER_WRAP_SHIFT(1)) |
                              ((uint32_t)(s.wrap[2] <= 2u ? s.wrap[2] : 2u) << GPU_SAMPLER_WRAP_SHIFT(2));
      }
      handles[b] = handle;
      g->texture_id[b] = id;
      desc->sample_type[b] = (uint8_t)sample_type;
      uint32_t *p = data + WGSL_DRAW_TEXTURE_PARAMS + WGSL_TEX_PARAM_WORDS * b;
      const bool cube = h.type == TEX_TYPE_CUBE || h.type == TEX_TYPE_CUBE_ARRAY;
      p[WGSL_TEXP_FLAGS] = ((h.normalized || cube) ? WGSL_TEXP_SCALE : 0u) |
                           (s.mag_filter == 2u && sample_type == WGSL_SAMPLE_FLOAT ? WGSL_TEXP_LINEAR : 0u) |
                           (cube ? WGSL_TEXP_CUBE : 0u) | (s.depth_compare ? WGSL_TEXP_DEPTH_COMPARE : 0u);
      p[WGSL_TEXP_WRAP] = (uint32_t)s.wrap[0] | ((uint32_t)s.wrap[1] << 4) | ((uint32_t)s.wrap[2] << 8);
      p[WGSL_TEXP_SWIZZLE] = (uint32_t)h.swizzle[0] | ((uint32_t)h.swizzle[1] << 4) | ((uint32_t)h.swizzle[2] << 8) |
                             ((uint32_t)h.swizzle[3] << 12);
      p[WGSL_TEXP_LEVELS] = h.levels;
      p[WGSL_TEXP_COMPARE] = s.compare_func;
      /* Level selection (textures with a mip chain only; the shader
       * clamps to the levels the GPU texture has). */
      if (s.mip_filter > TEX_MIP_NONE && s.min_filter == 2u) p[WGSL_TEXP_FLAGS] |= WGSL_TEXP_MIN_LINEAR;
      p[WGSL_TEXP_LOD_BIAS] = u32f(s.lod_bias);
      p[WGSL_TEXP_MIN_LOD] = u32f(s.mip_filter > TEX_MIP_NONE ? s.min_lod : 0.0f);
      p[WGSL_TEXP_MAX_LOD] = u32f(s.mip_filter > TEX_MIP_NONE ? s.max_lod : 0.0f);
      for (uint32_t c = 0; c < 4; c++) p[WGSL_TEXP_BORDER + c] = u32f(s.border[c]);
      desc->texture_count++;
    }
    desc->binding_of[probe.pc[i]] = (uint8_t)b;
  }
  g->texture_count = desc->texture_count;
  /* Targets. */
  desc->target_count = rs->target_count;
  desc->target_int_mask = desc->target_sint_mask = 0;
  for (uint32_t i = 0; i < rs->target_count; i++) {
    bool is_signed = false;
    if (g->target_id[i] && gpu_format_is_int(g->target_format[i], &is_signed)) {
      desc->target_int_mask |= 1u << i;
      if (is_signed) desc->target_sint_mask |= 1u << i;
    }
  }
  desc->mrt = rs->mrt;
  /* The shader. */
  const uint64_t key = wgsl_desc_hash(desc, ctx->ps);
  uint32_t slot = (uint32_t)(key % RASTER_GPU_SHADERS);
  Raster3d_Gpu_Shader *entry = NULL;
  for (uint32_t probe_i = 0; probe_i < RASTER_GPU_SHADERS; probe_i++) {
    Raster3d_Gpu_Shader *e = &r->gpu_shaders[(slot + probe_i) % RASTER_GPU_SHADERS];
    if (e->id == 0 || e->key == key) {
      entry = e;
      break;
    }
  }
  if (!entry) { /* full: start over (the GPU side keeps its pipelines by id) */
    memset(r->gpu_shaders, 0, sizeof(Raster3d_Gpu_Shader) * RASTER_GPU_SHADERS);
    entry = &r->gpu_shaders[slot];
  }
  if (entry->id == 0) {
    entry->key = key;
    const Wgsl_Result res = wgsl_translate(ctx->ps, desc, r->gpu_wgsl, GPU_WGSL_BYTES);
    if (!res.ok) {
      log_warn("[gpu] pixel program %llx not translated: %s", (unsigned long long)ctx->ps->address, res.reason);
      entry->id = GPU_SHADER_FAILED;
    } else {
      entry->id = gpu_new_id(r);
      const uint32_t padded = ((uint32_t)res.length + 7u) & ~7u;
      uint8_t *p = gpu_stream_begin(r->gpu, GPU_REC_SHADER, 8u + padded);
      const uint32_t head[2] = {entry->id, (uint32_t)res.length};
      memcpy(p, head, sizeof(head));
      memcpy(p + 8u, res.text, res.length);
      memset(p + 8u + res.length, ' ', padded - res.length);
      gpu_stream_end(r->gpu);
      r->gpu_stats.shaders++;
    }
  }
  if (entry->id == GPU_SHADER_FAILED) return false;
  g->shader_id = entry->id;
  /* Draw constants and the constant buffers the program reads. */
  data[WGSL_DRAW_SURFACE_HEIGHT] = u32f((float)rs->surface_height);
  data[WGSL_DRAW_FLAGS] = rs->lower_left ? WGSL_DRAW_LOWER_LEFT : 0u;
  data[WGSL_DRAW_ALPHA_FUNC] = rs->alpha_test ? 1u + gpu_compare(rs->alpha_func) : 0u;
  data[WGSL_DRAW_ALPHA_REF] = u32f(rs->alpha_ref);
  uint32_t words = WGSL_DRAW_CONSTANT_WORDS;
  const Sm_Env *penv = &ctx->env[1];
  for (uint32_t s = 0; s < SM_CBUF_SLOTS; s++) {
    if (!penv->cbuf[s] || !penv->cbuf_size[s]) continue;
    const uint32_t n = penv->cbuf_size[s] / 4u;
    memcpy(data + words, penv->cbuf[s], (size_t)n * 4u);
    data[WGSL_DRAW_CBUF_TABLE + 2u * s] = words;
    data[WGSL_DRAW_CBUF_TABLE + 2u * s + 1u] = n;
    words += n;
  }
  if (words & 1u) data[words++] = 0; /* records stay 8-aligned */
  g->data_words = words;
  return true;
}

static void gpu_emit_draw(Raster_State *rs) {
  Raster3d *r = rs->ctx->r;
  Gpu_Draw *g = &g_gpu_draw;
  if (!g->vertices) return;
  uint32_t samplers = 0;
  for (uint32_t i = 0; i < g->texture_count; i++) samplers += (g->desc.hw_sample_mask >> i) & 1u;
  const uint32_t bindings = 1u + g->texture_count + samplers;
  const uint64_t bytes = sizeof(Gpu_Rec_Draw) + sizeof(Gpu_Rec_Binding) * bindings + (uint64_t)g->data_words * 4u +
                         g->vertex_bytes;
  if (bytes > gpu_stream_max_payload(r->gpu)) {
    log_warn("[gpu] draw of %u bytes exceeds the stream's record limit: dropped", (uint32_t)bytes);
    g->vertices = g->vertex_bytes = 0;
    return;
  }
  uint8_t *p = gpu_stream_begin(r->gpu, GPU_REC_DRAW, (uint32_t)bytes);
  Gpu_Rec_Draw d;
  memset(&d, 0, sizeof(d));
  d.shader_id = g->shader_id;
  d.target_count = rs->target_count;
  for (uint32_t i = 0; i < rs->target_count && i < GPU_MAX_TARGETS; i++) {
    const Target *t = &rs->targets[i];
    Gpu_Rec_Target *o = &d.targets[i];
    o->id = g->target_id[i];
    o->write_mask = t->write_mask;
    o->blend = t->blend ? 1u : 0u;
    o->color_op = t->color_op;
    o->alpha_op = t->alpha_op;
    o->color_src = gpu_blend_factor(t->color_src);
    o->color_dst = gpu_blend_factor(t->color_dst);
    o->alpha_src = gpu_blend_factor(t->alpha_src);
    o->alpha_dst = gpu_blend_factor(t->alpha_dst);
  }
  d.depth_id = g->depth_id;
  d.depth_test = rs->depth_test ? 1u : 0u;
  d.depth_write = rs->depth_write ? 1u : 0u;
  d.depth_compare = rs->depth_test ? gpu_compare(rs->depth_func) : GPU_CMP_ALWAYS;
  d.stencil = rs->stencil ? 1u : 0u;
  if (rs->stencil) {
    Gpu_Rec_Stencil_Face *faces[2] = {&d.stencil_front, &d.stencil_back};
    for (uint32_t f = 0; f < 2u; f++) {
      faces[f]->fail = gpu_stencil_op(rs->stencil_op_fail[f]);
      faces[f]->depth_fail = gpu_stencil_op(rs->stencil_op_zfail[f]);
      faces[f]->pass = gpu_stencil_op(rs->stencil_op_zpass[f]);
      faces[f]->compare = gpu_compare(rs->stencil_func[f]);
    }
    d.stencil_read_mask = rs->stencil_func_mask[0];
    d.stencil_write_mask = rs->stencil_write_mask[0];
    d.stencil_ref = rs->stencil_ref[0];
  }
  for (uint32_t c = 0; c < 4; c++) d.blend_constant[c] = u32f(rs->blend_const[c]);
  d.scissor[0] = rs->clip.x0;
  d.scissor[1] = rs->clip.y0;
  d.scissor[2] = rs->clip.x1 - rs->clip.x0;
  d.scissor[3] = rs->clip.y1 - rs->clip.y0;
  d.varying_count = g->locations;
  d.flat_mask = g->flat_mask;
  d.binding_count = bindings;
  d.vertex_count = g->vertices;
  memcpy(p, &d, sizeof(d));
  p += sizeof(d);
  Gpu_Rec_Binding b = {GPU_BIND_DATA, WGSL_DATA_BINDING, g->data_words * 4u, 0};
  memcpy(p, &b, sizeof(b));
  p += sizeof(b);
  memcpy(p, r->gpu_data, (size_t)g->data_words * 4u);
  p += (size_t)g->data_words * 4u;
  for (uint32_t i = 0; i < g->texture_count; i++) {
    const bool hw = (g->desc.hw_sample_mask >> i) & 1u;
    const Gpu_Rec_Binding tb = {GPU_BIND_TEXTURE, WGSL_TEXTURE_BINDING_BASE + i, hw ? GPU_BIND_FILTERED : 0u,
                                g->texture_id[i]};
    memcpy(p, &tb, sizeof(tb));
    p += sizeof(tb);
    if (!hw) continue;
    const Gpu_Rec_Binding sb = {GPU_BIND_SAMPLER, WGSL_SAMPLER_BINDING_BASE + i, 0, g->sampler_state[i]};
    memcpy(p, &sb, sizeof(sb));
    p += sizeof(sb);
  }
  memcpy(p, r->gpu_vertices, g->vertex_bytes);
  gpu_stream_end(r->gpu);
  r->gpu_stats.draws++;
  g->vertices = 0;
  g->vertex_bytes = 0;
}

static void gpu_put_vertex(Raster_State *rs, const Screen_Vertex *v, const Vertex *provoking) {
  Gpu_Draw *g = &g_gpu_draw;
  uint8_t *p = rs->ctx->r->gpu_vertices + g->vertex_bytes;
  const float head[4] = {v->x / (float)g->width * 2.0f - 1.0f, 1.0f - v->y / (float)g->height * 2.0f, v->z, v->inv_w};
  memcpy(p, head, sizeof(head));
  uint32_t *vary = (uint32_t *)(void *)(p + GPU_VERTEX_HEADER_BYTES);
  for (uint32_t l = 0; l < g->locations; l++) {
    const uint32_t vec = g->location_vector[l];
    for (uint32_t c = 0; c < 4u; c++) {
      const uint32_t word = vec * 4u + c;
      const uint8_t i = g->varying_index[word];
      uint32_t value = 0;
      if (i != 0xffu) value = rs->varyings.interp[i] == SM_INTERP_CONSTANT ? provoking->varying[word] : u32f(v->varying[i]);
      vary[l * 4u + c] = value;
    }
  }
  g->vertex_bytes += g->stride;
  g->vertices++;
}

/* GPU mode's raster_triangle: culls, then queues the triangle in NDC with
 * front faces counter-clockwise (y up). */
static void gpu_triangle(Raster_State *rs, const Vertex *a, const Vertex *b, const Vertex *c, const Vertex *provoking) {
  Gpu_Draw *g = &g_gpu_draw;
  if (!g->active || g->dead) return;
  Screen_Vertex sv[3];
  to_screen(rs, a, &sv[0]);
  to_screen(rs, b, &sv[1]);
  to_screen(rs, c, &sv[2]);
  const int64_t area = (sv[1].fx - sv[0].fx) * (sv[2].fy - sv[0].fy) - (sv[1].fy - sv[0].fy) * (sv[2].fx - sv[0].fx);
  if (area == 0) return;
  const bool clockwise = area > 0; /* on screen, y down */
  const bool front = rs->front_ccw ? !clockwise : clockwise;
  if (rs->cull) {
    if (rs->cull_face == 0x408u) return;
    if (rs->cull_face == 0x404u && front) return;
    if (rs->cull_face == 0x405u && !front) return;
  }
  if (!g->prepared) {
    g->prepared = true;
    if (!gpu_prepare(rs, &sv[0], provoking, front)) {
      g->dead = true;
      rs->ctx->r->gpu_stats.untranslated_draws++;
      return;
    }
  }
  if (g->vertex_bytes + 3u * g->stride > GPU_VERTEX_BYTES) gpu_emit_draw(rs);
  /* Clockwise on screen (y down) is counter-clockwise in NDC (y up): the
   * front face winds that way. */
  const bool keep = clockwise == front;
  gpu_put_vertex(rs, &sv[0], provoking);
  gpu_put_vertex(rs, keep ? &sv[1] : &sv[2], provoking);
  gpu_put_vertex(rs, keep ? &sv[2] : &sv[1], provoking);
  rs->ctx->r->gpu_stats.triangles++;
}

static void gpu_draw_end(Raster_State *rs) {
  Gpu_Draw *g = &g_gpu_draw;
  if (g->active && !g->dead) gpu_emit_draw(rs);
  g->active = false;
}

/* ---- clears, presents, copies ---- */

static void gpu_clear(Raster3d *r, const uint32_t *regs, const Gpu_Memory *mem, uint32_t clear) {
  const uint32_t control = regs[REG_CLEAR_CONTROL];
  const bool color = (clear & 0x3cu) != 0;
  const bool depth = (clear & 1u) != 0;
  const bool stencil = (clear & 2u) != 0;
  Gpu_Rec_Clear c;
  memset(&c, 0, sizeof(c));
  Rect rect = {0, 0, 0, 0};
  if (color) {
    const uint32_t mrt = (clear >> 6) & 0xfu;
    const uint32_t select = regs[REG_CT_SELECT];
    const uint32_t target = mrt < 8u ? (select >> (4u + 3u * mrt)) & 7u : 0u;
    Surface_Desc d;
    if (color_target_desc(regs, target, &d)) {
      rect = (Rect){0, 0, (int32_t)d.width, (int32_t)d.height};
      if (control & 0x100u) rect_scissor(&rect, regs);
      if (control & 0x10u) {
        const uint32_t h = regs[REG_CLEAR_RECT_H], v = regs[REG_CLEAR_RECT_V];
        rect_intersect(&rect, (int32_t)(h & 0xffffu), (int32_t)(v & 0xffffu), (int32_t)(h >> 16), (int32_t)(v >> 16));
      }
      if (control & 0x1000u) rect_viewport_clip(&rect, regs);
      const uint32_t write = (regs[REG_SINGLE_CT_WRITE] & 1u) ? regs[REG_CT_WRITE] : regs[REG_CT_WRITE + target];
      for (uint32_t i = 0; i < 4; i++)
        if ((clear >> (2u + i)) & 1u && (write >> (4u * i)) & 1u) c.color_mask |= 1u << i;
      if (rect.x0 < rect.x1 && rect.y0 < rect.y1 && c.color_mask) {
        const bool full = rect.x0 == 0 && rect.y0 == 0 && rect.x1 == (int32_t)d.width && rect.y1 == (int32_t)d.height &&
                          c.color_mask == 0xfu;
        Raster3d_Gpu_Surface *s = gpu_surface_get(r, &d, mem, !full);
        c.color_id = s->id;
        for (uint32_t i = 0; i < 4; i++) c.color[i] = regs[REG_CLEAR_COLOR + i];
        c.flags |= GPU_CLEAR_COLOR;
      }
    }
  }
  if (depth || stencil) {
    Surface_Desc d;
    if (zeta_desc(regs, &d)) {
      Rect zr = {0, 0, (int32_t)d.width, (int32_t)d.height};
      if (control & 0x100u) rect_scissor(&zr, regs);
      const uint32_t sbyte = stencil_byte(d.format);
      const bool clear_depth = depth && d.format != ZT_S8;
      const uint8_t smask = (uint8_t)regs[REG_STENCIL_FRONT + 6u];
      const bool clear_stencil = stencil && sbyte != ZT_NO_STENCIL && smask;
      if ((clear_depth || clear_stencil) && zr.x0 < zr.x1 && zr.y0 < zr.y1) {
        if (c.flags) { /* a colour clear too: one record each */
          c.rect[0] = rect.x0;
          c.rect[1] = rect.y0;
          c.rect[2] = rect.x1 - rect.x0;
          c.rect[3] = rect.y1 - rect.y0;
          gpu_stream_write(r->gpu, GPU_REC_CLEAR, &c, sizeof(c));
          memset(&c, 0, sizeof(c));
        }
        Raster3d_Gpu_Surface *s = gpu_surface_get(r, &d, mem, false);
        c.depth_id = s->id;
        if (clear_depth) c.flags |= GPU_CLEAR_DEPTH;
        if (clear_stencil) c.flags |= GPU_CLEAR_STENCIL;
        c.depth = regs[REG_Z_CLEAR];
        c.stencil = regs[REG_STENCIL_CLEAR] & 0xffu;
        c.stencil_mask = smask;
        rect = zr;
      }
    }
  }
  if (!c.flags) return;
  c.rect[0] = rect.x0;
  c.rect[1] = rect.y0;
  c.rect[2] = rect.x1 - rect.x0;
  c.rect[3] = rect.y1 - rect.y0;
  gpu_stream_write(r->gpu, GPU_REC_CLEAR, &c, sizeof(c));
}

bool raster3d_gpu_present(Raster3d *r, uint64_t cpu_address, uint32_t width, uint32_t height, const int32_t crop[4],
                          uint32_t flags) {
  if (!r || !r->gpu) return false;
  Raster3d_Gpu_Surface *s = NULL;
  for (uint32_t i = 0; i < RASTER_GPU_SURFACES && !s; i++) {
    Raster3d_Gpu_Surface *c = &r->gpu_surfaces[i];
    if (c->in_use && !c->depth && c->cpu_address == cpu_address && c->width >= width) s = c;
  }
  if (!s) return false;
  Gpu_Rec_Present p;
  memset(&p, 0, sizeof(p));
  p.id = s->id;
  p.rect[0] = crop[2] ? crop[0] : 0;
  p.rect[1] = crop[2] ? crop[1] : 0;
  p.rect[2] = crop[2] ? crop[2] : (int32_t)width;
  p.rect[3] = crop[2] ? crop[3] : (int32_t)(height < s->height ? height : s->height);
  p.flags = flags;
  gpu_stream_write(r->gpu, GPU_REC_PRESENT, &p, sizeof(p));
  __atomic_fetch_add((uint32_t *)(void *)(r->gpu->header + GPU_STREAM_OFF_PRESENTS), 1u, __ATOMIC_RELAXED);
  gpu_stream_publish(r->gpu);
  r->gpu_stats.presents++;
  return true;
}

bool raster3d_gpu_copy(Raster3d *r, const Gpu_Memory *mem, const Raster3d_Surface_Ref *src,
                       const Raster3d_Surface_Ref *dst, const int32_t src_rect[4], const int32_t dst_rect[4],
                       bool linear) {
  if (!r || !r->gpu) return false;
  Raster3d_Gpu_Surface *from = gpu_surface_at(r, src->address, src->width);
  if (!from || from->depth) return false;
  const Color_Format *f = color_format(dst->format);
  if (!f) return false;
  Surface_Desc d;
  memset(&d, 0, sizeof(d));
  d.address = dst->address;
  d.width = dst->width;
  d.height = dst->height;
  d.format = dst->format;
  d.bytes_per_pixel = f->bytes;
  d.block_linear = dst->block_linear;
  d.block_height_log2 = dst->block_height_log2;
  d.pitch = dst->pitch;
  const bool full = dst_rect[0] == 0 && dst_rect[1] == 0 && dst_rect[2] == (int32_t)dst->width &&
                    dst_rect[3] == (int32_t)dst->height;
  Raster3d_Gpu_Surface *to = gpu_surface_get(r, &d, mem, !full);
  Gpu_Rec_Copy c;
  memset(&c, 0, sizeof(c));
  c.src_id = from->id;
  c.dst_id = to->id;
  memcpy(c.src_rect, src_rect, sizeof(c.src_rect));
  memcpy(c.dst_rect, dst_rect, sizeof(c.dst_rect));
  c.filter = linear ? 1u : 0u;
  gpu_stream_write(r->gpu, GPU_REC_COPY, &c, sizeof(c));
  r->gpu_stats.copies++;
  return true;
}

/* ---- draws -------------------------------------------------------- */

static Draw_Context g_draw_context; /* large (vertex windows); one draw at a time */

void raster3d_draw(Raster3d *r, const uint32_t *regs, const Raster3d_Bindings *bindings, const Gpu_Memory *mem,
                   const Raster3d_Draw *draw) {
  if (!r || !r->ready || !draw->count || r->skip_draws) return;
  if (!(regs[REG_RASTER_ENABLE] & 1u) && regs[REG_RASTER_ENABLE] != 0) return;
  r->stats.draws++;
  r->surface_view_count = 0; /* no worker holds last draw's views */
  r->draw_serial++;          /* textures this draw touches are not evicted during it */
  Draw_Context *ctx = &g_draw_context;
  ctx->r = r;
  ctx->regs = regs;
  ctx->mem = mem;
  ctx->instance = draw->instance;
  ctx->resolver.ctx = ctx;
  ctx->resolver.count = 0;
  ctx->vs = NULL;
  ctx->ps = NULL;
  memset(ctx->window_size, 0, sizeof(ctx->window_size));
  const uint64_t region = addr40(regs[REG_PROGRAM_REGION], regs[REG_PROGRAM_REGION + 1u]);
  uint32_t vs_group = 0, ps_group = 4;
  for (uint32_t j = 0; j < PIPELINE_STAGES; j++) {
    const uint32_t *p = regs + REG_PIPELINE + j * REG_PIPELINE_STRIDE;
    if (!(p[0] & 1u)) continue;
    const uint32_t type = (p[0] >> 4) & 0xfu;
    const uint32_t group = p[4] & 7u;
    if (type == SM_STAGE_VERTEX || type == 0u) {
      ctx->vs = program_get(r, region + p[1], mem);
      vs_group = group;
    } else if (type == SM_STAGE_PIXEL) {
      ctx->ps = program_get(r, region + p[1], mem);
      ps_group = group;
    }
  }
  if (!ctx->vs || !ctx->ps) {
    if (!r->stats.skipped_draws) {
      for (uint32_t j = 0; j < PIPELINE_STAGES; j++) {
        const uint32_t *p = regs + REG_PIPELINE + j * REG_PIPELINE_STRIDE;
        log_warn("[gpu] draw skipped (no %s program): pipeline %u ctl 0x%x offset 0x%x group %u region %llx",
                 ctx->vs ? "pixel" : "vertex", j, p[0], p[1], p[4] & 7u, (unsigned long long)region);
      }
    }
    r->stats.skipped_draws++;
    return;
  }
  env_setup(ctx, 0, ctx->vs, bindings, vs_group);
  env_setup(ctx, 1, ctx->ps, bindings, ps_group);
  static Raster_State rs;
  if (!setup_state(ctx, &rs)) {
    if (!r->stats.skipped_draws) {
      Surface_Desc d;
      const bool has_rt = color_target_desc(regs, 0, &d);
      log_warn("[gpu] draw skipped: RT0 %s (fmt 0x%02x %ux%u @%llx), CT_SELECT 0x%x, CT_WRITE 0x%x, zeta %s, clip %d,%d-%d,%d",
               has_rt ? "ok" : "unusable", regs[REG_RT + 4], regs[REG_RT + 2], regs[REG_RT + 3],
               (unsigned long long)(((uint64_t)regs[REG_RT] << 32) | regs[REG_RT + 1]), regs[REG_CT_SELECT],
               regs[REG_CT_WRITE], (regs[REG_ZT_SELECT] & 1u) ? "on" : "off", rs.clip.x0, rs.clip.y0, rs.clip.x1, rs.clip.y1);
    }
    r->stats.skipped_draws++;
    return;
  }
  if (r->gpu && !gpu_draw_begin(ctx, &rs)) {
    r->stats.skipped_draws++;
    return;
  }
  rs.span_ok = span_state_ok(&rs);
  rs.row_begin = INT64_MIN;
  rs.row_end = INT64_MAX;
  rs.thread = r->thread;
  rs.ps_env = ctx->env[1];
  rs.resolver = ctx->resolver;
  rs.ps_env.user = &rs.resolver;
  memset(&rs.stats, 0, sizeof(rs.stats));
  static Vertex_Cache cache;
  memset(cache.valid, 0, sizeof(cache.valid));
  Assembler as;
  memset(&as, 0, sizeof(as));
  as.topology = draw->topology;
  const bool restart = (regs[REG_PRIMITIVE_RESTART] & 1u) != 0;
  const uint32_t restart_index = regs[REG_PRIMITIVE_RESTART_INDEX];
  const uint32_t base_vertex = regs[REG_BASE_VERTEX];
  /* Each chunk's vertices are shaded SM_LANES at a time before the
   * assembler consumes it; RESTART marks a primitive restart. */
#define RESTART UINT32_MAX
  static uint32_t chunk[INDEX_BATCH], vertices[INDEX_BATCH];
  switch (draw->kind) {
  case RASTER_DRAW_ARRAYS:
    for (uint32_t done = 0; done < draw->count;) {
      const uint32_t n = draw->count - done < INDEX_BATCH ? draw->count - done : INDEX_BATCH;
      for (uint32_t i = 0; i < n; i++) chunk[i] = draw->first + done + i;
      vertex_prefetch(ctx, &cache, chunk, n);
      for (uint32_t i = 0; i < n; i++) assemble(&rs, &cache, &as, chunk[i]);
      done += n;
    }
    break;
  default: {
    const bool inline_indices = draw->kind == RASTER_DRAW_INLINE;
    const uint32_t size = inline_indices ? 4u : draw->index_size;
    const uint64_t base = addr40(regs[REG_INDEX_BUFFER], regs[REG_INDEX_BUFFER + 1u]) + (uint64_t)draw->first * size;
    const uint32_t restart_value = size == 4u ? restart_index : (restart_index & ((1u << (8u * size)) - 1u));
    static uint8_t batch[INDEX_BATCH * 4u];
    for (uint32_t done = 0; done < draw->count;) {
      const uint32_t n = draw->count - done < INDEX_BATCH ? draw->count - done : INDEX_BATCH;
      if (inline_indices) memcpy(batch, draw->inline_indices + done, (size_t)n * 4u);
      else if (!mem->read(mem->user, base + (uint64_t)done * size, batch, (uint64_t)n * size)) break;
      uint32_t vertex_count = 0;
      for (uint32_t i = 0; i < n; i++) {
        uint32_t index = 0;
        memcpy(&index, batch + (size_t)i * size, size);
        chunk[i] = (restart && index == restart_value) ? RESTART : index + base_vertex;
        if (chunk[i] != RESTART) vertices[vertex_count++] = chunk[i];
      }
      vertex_prefetch(ctx, &cache, vertices, vertex_count);
      for (uint32_t i = 0; i < n; i++) {
        if (chunk[i] == RESTART) assemble_end(&rs, &cache, &as);
        else assemble(&rs, &cache, &as, chunk[i]);
      }
      done += n;
    }
    break;
  }
  }
#undef RESTART
  const uint64_t pixels_before = r->stats.pixels, triangles_before = r->stats.triangles;
  assemble_end(&rs, &cache, &as);
  flush_triangles(&rs);
  if (r->gpu) gpu_draw_end(&rs);
  if (r->trace_draws) {
    const Target *t0 = &rs.targets[0];
    log_info("[gpu] draw %llu: topo %u count %u vs %llx ps %llx | rt0 %llx fmt 0x%02x %ux%u targets %u mask %x | "
             "blend %d op %u/%u src %u/%u dst %u/%u | depth %d/%d func %u | cull %d | tris %llu px %llu",
             (unsigned long long)r->stats.draws, draw->topology, draw->count, (unsigned long long)ctx->vs->address,
             (unsigned long long)ctx->ps->address, (unsigned long long)(t0->surface ? t0->surface->address : 0),
             t0->surface ? t0->surface->format : 0u, t0->surface ? t0->surface->width : 0u,
             t0->surface ? t0->surface->height : 0u, rs.target_count, t0->write_mask, t0->blend, t0->color_op,
             t0->alpha_op, t0->color_src, t0->alpha_src, t0->color_dst, t0->alpha_dst, rs.depth_test, rs.depth_write,
             rs.depth_func, rs.cull, (unsigned long long)(r->stats.triangles - triangles_before),
             (unsigned long long)(r->stats.pixels - pixels_before));
    const Tex_Resolver *res = &g_band_state[0].resolver;
    for (uint32_t i = 0; i < res->count; i++) {
      const Raster3d_Texture *t = res->texture[i];
      log_info("[gpu]   texture %08x: %ux%u fmt 0x%02x @%llx", res->handle[i], t ? t->image.width : 0u,
               t ? t->image.height : 0u, t ? t->image.header.format : 0u, (unsigned long long)(t ? t->address : 0));
    }
  }
}
