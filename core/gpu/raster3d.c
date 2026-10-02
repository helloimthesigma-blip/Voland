/**
 * Maxwell 3D reference renderer. See raster3d.h.
 *
 * Register words are B197 method byte offsets / 4 (NVIDIA clb197.h).
 */
#include "gpu/raster3d.h"

#include <math.h>
#include <string.h>

#include "common/log.h"
#include "gpu/block_linear.h"
#include "gpu/gpu_channel.h"

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
#define SUBPIXEL_BITS 8
#define SUBPIXEL_ONE (1 << SUBPIXEL_BITS)
#define GUARD_BAND 4096.0f          /* clip-space x/y limit, in w units */
#define MAX_POLY 12u
#define VERTEX_CACHE 32u
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
#define CBUF_OFFSET (TEXTURE_POOL_OFFSET + RASTER_TEXTURE_POOL_BYTES)
#define CBUF_BYTES ((size_t)2 * SM_CBUF_SLOTS * CBUF_SLOT_BYTES)
#define THREAD_OFFSET (CBUF_OFFSET + CBUF_BYTES)
#define STORAGE_BYTES (THREAD_OFFSET + sizeof(Sm_Thread) + 64u)

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
  r->cbuf_data = s + CBUF_OFFSET;
  r->thread = (Sm_Thread *)(void *)(s + THREAD_OFFSET);
  for (uint32_t i = 0; i < 256u; i++) g_unorm8[i] = (float)i / 255.0f;
  r->ready = true;
}

void raster3d_begin_submission(Raster3d *r) {
  if (!r->ready) return;
  r->submission++;
  /* Decoded textures survive submissions (each is re-hashed against guest
   * memory on first use in a submission); start over when space runs low. */
  if (r->texture_count >= RASTER_TEXTURES * 3u / 4u || r->texture_pool_used >= RASTER_TEXTURE_POOL_BYTES / 2u) {
    r->texture_count = 0;
    r->texture_pool_used = 0;
  }
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

static uint32_t zeta_bytes(uint32_t format) {
  switch (format) {
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

/* Finds or creates the cached copy of a surface. `load`: fetch guest
 * contents now (false when the caller overwrites all of it). */
static Raster3d_Surface *surface_get(Raster3d *r, const Surface_Desc *d, const Gpu_Memory *mem, bool load) {
  Raster3d_Surface *victim = NULL;
  r->tick++;
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
  for (uint32_t i = 0; i < RASTER_SURFACES; i++) surface_write_back(r, &r->surfaces[i], mem);
  /* The guest may change these before the next submission. */
  for (uint32_t i = 0; i < RASTER_SURFACES; i++) r->surfaces[i].loaded = r->surfaces[i].in_use ? false : r->surfaces[i].loaded;
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

void raster3d_clear(Raster3d *r, const uint32_t *regs, const Gpu_Memory *mem, uint32_t clear) {
  if (!r || !r->ready) return;
  r->stats.clears++;
  const uint32_t control = regs[REG_CLEAR_CONTROL];
  const bool color = (clear & 0x3cu) != 0;
  const bool depth = (clear & 1u) != 0;
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
        if (comps == 0xfu) encode_color(f, value, px);
        for (int32_t y = rect.y0; y < rect.y1; y++) {
          uint8_t *line = s->pixels + (uint64_t)y * row;
          if (comps == 0xfu && bpp == 4u) {
            uint32_t v;
            memcpy(&v, px, 4);
            uint32_t *q = (uint32_t *)(void *)(line + (uint64_t)rect.x0 * 4u);
            for (int32_t x = rect.x0; x < rect.x1; x++) *q++ = v;
            continue;
          }
          for (int32_t x = rect.x0; x < rect.x1; x++) {
            uint8_t *p = line + (uint64_t)x * bpp;
            if (comps == 0xfu) {
              for (uint32_t k = 0; k < bpp; k++) p[k] = px[k];
              continue;
            }
            float cur[4];
            decode_color(f, p, cur);
            uint32_t merged[4];
            for (uint32_t c = 0; c < 4; c++) merged[c] = (comps >> c) & 1u ? value[c] : u32f(cur[c]);
            encode_color(f, merged, p);
          }
        }
        s->dirty = true;
      }
    }
  }
  if (depth) {
    Surface_Desc d;
    if (zeta_desc(regs, &d)) {
      Rect rect = {0, 0, (int32_t)d.width, (int32_t)d.height};
      if (control & 0x100u) rect_scissor(&rect, regs);
      const bool full = rect.x0 == 0 && rect.y0 == 0 && rect.x1 == (int32_t)d.width && rect.y1 == (int32_t)d.height;
      Raster3d_Surface *s = surface_get(r, &d, mem, !full);
      if (full) s->loaded = true;
      const float z = f32(regs[REG_Z_CLEAR]);
      for (int32_t y = rect.y0; y < rect.y1; y++)
        for (int32_t x = rect.x0; x < rect.x1; x++)
          write_depth(s->format, s->pixels + ((uint64_t)y * s->width + (uint64_t)x) * s->bytes_per_pixel, z);
      s->dirty = true;
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

typedef struct Draw_Context {
  Raster3d *r;
  const uint32_t *regs;
  const Gpu_Memory *mem;
  Sm_Env env[2];         /* 0 vertex, 1 pixel */
  const Sm_Program *vs;
  const Sm_Program *ps;
  uint32_t instance;
  /* per-draw handle -> texture resolution */
  uint32_t resolved_handle[16];
  Raster3d_Texture *resolved_texture[16];
  Tex_Sampler resolved_sampler[16];
  uint32_t resolved_count;
  /* vertex fetch windows */
  uint64_t window_base[RASTER_STREAMS];
  uint32_t window_size[RASTER_STREAMS];
  uint8_t window[RASTER_STREAMS][RASTER_STREAM_WINDOW];
} Draw_Context;

static uint64_t content_hash(const uint8_t *p, uint64_t n) {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
  uint64_t i = 0;
  for (; i + 8u <= n; i += 8u) {
    uint64_t v;
    memcpy(&v, p + i, 8);
    h = (h ^ v) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 32;
  }
  for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

static Raster3d_Texture *texture_load(Raster3d *r, const uint32_t tic[8], const Gpu_Memory *mem) {
  Raster3d_Texture *t = NULL;
  for (uint32_t i = 0; i < r->texture_count; i++) {
    if (r->textures[i].valid && memcmp(r->textures[i].tic, tic, sizeof(r->textures[i].tic)) == 0) {
      t = &r->textures[i];
      break;
    }
  }
  if (t && t->validated == r->submission) return t;
  Tex_Header h;
  tex_header_parse(tic, &h);
  const uint64_t raw_bytes = tex_read_bytes(&h);
  const uint64_t decoded = tex_decoded_bytes(&h);
  const size_t decoded_aligned = (size_t)((decoded + 63u) & ~63ull);
  /* Raw bytes are staged past the pool's high-water mark. */
  const size_t need = (t ? 0u : decoded_aligned) + (size_t)raw_bytes;
  if (!raw_bytes || !decoded || (!t && r->texture_count >= RASTER_TEXTURES) ||
      r->texture_pool_used + need > RASTER_TEXTURE_POOL_BYTES) {
    if (!r->stats.texture_misses)
      log_warn("[gpu] texture %ux%u format 0x%02x layout %u type %u: %s", h.width, h.height, h.format, h.layout, h.type,
               (!raw_bytes || !decoded) ? "unsupported format" : "texture pool full");
    r->stats.texture_misses++;
    return NULL;
  }
  /* A texture the GPU just rendered: write those pixels back first. */
  for (uint32_t i = 0; i < RASTER_SURFACES; i++) {
    Raster3d_Surface *s = &r->surfaces[i];
    if (s->in_use && s->dirty && s->address < h.address + raw_bytes && h.address < s->address + s->guest_bytes)
      surface_write_back(r, s, mem);
  }
  uint8_t *dst = t ? (uint8_t *)(uintptr_t)t->image.texels : r->texture_pool + r->texture_pool_used;
  uint8_t *raw = r->texture_pool + r->texture_pool_used + (t ? 0u : decoded_aligned);
  if (!mem->read(mem->user, h.address, raw, raw_bytes)) {
    if (!r->stats.texture_misses)
      log_warn("[gpu] texture %ux%u format 0x%02x at %llx: unreadable (%llu bytes)", h.width, h.height, h.format,
               (unsigned long long)h.address, (unsigned long long)raw_bytes);
    r->stats.texture_misses++;
    return NULL;
  }
  const uint64_t hash = content_hash(raw, raw_bytes);
  if (t && t->raw_hash == hash) {
    t->validated = r->submission;
    return t;
  }
  Raster3d_Texture *slot = t ? t : &r->textures[r->texture_count];
  if (!tex_decode(&h, raw, dst, &slot->image)) {
    r->stats.texture_misses++;
    return NULL;
  }
  log_debug("[gpu] texture %ux%u fmt 0x%02x types %u%u%u%u swizzle %u%u%u%u layout %u type %u srgb %d @%llx",
            h.width, h.height, h.format, h.data_type[0], h.data_type[1], h.data_type[2], h.data_type[3], h.swizzle[0],
            h.swizzle[1], h.swizzle[2], h.swizzle[3], h.layout, h.type, h.srgb, (unsigned long long)h.address);
  memcpy(slot->tic, tic, sizeof(slot->tic));
  slot->raw_hash = hash;
  slot->validated = r->submission;
  slot->valid = true;
  if (!t) {
    r->texture_count++;
    r->texture_pool_used += decoded_aligned; /* the raw staging after it is reusable */
  }
  return slot;
}

static bool resolve_texture(Draw_Context *ctx, uint32_t handle, Raster3d_Texture **tex, Tex_Sampler **sampler) {
  for (uint32_t i = 0; i < ctx->resolved_count; i++) {
    if (ctx->resolved_handle[i] == handle) {
      *tex = ctx->resolved_texture[i];
      *sampler = &ctx->resolved_sampler[i];
      return *tex != NULL;
    }
  }
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
  if (tic_pool && ctx->mem->read(ctx->mem->user, tic_pool + (uint64_t)tic_index * TEX_HEADER_BYTES, tic, sizeof(tic)))
    t = texture_load(ctx->r, tic, ctx->mem);
  if (tsc_pool && ctx->mem->read(ctx->mem->user, tsc_pool + (uint64_t)tsc_index * TEX_SAMPLER_BYTES, tsc, sizeof(tsc)))
    tex_sampler_parse(tsc, &s);
  uint32_t slot = ctx->resolved_count;
  if (slot >= 16u) slot = 15u; /* overwrite the last entry */
  else ctx->resolved_count++;
  ctx->resolved_handle[slot] = handle;
  ctx->resolved_texture[slot] = t;
  ctx->resolved_sampler[slot] = s;
  *tex = t;
  *sampler = &ctx->resolved_sampler[slot];
  return t != NULL;
}

static void env_texture(void *user, const Sm_Tex_Request *req, uint32_t out[4]) {
  Draw_Context *ctx = (Draw_Context *)user;
  Raster3d_Texture *t = NULL;
  Tex_Sampler *s = NULL;
  if (!resolve_texture(ctx, req->handle, &t, &s)) {
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

static bool env_global_read(void *user, uint64_t va, void *out, uint32_t size) {
  const Draw_Context *ctx = (const Draw_Context *)user;
  return ctx->mem->read(ctx->mem->user, va, out, size);
}

static bool env_global_write(void *user, uint64_t va, const void *src, uint32_t size) {
  const Draw_Context *ctx = (const Draw_Context *)user;
  return ctx->mem->write(ctx->mem->user, va, src, size);
}

/* Binds the constant buffers `program` reads from `group`. */
static void env_setup(Draw_Context *ctx, uint32_t stage, const Sm_Program *program, const Raster3d_Bindings *b,
                      uint32_t group) {
  Sm_Env *env = &ctx->env[stage];
  memset(env, 0, sizeof(*env));
  env->user = ctx;
  env->texture = env_texture;
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

static bool run_vertex(Draw_Context *ctx, uint32_t index, Vertex *out) {
  Sm_Thread *t = ctx->r->thread;
  sm_thread_reset(t, 1);
  const Sm_Header *h = &ctx->vs->header;
  for (uint32_t v = 0; v < SM_ATTR_GENERIC_COUNT; v++) {
    const uint32_t mask = (h->input_generic[v / 8u] >> ((v % 8u) * 4u)) & 0xfu;
    const uint32_t base = (SM_ATTR_GENERIC / 4u) + v * 4u;
    if (!mask) continue;
    uint32_t value[4];
    fetch_attribute(ctx, v, index, value);
    for (uint32_t c = 0; c < 4; c++) t->attr_in[base + c][0] = value[c];
  }
  for (uint32_t i = 0; i < SM_ATTRIBUTE_WORDS; i++) t->attr_out[i][0] = 0;
  t->attr_out[(SM_ATTR_POSITION / 4u) + 3u][0] = u32f(1.0f);
  t->vertex_id[0] = index;
  t->instance_id[0] = ctx->instance;
  if (!sm_run(ctx->vs, &ctx->env[0], t)) {
    ctx->r->stats.shader_faults++;
    return false;
  }
  for (uint32_t c = 0; c < 4; c++) out->pos[c] = f32(t->attr_out[SM_ATTR_POSITION / 4u + c][0]);
  for (uint32_t i = 0; i < MAX_VARYINGS; i++) out->varying[i] = t->attr_out[SM_ATTR_GENERIC / 4u + i][0];
  return true;
}

static const Vertex *vertex_get(Draw_Context *ctx, Vertex_Cache *cache, uint32_t index, bool *ok) {
  const uint32_t slot = index % VERTEX_CACHE;
  if (cache->valid[slot] && cache->index[slot] == index) return &cache->vertex[slot];
  cache->valid[slot] = run_vertex(ctx, index, &cache->vertex[slot]);
  cache->index[slot] = index;
  *ok = cache->valid[slot];
  return &cache->vertex[slot];
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

static bool setup_state(Draw_Context *ctx, Raster_State *rs) {
  Raster3d *r = ctx->r;
  const uint32_t *regs = ctx->regs;
  memset(rs, 0, sizeof(*rs));
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
    if ((int32_t)d.width < width) width = (int32_t)d.width;
    if ((int32_t)d.height < height) height = (int32_t)d.height;
    rs->target_count = i + 1u;
  }
  Surface_Desc zd;
  if (zeta_desc(regs, &zd)) {
    rs->depth_test = (regs[REG_DEPTH_TEST] & 1u) != 0;
    rs->depth_write = (regs[REG_DEPTH_WRITE] & 1u) != 0;
    rs->depth_func = regs[REG_DEPTH_FUNC];
    if (rs->depth_test || rs->depth_write) {
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
  if (!rs->depth || !rs->depth_test || rs->depth_reg != 0xffu) return false;
  const uint8_t *zp = rs->depth->pixels + ((uint64_t)py * rs->depth->width + (uint64_t)px) * rs->depth->bytes_per_pixel;
  return !depth_compare(rs->depth_func, depth, read_depth(rs->depth->format, zp));
}

static void output_pixel(Raster_State *rs, int32_t px, int32_t py, float depth, const uint32_t *out_regs);

/* Shades one pixel, or reuses `shared` (a flat triangle's one result). */
static void shade_pixel(Raster_State *rs, int32_t px, int32_t py, float depth, const Plane *planes, const Plane *inv_w,
                        float x0, float y0, bool front, const Screen_Vertex *provoking, Frag_Result *shared) {
  if (early_depth_reject(rs, px, py, depth)) return;
  if (!shared->valid) {
    Raster3d *r = rs->ctx->r;
    Sm_Thread *t = r->thread;
    sm_thread_reset_light(t, 1);
    t->front_facing = front ? SM_ALL_LANES : 0;
    setup_lane(rs, t, 0, px, py, depth, planes, inv_w, x0, y0, provoking);
    const bool ok = sm_run(rs->ctx->ps, &rs->ctx->env[1], t);
    if (!ok) r->stats.shader_faults++;
    shared->killed = !ok || (t->killed & 1u);
    for (uint32_t i = 0; i < rs->out_regs; i++) shared->regs[i] = t->r[i][0];
    shared->valid = true;
  }
  if (!shared->killed) output_pixel(rs, px, py, depth, shared->regs);
}

/* Pixels waiting to be shaded together. */
typedef struct Pixel_Batch {
  uint32_t count;
  int32_t x[SM_LANES], y[SM_LANES];
  float z[SM_LANES];
} Pixel_Batch;

static void shade_batch(Raster_State *rs, Pixel_Batch *b, const Plane *planes, const Plane *inv_w, float x0, float y0,
                        bool front, const Screen_Vertex *provoking) {
  if (!b->count) return;
  Raster3d *r = rs->ctx->r;
  Sm_Thread *t = r->thread;
  sm_thread_reset_light(t, b->count);
  t->front_facing = front ? SM_ALL_LANES : 0;
  for (uint32_t l = 0; l < b->count; l++) setup_lane(rs, t, l, b->x[l], b->y[l], b->z[l], planes, inv_w, x0, y0, provoking);
  const bool ok = sm_run(rs->ctx->ps, &rs->ctx->env[1], t);
  if (!ok) r->stats.shader_faults++;
  for (uint32_t l = 0; ok && l < b->count; l++) {
    if ((t->killed >> l) & 1u) continue;
    uint32_t regs[MAX_TARGETS * 4u + 1u];
    for (uint32_t i = 0; i < rs->out_regs; i++) regs[i] = t->r[i][l];
    output_pixel(rs, b->x[l], b->y[l], b->z[l], regs);
  }
  b->count = 0;
}

/* Depth, alpha test, blending and the colour write for one shaded pixel. */
static void output_pixel(Raster_State *rs, int32_t px, int32_t py, float depth, const uint32_t *out_regs) {
  Raster3d *r = rs->ctx->r;
  uint8_t *zp = rs->depth ? rs->depth->pixels + ((uint64_t)py * rs->depth->width + (uint64_t)px) * rs->depth->bytes_per_pixel
                          : NULL;
  if (rs->alpha_test) {
    const uint8_t reg = rs->color_reg[0][3];
    const float alpha = reg == 0xffu ? 1.0f : f32(out_regs[reg]);
    if (!depth_compare(rs->alpha_func, alpha, rs->alpha_ref)) return;
  }
  if (rs->depth_reg != 0xffu) depth = f32(out_regs[rs->depth_reg]);
  if (rs->depth) {
    if (rs->depth_test && rs->depth_reg != 0xffu &&
        !depth_compare(rs->depth_func, depth, read_depth(rs->depth->format, zp)))
      return;
    if (rs->depth_write) {
      write_depth(rs->depth->format, zp, depth);
      rs->depth->dirty = true;
    }
  }
  r->stats.pixels++;
  for (uint32_t i = 0; i < rs->target_count; i++) {
    Target *tg = &rs->targets[i];
    if (!tg->surface) continue;
    const uint32_t src_target = rs->mrt ? i : 0u;
    uint32_t color[4];
    for (uint32_t c = 0; c < 4; c++) {
      const uint8_t reg = rs->color_reg[src_target][c];
      color[c] = reg == 0xffu ? (c == 3u ? u32f(1.0f) : 0u) : out_regs[reg];
    }
    uint8_t *p = tg->surface->pixels + ((uint64_t)py * tg->surface->width + (uint64_t)px) * tg->surface->bytes_per_pixel;
    if (tg->rgba8 && tg->write_mask == 0xfu && (!tg->blend || tg->alpha_blend)) {
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

static void raster_triangle(Raster_State *rs, const Vertex *a, const Vertex *b, const Vertex *c, const Vertex *provoking) {
  Screen_Vertex sv[3];
  to_screen(rs, a, &sv[0]);
  to_screen(rs, b, &sv[1]);
  to_screen(rs, c, &sv[2]);
  int64_t area = (sv[1].fx - sv[0].fx) * (sv[2].fy - sv[0].fy) - (sv[1].fy - sv[0].fy) * (sv[2].fx - sv[0].fx);
  if (area == 0) return;
  /* Facing: positive area is clockwise on screen (y down). */
  const bool clockwise = area > 0;
  const bool front = rs->front_ccw ? !clockwise : clockwise;
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
  rs->ctx->r->stats.triangles++;
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
  if (x0 > x1 || y0 > y1) return;
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
  for (int64_t y = y0; y <= y1; y++) {
    int64_t e0 = e_row[0], e1 = e_row[1], e2 = e_row[2];
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
          if (++batch.count == SM_LANES) shade_batch(rs, &batch, planes, &wp, sv[0].x, sv[0].y, front, &prov);
        }
      }
      e0 += ex[0];
      e1 += ex[1];
      e2 += ex[2];
    }
    e_row[0] += ey[0];
    e_row[1] += ey[1];
    e_row[2] += ey[2];
  }
  shade_batch(rs, &batch, planes, &wp, sv[0].x, sv[0].y, front, &prov);
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

/* ---- draws -------------------------------------------------------- */

static Draw_Context g_draw_context; /* large (vertex windows); one draw at a time */

void raster3d_draw(Raster3d *r, const uint32_t *regs, const Raster3d_Bindings *bindings, const Gpu_Memory *mem,
                   const Raster3d_Draw *draw) {
  if (!r || !r->ready || !draw->count) return;
  if (!(regs[REG_RASTER_ENABLE] & 1u) && regs[REG_RASTER_ENABLE] != 0) return;
  r->stats.draws++;
  Draw_Context *ctx = &g_draw_context;
  ctx->r = r;
  ctx->regs = regs;
  ctx->mem = mem;
  ctx->instance = draw->instance;
  ctx->resolved_count = 0;
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
    r->stats.skipped_draws++;
    return;
  }
  env_setup(ctx, 0, ctx->vs, bindings, vs_group);
  env_setup(ctx, 1, ctx->ps, bindings, ps_group);
  static Raster_State rs;
  if (!setup_state(ctx, &rs)) {
    r->stats.skipped_draws++;
    return;
  }
  static Vertex_Cache cache;
  memset(cache.valid, 0, sizeof(cache.valid));
  Assembler as;
  memset(&as, 0, sizeof(as));
  as.topology = draw->topology;
  const bool restart = (regs[REG_PRIMITIVE_RESTART] & 1u) != 0;
  const uint32_t restart_index = regs[REG_PRIMITIVE_RESTART_INDEX];
  const uint32_t base_vertex = regs[REG_BASE_VERTEX];
  switch (draw->kind) {
  case RASTER_DRAW_ARRAYS:
    for (uint32_t i = 0; i < draw->count; i++) assemble(&rs, &cache, &as, draw->first + i);
    break;
  case RASTER_DRAW_INLINE:
    for (uint32_t i = 0; i < draw->count; i++) {
      const uint32_t index = draw->inline_indices[i];
      if (restart && index == restart_index) {
        assemble_end(&rs, &cache, &as);
        continue;
      }
      assemble(&rs, &cache, &as, index + base_vertex);
    }
    break;
  default: {
    const uint32_t size = draw->index_size;
    const uint64_t base = addr40(regs[REG_INDEX_BUFFER], regs[REG_INDEX_BUFFER + 1u]) + (uint64_t)draw->first * size;
    static uint8_t batch[INDEX_BATCH * 4u];
    for (uint32_t done = 0; done < draw->count;) {
      const uint32_t n = draw->count - done < INDEX_BATCH ? draw->count - done : INDEX_BATCH;
      if (!mem->read(mem->user, base + (uint64_t)done * size, batch, (uint64_t)n * size)) break;
      for (uint32_t i = 0; i < n; i++) {
        uint32_t index = 0;
        memcpy(&index, batch + (size_t)i * size, size);
        if (restart && index == (size == 4u ? restart_index : (restart_index & ((1u << (8u * size)) - 1u)))) {
          assemble_end(&rs, &cache, &as);
          continue;
        }
        assemble(&rs, &cache, &as, index + base_vertex);
      }
      done += n;
    }
    break;
  }
  }
  assemble_end(&rs, &cache, &as);
}
