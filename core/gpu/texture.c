/**
 * Texture headers, samplers and texel decoding. See texture.h.
 */
#include "gpu/texture.h"

#include <math.h>
#include <string.h>

#include "gpu/astc.h"
#include "gpu/bc7.h"
#include "gpu/block_linear.h"

/* MW(hi:lo) field of the 256-bit header. */
static uint32_t mw(const uint32_t words[8], uint32_t lo, uint32_t n) {
  const uint32_t word = lo / 32u, bit = lo % 32u;
  uint64_t v = words[word];
  if (word + 1u < 8u) v |= (uint64_t)words[word + 1u] << 32;
  return (uint32_t)((v >> bit) & ((1ull << n) - 1ull));
}

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

void tex_header_parse(const uint32_t words[8], Tex_Header *out) {
  memset(out, 0, sizeof(*out));
  memcpy(out->words, words, sizeof(out->words));
  out->format = mw(words, 0, 7);
  for (uint32_t c = 0; c < 4; c++) out->data_type[c] = (uint8_t)mw(words, 7u + 3u * c, 3);
  for (uint32_t c = 0; c < 4; c++) out->swizzle[c] = (uint8_t)mw(words, 19u + 3u * c, 3);
  const uint32_t version = mw(words, 85, 3);
  out->width = mw(words, 128, 16) + 1u;
  out->height = mw(words, 160, 16) + 1u;
  out->depth = mw(words, 176, 14) + 1u;
  out->type = (Tex_Type)mw(words, 151, 4);
  out->srgb = mw(words, 150, 1) != 0;
  out->normalized = mw(words, 191, 1) != 0;
  out->levels = mw(words, 124, 4) + 1u;
  switch (version) {
  case 3: /* block linear */
  case 4: /* block linear, colour key */
    out->layout = TEX_LAYOUT_BLOCK_LINEAR;
    out->address = ((uint64_t)mw(words, 41, 23) << 9) | ((uint64_t)mw(words, 64, 16) << 32);
    out->block_height_log2 = mw(words, 99, 3);
    out->block_depth_log2 = mw(words, 102, 3);
    break;
  case 2: /* pitch */
  case 1: /* pitch, colour key */
    out->layout = TEX_LAYOUT_PITCH;
    out->address = ((uint64_t)mw(words, 37, 27) << 5) | ((uint64_t)mw(words, 64, 16) << 32);
    out->pitch = mw(words, 96, 16) << 5;
    out->levels = 1;
    break;
  default: /* 1D buffer: 32-bit width (TEXHEAD_1D WIDTH_MINUS_ONE @128..159) */
    out->layout = TEX_LAYOUT_BUFFER;
    out->address = (uint64_t)words[1] | ((uint64_t)mw(words, 64, 16) << 32);
    out->width = words[4] + 1u;
    out->height = 1;
    out->depth = 1;
    out->levels = 1;
    break;
  }
  if (out->type == TEX_TYPE_1D || out->type == TEX_TYPE_1D_ARRAY || out->type == TEX_TYPE_1D_BUFFER) out->height = 1;
  if (out->type == TEX_TYPE_CUBE) out->depth = 6;
  if (out->type == TEX_TYPE_CUBE_ARRAY) out->depth *= 6u;
}

void tex_sampler_parse(const uint32_t words[8], Tex_Sampler *out) {
  memset(out, 0, sizeof(*out));
  out->wrap[0] = (uint8_t)(words[0] & 7u);
  out->wrap[1] = (uint8_t)((words[0] >> 3) & 7u);
  out->wrap[2] = (uint8_t)((words[0] >> 6) & 7u);
  out->depth_compare = ((words[0] >> 9) & 1u) != 0;
  out->compare_func = (uint8_t)((words[0] >> 10) & 7u);
  out->mag_filter = (uint8_t)(words[1] & 7u);
  out->min_filter = (uint8_t)((words[1] >> 4) & 3u);
  out->mip_filter = (uint8_t)((words[1] >> 6) & 3u);
  out->lod_bias = (float)((int32_t)(((words[1] >> 12) & 0x1fffu) << 19) >> 19) / 256.0f; /* signed 5.8 */
  out->min_lod = (float)(words[2] & 0xfffu) / 256.0f;                                    /* unsigned 4.8 */
  out->max_lod = (float)((words[2] >> 12) & 0xfffu) / 256.0f;
  for (uint32_t c = 0; c < 4; c++) out->border[c] = f32(words[4u + c]);
}

/* ---- formats ------------------------------------------------------ */

#define FMT_R32_G32_B32_A32 0x01u
#define FMT_R32_G32_B32 0x02u
#define FMT_R16_G16_B16_A16 0x03u
#define FMT_R32_G32 0x04u
#define FMT_X8B8G8R8 0x07u
#define FMT_A8B8G8R8 0x08u
#define FMT_A2B10G10R10 0x09u
#define FMT_R16_G16 0x0cu
#define FMT_G8R24 0x0du
#define FMT_G24R8 0x0eu
#define FMT_R32 0x0fu
#define FMT_A4B4G4R4 0x12u
#define FMT_A5B5G5R1 0x13u
#define FMT_A1B5G5R5 0x14u
#define FMT_B5G6R5 0x15u
#define FMT_B6G5R5 0x16u
#define FMT_BC7U 0x17u
#define FMT_G8R8 0x18u
#define FMT_R16 0x1bu
#define FMT_Y8_VIDEO 0x1cu
#define FMT_R8 0x1du
#define FMT_G4R4 0x1eu
#define FMT_E5B9G9R9 0x20u
#define FMT_BF10GF11RF11 0x21u
#define FMT_DXT1 0x24u
#define FMT_DXT23 0x25u
#define FMT_DXT45 0x26u
#define FMT_DXN1 0x27u
#define FMT_DXN2 0x28u
#define FMT_Z24S8 0x29u
#define FMT_X8Z24 0x2au
#define FMT_S8Z24 0x2bu
#define FMT_ZF32 0x2fu
#define FMT_ZF32_X24S8 0x30u
#define FMT_Z16 0x3au

typedef struct Format_Info {
  uint8_t format;
  uint8_t bytes;
  uint8_t bw, bh; /* block footprint: 1x1, 4x4 (BCn), ASTC sizes */
  uint8_t bits[4];
} Format_Info;

static const Format_Info k_formats[] = {
    {FMT_R32_G32_B32_A32, 16, 1, 1, {32, 32, 32, 32}}, {FMT_R32_G32_B32, 12, 1, 1, {32, 32, 32, 0}},
    {FMT_R16_G16_B16_A16, 8, 1, 1, {16, 16, 16, 16}}, {FMT_R32_G32, 8, 1, 1, {32, 32, 0, 0}},
    {FMT_X8B8G8R8, 4, 1, 1, {8, 8, 8, 0}}, {FMT_A8B8G8R8, 4, 1, 1, {8, 8, 8, 8}},
    {FMT_A2B10G10R10, 4, 1, 1, {10, 10, 10, 2}}, {FMT_R16_G16, 4, 1, 1, {16, 16, 0, 0}},
    {FMT_G8R24, 4, 1, 1, {24, 8, 0, 0}}, {FMT_G24R8, 4, 1, 1, {8, 24, 0, 0}}, {FMT_R32, 4, 1, 1, {32, 0, 0, 0}},
    {FMT_A4B4G4R4, 2, 1, 1, {4, 4, 4, 4}}, {FMT_A5B5G5R1, 2, 1, 1, {1, 5, 5, 5}}, {FMT_A1B5G5R5, 2, 1, 1, {5, 5, 5, 1}},
    {FMT_B5G6R5, 2, 1, 1, {5, 6, 5, 0}}, {FMT_B6G5R5, 2, 1, 1, {5, 5, 6, 0}}, {FMT_G8R8, 2, 1, 1, {8, 8, 0, 0}},
    {FMT_R16, 2, 1, 1, {16, 0, 0, 0}}, {FMT_Y8_VIDEO, 1, 1, 1, {8, 0, 0, 0}}, {FMT_R8, 1, 1, 1, {8, 0, 0, 0}},
    {FMT_G4R4, 1, 1, 1, {4, 4, 0, 0}}, {FMT_E5B9G9R9, 4, 1, 1, {0}}, {FMT_BF10GF11RF11, 4, 1, 1, {0}},
    {FMT_DXT1, 8, 4, 4, {0}}, {FMT_DXT23, 16, 4, 4, {0}}, {FMT_DXT45, 16, 4, 4, {0}}, {FMT_DXN1, 8, 4, 4, {0}},
    {FMT_DXN2, 16, 4, 4, {0}}, {FMT_BC7U, 16, 4, 4, {0}},
    {FMT_Z24S8, 4, 1, 1, {0}}, {FMT_X8Z24, 4, 1, 1, {0}}, {FMT_S8Z24, 4, 1, 1, {0}}, {FMT_ZF32, 4, 1, 1, {0}},
    {FMT_ZF32_X24S8, 8, 1, 1, {0}}, {FMT_Z16, 2, 1, 1, {0}},
    /* ASTC: 16-byte blocks of various footprints. */
    {0x40, 16, 4, 4, {0}}, {0x50, 16, 5, 4, {0}}, {0x41, 16, 5, 5, {0}}, {0x51, 16, 6, 5, {0}},
    {0x42, 16, 6, 6, {0}}, {0x55, 16, 8, 5, {0}}, {0x52, 16, 8, 6, {0}}, {0x44, 16, 8, 8, {0}},
    {0x56, 16, 10, 5, {0}}, {0x57, 16, 10, 6, {0}}, {0x53, 16, 10, 8, {0}}, {0x45, 16, 10, 10, {0}},
    {0x54, 16, 12, 10, {0}}, {0x46, 16, 12, 12, {0}},
};

static const Format_Info *format_info(uint32_t format) {
  for (uint32_t i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); i++)
    if (k_formats[i].format == format) return &k_formats[i];
  return NULL;
}

bool tex_format_info(uint32_t format, uint32_t *bytes_per_element, uint32_t *block_width, uint32_t *block_height) {
  const Format_Info *f = format_info(format);
  if (!f) return false;
  *bytes_per_element = f->bytes;
  *block_width = f->bw;
  *block_height = f->bh;
  return true;
}

static uint32_t max1(uint32_t v) { return v ? v : 1u; }

static uint64_t bl_level_bytes(const Tex_Header *h, const Format_Info *f, uint32_t level) {
  const uint32_t w = max1(h->width >> level), ht = max1(h->height >> level);
  const uint32_t cols = (w + f->bw - 1u) / f->bw, rows = (ht + f->bh - 1u) / f->bh;
  uint32_t bh = h->block_height_log2;
  const uint32_t gob_rows = (rows + BLOCK_LINEAR_GOB_HEIGHT - 1u) / BLOCK_LINEAR_GOB_HEIGHT;
  while (bh > 0 && (1u << (bh - 1u)) >= gob_rows) bh--;
  return block_linear_size(cols * f->bytes, rows, bh);
}

uint64_t tex_level0_bytes(const Tex_Header *h) {
  const Format_Info *f = format_info(h->format);
  if (!f) return 0;
  const uint32_t cols = (h->width + f->bw - 1u) / f->bw, rows = (h->height + f->bh - 1u) / f->bh;
  switch (h->layout) {
  case TEX_LAYOUT_BLOCK_LINEAR: return bl_level_bytes(h, f, 0);
  case TEX_LAYOUT_PITCH: return (uint64_t)h->pitch * rows;
  default: return (uint64_t)cols * f->bytes;
  }
}

uint64_t tex_layer_stride(const Tex_Header *h) {
  const Format_Info *f = format_info(h->format);
  if (!f) return 0;
  if (h->layout != TEX_LAYOUT_BLOCK_LINEAR) return tex_level0_bytes(h);
  uint64_t total = 0;
  for (uint32_t level = 0; level < h->levels && level < 16u; level++) total += bl_level_bytes(h, f, level);
  const uint64_t align = (uint64_t)BLOCK_LINEAR_GOB_BYTES << h->block_height_log2;
  return (total + align - 1u) / align * align;
}

static uint32_t layer_count(const Tex_Header *h) {
  switch (h->type) {
  case TEX_TYPE_1D_ARRAY:
  case TEX_TYPE_2D_ARRAY:
  case TEX_TYPE_CUBE:
  case TEX_TYPE_CUBE_ARRAY:
    return h->depth;
  default:
    return 1;
  }
}

uint64_t tex_read_bytes(const Tex_Header *h) {
  const uint32_t layers = layer_count(h);
  return tex_layer_stride(h) * (layers - 1u) + tex_level0_bytes(h);
}

/* 8-bit UNORM formats sampled through an RGBA8 copy. */
static bool expands_to_rgba8(const Tex_Header *h) {
  uint32_t comps;
  switch (h->format) {
  case FMT_R8: comps = 1; break;
  case FMT_G8R8: comps = 2; break;
  case FMT_A8B8G8R8: comps = 4; break;
  case FMT_X8B8G8R8: comps = 3; break;
  default: return false;
  }
  if (h->srgb) return false;
  for (uint32_t c = 0; c < comps; c++)
    if (h->data_type[c] != TEX_DATA_UNORM) return false;
  return true;
}

uint64_t tex_decoded_bytes(const Tex_Header *h) {
  const Format_Info *f = format_info(h->format);
  if (!f) return 0;
  const uint32_t layers = layer_count(h);
  const uint32_t cols = (h->width + f->bw - 1u) / f->bw, rows = (h->height + f->bh - 1u) / f->bh;
  const uint64_t linear = (uint64_t)cols * f->bytes * rows * layers;
  if (f->bw == 1u && !expands_to_rgba8(h)) return linear;
  /* Expanded RGBA8 plus the linear compressed staging. */
  return (uint64_t)h->width * h->height * 4u * layers + linear;
}

/* ---- BCn ---------------------------------------------------------- */

static void rgb565(uint32_t v, uint8_t out[3]) {
  const uint32_t r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
  out[0] = (uint8_t)((r << 3) | (r >> 2));
  out[1] = (uint8_t)((g << 2) | (g >> 4));
  out[2] = (uint8_t)((b << 3) | (b >> 2));
}

/* BC1 colour block -> 16 RGBA texels. */
static void bc1_colors(const uint8_t *block, bool force_four, uint8_t out[][4]) {
  const uint32_t c0 = block[0] | (block[1] << 8), c1 = block[2] | (block[3] << 8);
  uint8_t pal[4][4];
  rgb565(c0, pal[0]);
  rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (c0 > c1 || force_four) {
    for (uint32_t c = 0; c < 3; c++) {
      pal[2][c] = (uint8_t)((2u * pal[0][c] + pal[1][c]) / 3u);
      pal[3][c] = (uint8_t)((pal[0][c] + 2u * pal[1][c]) / 3u);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (uint32_t c = 0; c < 3; c++) {
      pal[2][c] = (uint8_t)((pal[0][c] + pal[1][c]) / 2u);
      pal[3][c] = 0;
    }
    pal[2][3] = 255;
    pal[3][3] = 0;
  }
  const uint32_t bits = block[4] | (block[5] << 8) | (block[6] << 16) | ((uint32_t)block[7] << 24);
  for (uint32_t i = 0; i < 16u; i++) memcpy(out[i], pal[(bits >> (2u * i)) & 3u], 4);
}

/* BC4-style 8-byte alpha/red block -> 16 values. */
static void bc4_values(const uint8_t *block, uint8_t out[16]) {
  const uint32_t a0 = block[0], a1 = block[1];
  uint32_t pal[8];
  pal[0] = a0;
  pal[1] = a1;
  if (a0 > a1) {
    for (uint32_t i = 1; i < 7u; i++) pal[i + 1u] = ((7u - i) * a0 + i * a1) / 7u;
  } else {
    for (uint32_t i = 1; i < 5u; i++) pal[i + 1u] = ((5u - i) * a0 + i * a1) / 5u;
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t bits = 0;
  for (uint32_t i = 0; i < 6u; i++) bits |= (uint64_t)block[2u + i] << (8u * i);
  for (uint32_t i = 0; i < 16u; i++) out[i] = (uint8_t)pal[(bits >> (3u * i)) & 7u];
}

static void decode_bc_block(uint32_t format, const uint8_t *block, uint8_t out[][4]) {
  uint8_t v[16];
  switch (format) {
  case FMT_DXT1:
    bc1_colors(block, false, out);
    break;
  case FMT_DXT23:
    bc1_colors(block + 8, true, out);
    for (uint32_t i = 0; i < 16u; i++) {
      const uint32_t a = (block[i / 2u] >> ((i % 2u) * 4u)) & 0xfu;
      out[i][3] = (uint8_t)(a * 17u);
    }
    break;
  case FMT_DXT45:
    bc1_colors(block + 8, true, out);
    bc4_values(block, v);
    for (uint32_t i = 0; i < 16u; i++) out[i][3] = v[i];
    break;
  case FMT_DXN1:
    bc4_values(block, v);
    for (uint32_t i = 0; i < 16u; i++) {
      out[i][0] = v[i];
      out[i][1] = out[i][2] = 0;
      out[i][3] = 255;
    }
    break;
  case FMT_DXN2:
    bc4_values(block, v);
    for (uint32_t i = 0; i < 16u; i++) out[i][0] = v[i];
    bc4_values(block + 8, v);
    for (uint32_t i = 0; i < 16u; i++) {
      out[i][1] = v[i];
      out[i][2] = 0;
      out[i][3] = 255;
    }
    break;
  case FMT_BC7U:
    bc7_decode_block(block, out);
    break;
  default: /* BC6H and others: unsupported - magenta marks it */
    for (uint32_t i = 0; i < 16u; i++) {
      out[i][0] = 255;
      out[i][1] = 0;
      out[i][2] = 255;
      out[i][3] = 255;
    }
    break;
  }
}

bool tex_decode(const Tex_Header *h, const uint8_t *raw, uint8_t *dst, Tex_Image *out) {
  memset(out, 0, sizeof(*out));
  const Format_Info *f = format_info(h->format);
  if (!f) return false;
  const uint32_t layers = layer_count(h);
  const uint32_t cols = (h->width + f->bw - 1u) / f->bw, rows = (h->height + f->bh - 1u) / f->bh;
  const uint32_t row_bytes = cols * f->bytes;
  const uint64_t linear_layer = (uint64_t)row_bytes * rows;
  const uint64_t stride = tex_layer_stride(h);
  const bool expand = expands_to_rgba8(h);
  uint8_t *linear = (f->bw == 1u && !expand) ? dst : dst + (uint64_t)h->width * h->height * 4u * layers;
  for (uint32_t layer = 0; layer < layers; layer++) {
    const uint8_t *src = raw + stride * layer;
    uint8_t *to = linear + linear_layer * layer;
    switch (h->layout) {
    case TEX_LAYOUT_BLOCK_LINEAR:
      block_linear_to_pitch(src, to, row_bytes, row_bytes, rows, h->block_height_log2);
      break;
    case TEX_LAYOUT_PITCH:
      for (uint32_t y = 0; y < rows; y++) memcpy(to + (uint64_t)y * row_bytes, src + (uint64_t)y * h->pitch, row_bytes);
      break;
    default:
      memcpy(to, src, row_bytes);
      break;
    }
  }
  out->header = *h;
  out->width = h->width;
  out->height = h->height;
  out->layers = layers;
  if (expand) {
    const uint32_t bpp = f->bytes;
    for (uint32_t layer = 0; layer < layers; layer++) {
      const uint8_t *from = linear + linear_layer * layer;
      uint8_t *to = dst + (uint64_t)h->width * h->height * 4u * layer;
      for (uint64_t i = 0; i < (uint64_t)h->width * h->height; i++) {
        const uint8_t *p = from + i * bpp;
        uint8_t *q = to + i * 4u;
        q[0] = p[0];
        q[1] = bpp > 1u ? p[1] : 0u;
        q[2] = bpp > 2u ? p[2] : 0u;
        q[3] = (bpp > 3u && h->format != FMT_X8B8G8R8) ? p[3] : 255u;
      }
    }
    out->format = FMT_A8B8G8R8;
    out->bytes_per_texel = 4;
    out->row_bytes = h->width * 4u;
    out->layer_bytes = (uint64_t)h->width * h->height * 4u;
    out->texels = dst;
    out->rgba8 = true;
    out->valid = true;
    return true;
  }
  if (f->bw == 1u) {
    out->format = h->format;
    out->bytes_per_texel = f->bytes;
    out->row_bytes = row_bytes;
    out->layer_bytes = linear_layer;
    out->texels = dst;
    out->valid = true;
    return true;
  }
  /* Expand blocks to A8B8G8R8. */
  const uint32_t out_row = h->width * 4u;
  const bool astc = f->bytes == ASTC_BLOCK_BYTES && h->format >= 0x40u;
  for (uint32_t layer = 0; layer < layers; layer++) {
    const uint8_t *blocks = linear + linear_layer * layer;
    uint8_t *texels = dst + (uint64_t)out_row * h->height * layer;
    for (uint32_t by = 0; by < rows; by++) {
      for (uint32_t bx = 0; bx < cols; bx++) {
        uint8_t px[ASTC_MAX_FOOTPRINT * ASTC_MAX_FOOTPRINT][4];
        const uint8_t *block = blocks + (uint64_t)by * row_bytes + (uint64_t)bx * f->bytes;
        if (astc) (void)astc_decode_block(block, f->bw, f->bh, h->srgb, px);
        else decode_bc_block(h->format, block, px);
        for (uint32_t i = 0; i < (uint32_t)f->bw * f->bh; i++) {
          const uint32_t x = bx * f->bw + i % f->bw, y = by * f->bh + i / f->bw;
          if (x >= h->width || y >= h->height) continue;
          memcpy(texels + (uint64_t)y * out_row + (uint64_t)x * 4u, px[i], 4);
        }
      }
    }
  }
  out->format = FMT_A8B8G8R8;
  out->header.data_type[0] = out->header.data_type[1] = out->header.data_type[2] = out->header.data_type[3] = TEX_DATA_UNORM;
  out->bytes_per_texel = 4;
  out->row_bytes = out_row;
  out->layer_bytes = (uint64_t)out_row * h->height;
  out->texels = dst;
  out->rgba8 = !h->srgb;
  out->valid = true;
  return true;
}

/* ---- texel decode ------------------------------------------------- */

static float half_to_float(uint32_t h) {
  const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, mant = h & 0x3ffu;
  float v;
  if (exp == 0) v = ldexpf((float)mant, -24);
  else if (exp == 31) v = mant ? NAN : INFINITY;
  else v = ldexpf((float)(mant | 0x400u), (int)exp - 25);
  return sign ? -v : v;
}

static float small_float(uint32_t v, uint32_t mant_bits) {
  const uint32_t exp = v >> mant_bits, mant = v & ((1u << mant_bits) - 1u);
  if (exp == 0) return ldexpf((float)mant, -14 - (int)mant_bits);
  if (exp == 31) return mant ? NAN : INFINITY;
  return ldexpf((float)(mant | (1u << mant_bits)), (int)exp - 15 - (int)mant_bits);
}

static float srgb_to_linear(float c) {
  if (c <= 0.04045f) return c / 12.92f;
  return powf((c + 0.055f) / 1.055f, 2.4f);
}

static uint32_t extract_bits(const uint8_t *texel, uint32_t bit, uint32_t n) {
  uint64_t v = 0;
  const uint32_t first = bit / 8u, last = (bit + n + 7u) / 8u;
  for (uint32_t i = first; i < last && i - first < 8u; i++) v |= (uint64_t)texel[i] << (8u * (i - first));
  v >>= bit % 8u;
  return n >= 32u ? (uint32_t)v : (uint32_t)(v & ((1ull << n) - 1ull));
}

static uint32_t convert_component(uint32_t v, uint32_t bits, uint32_t type) {
  switch (type) {
  case TEX_DATA_SNORM:
  case TEX_DATA_SNORM_FORCE_FP16: {
    const int32_t s = bits >= 32u ? (int32_t)v : (int32_t)(v << (32u - bits)) >> (32u - bits);
    const float m = (float)((1ull << (bits - 1u)) - 1u);
    float f = (float)s / m;
    return u32f(f < -1.0f ? -1.0f : f);
  }
  case TEX_DATA_SINT:
    return bits >= 32u ? v : (uint32_t)((int32_t)(v << (32u - bits)) >> (32u - bits));
  case TEX_DATA_UINT:
    return v;
  case TEX_DATA_FLOAT:
    if (bits == 32u) return v;
    if (bits == 16u) return u32f(half_to_float(v));
    return 0;
  default: /* UNORM */
    return u32f(bits >= 32u ? (float)((double)v / 4294967295.0) : (float)v / (float)((1u << bits) - 1u));
  }
}

void tex_decode_texel(uint32_t format, const uint8_t data_type[4], bool srgb, const uint8_t *texel, uint32_t out[4]) {
  out[0] = out[1] = out[2] = 0;
  out[3] = u32f(1.0f);
  switch (format) {
  case FMT_BF10GF11RF11: {
    const uint32_t v = extract_bits(texel, 0, 32);
    out[0] = u32f(small_float(v & 0x7ffu, 6));
    out[1] = u32f(small_float((v >> 11) & 0x7ffu, 6));
    out[2] = u32f(small_float((v >> 22) & 0x3ffu, 5));
    return;
  }
  case FMT_E5B9G9R9: {
    const uint32_t v = extract_bits(texel, 0, 32);
    const float scale = ldexpf(1.0f, (int)(v >> 27) - 15 - 9);
    out[0] = u32f((float)(v & 0x1ffu) * scale);
    out[1] = u32f((float)((v >> 9) & 0x1ffu) * scale);
    out[2] = u32f((float)((v >> 18) & 0x1ffu) * scale);
    return;
  }
  case FMT_Z24S8:
    out[0] = u32f((float)(extract_bits(texel, 8, 24)) / 16777215.0f);
    out[1] = extract_bits(texel, 0, 8);
    return;
  case FMT_S8Z24:
  case FMT_X8Z24:
    out[0] = u32f((float)(extract_bits(texel, 0, 24)) / 16777215.0f);
    out[1] = extract_bits(texel, 24, 8);
    return;
  case FMT_ZF32:
  case FMT_ZF32_X24S8:
    out[0] = extract_bits(texel, 0, 32);
    return;
  case FMT_Z16:
    out[0] = u32f((float)extract_bits(texel, 0, 16) / 65535.0f);
    return;
  default:
    break;
  }
  const Format_Info *f = format_info(format);
  if (!f || f->bw != 1u) {
    out[0] = out[2] = u32f(1.0f); /* magenta: unsupported */
    return;
  }
  uint32_t bit = 0;
  for (uint32_t c = 0; c < 4; c++) {
    const uint32_t n = f->bits[c];
    if (n == 0) continue;
    out[c] = convert_component(extract_bits(texel, bit, n), n, data_type[c]);
    bit += n;
  }
  if (format == FMT_X8B8G8R8) out[3] = u32f(1.0f);
  if (srgb) {
    for (uint32_t c = 0; c < 3; c++)
      if (data_type[c] == TEX_DATA_UNORM) out[c] = u32f(srgb_to_linear(f32(out[c])));
  }
}

/* ---- sampling ----------------------------------------------------- */

#define WRAP_REPEAT 0u
#define WRAP_MIRROR 1u
#define WRAP_CLAMP_TO_EDGE 2u
#define WRAP_BORDER 3u
#define WRAP_CLAMP_OGL 4u
#define WRAP_MIRROR_ONCE_CLAMP_TO_EDGE 5u
#define WRAP_MIRROR_ONCE_BORDER 6u
#define WRAP_MIRROR_ONCE_CLAMP_OGL 7u

/* Returns -1 for "border". */
static int32_t wrap_index(int32_t i, int32_t size, uint32_t mode) {
  switch (mode) {
  case WRAP_REPEAT: {
    int32_t m = i % size;
    return m < 0 ? m + size : m;
  }
  case WRAP_MIRROR: {
    const int32_t period = size * 2;
    int32_t m = i % period;
    if (m < 0) m += period;
    return m < size ? m : period - 1 - m;
  }
  case WRAP_BORDER:
    return (i < 0 || i >= size) ? -1 : i;
  case WRAP_MIRROR_ONCE_BORDER: {
    const int32_t m = i < 0 ? -1 - i : i;
    return m >= size ? -1 : m;
  }
  case WRAP_MIRROR_ONCE_CLAMP_TO_EDGE:
  case WRAP_MIRROR_ONCE_CLAMP_OGL: {
    const int32_t m = i < 0 ? -1 - i : i;
    return m >= size ? size - 1 : m;
  }
  default: /* clamp to edge / clamp (OGL) */
    return i < 0 ? 0 : (i >= size ? size - 1 : i);
  }
}

static bool is_integer_type(uint32_t type) { return type == TEX_DATA_SINT || type == TEX_DATA_UINT; }

/* One texel (unswizzled), honouring the border. */
/* byte -> [0,1] for RGBA8 texels */
static float g_unorm8_table[256];
static bool g_tables_ready;

void tex_init_tables(void) {
  if (g_tables_ready) return;
  for (uint32_t i = 0; i < 256u; i++) g_unorm8_table[i] = (float)i / 255.0f;
  g_tables_ready = true;
}

static const float *unorm8_table(void) {
  tex_init_tables(); /* raster3d_init runs this before samplers go parallel */
  return g_unorm8_table;
}

static void texel_at(const Tex_Image *img, const Tex_Sampler *s, int32_t x, int32_t y, uint32_t layer, uint32_t out[4]) {
  const int32_t wx = wrap_index(x, (int32_t)img->width, s ? s->wrap[0] : WRAP_CLAMP_TO_EDGE);
  const int32_t wy = wrap_index(y, (int32_t)img->height, s ? s->wrap[1] : WRAP_CLAMP_TO_EDGE);
  if (wx < 0 || wy < 0) {
    for (uint32_t c = 0; c < 4; c++) out[c] = u32f(s ? s->border[c] : 0.0f);
    return;
  }
  if (layer >= img->layers) layer = img->layers - 1u;
  const uint8_t *p = img->texels + img->layer_bytes * layer + (uint64_t)wy * img->row_bytes +
                     (uint64_t)wx * img->bytes_per_texel;
  if (img->rgba8) {
    const float *table = unorm8_table();
    for (uint32_t c = 0; c < 4u; c++) out[c] = u32f(table[p[c]]);
    return;
  }
  tex_decode_texel(img->format, img->header.data_type, img->header.srgb && img->format == img->header.format, p, out);
  if (img->header.srgb && img->format != img->header.format) { /* expanded BCn */
    for (uint32_t c = 0; c < 3; c++) out[c] = u32f(srgb_to_linear(f32(out[c])));
  }
}

static uint32_t swizzle_one(const uint32_t texel[4], uint32_t source) {
  switch (source) {
  case TEX_SOURCE_R: return texel[0];
  case TEX_SOURCE_G: return texel[1];
  case TEX_SOURCE_B: return texel[2];
  case TEX_SOURCE_A: return texel[3];
  case TEX_SOURCE_ONE_INT: return 1u;
  case TEX_SOURCE_ONE_FLOAT: return u32f(1.0f);
  default: return 0;
  }
}

static void apply_swizzle(const Tex_Image *img, const uint32_t texel[4], uint32_t out[4]) {
  const uint8_t *sw = img->header.swizzle;
  if (sw[0] == TEX_SOURCE_R && sw[1] == TEX_SOURCE_G && sw[2] == TEX_SOURCE_B && sw[3] == TEX_SOURCE_A) {
    memcpy(out, texel, 4u * sizeof(uint32_t)); /* the common identity */
    return;
  }
  for (uint32_t c = 0; c < 4; c++) out[c] = swizzle_one(texel, sw[c]);
}

static bool compare(uint32_t func, float ref, float value) {
  switch (func & 7u) {
  case 0: return false;
  case 1: return ref < value;
  case 2: return ref == value;
  case 3: return ref <= value;
  case 4: return ref > value;
  case 5: return ref != value;
  case 6: return ref >= value;
  default: return true;
  }
}

/* Cube direction -> face (layer offset) and 2D coordinates in [0,1]. */
static uint32_t cube_face(const float c[3], float *s, float *t) {
  const float ax = fabsf(c[0]), ay = fabsf(c[1]), az = fabsf(c[2]);
  float sc, tc, ma;
  uint32_t face;
  if (ax >= ay && ax >= az) {
    face = c[0] >= 0 ? 0u : 1u;
    ma = ax;
    sc = c[0] >= 0 ? -c[2] : c[2];
    tc = -c[1];
  } else if (ay >= az) {
    face = c[1] >= 0 ? 2u : 3u;
    ma = ay;
    sc = c[0];
    tc = c[1] >= 0 ? c[2] : -c[2];
  } else {
    face = c[2] >= 0 ? 4u : 5u;
    ma = az;
    sc = c[2] >= 0 ? c[0] : -c[0];
    tc = -c[1];
  }
  if (ma == 0.0f) ma = 1.0f;
  *s = 0.5f * (sc / ma + 1.0f);
  *t = 0.5f * (tc / ma + 1.0f);
  return face;
}

static void resolve_coords(const Tex_Image *img, const float coords[3], float layer, float *u, float *v, uint32_t *lay) {
  float s = coords[0], t = img->height > 1u ? coords[1] : 0.0f;
  uint32_t l = layer < 0.0f ? 0u : (uint32_t)(layer + 0.5f);
  if (img->header.type == TEX_TYPE_CUBE || img->header.type == TEX_TYPE_CUBE_ARRAY) {
    l = l * 6u + cube_face(coords, &s, &t);
  }
  if (img->header.normalized || img->header.type == TEX_TYPE_CUBE || img->header.type == TEX_TYPE_CUBE_ARRAY) {
    s *= (float)img->width;
    t *= (float)img->height;
  }
  *u = s;
  *v = t;
  *lay = l;
}

/* A texel coordinate as an integer, saturated well inside int32 so the
 * +1 of a bilinear tap and the wrap arithmetic cannot overflow; NaN is 0.
 * (A shader's huge or NaN coordinates once overflowed x0 + 1 past the
 * bounds check below and read far outside the texture.) */
#define TEXEL_COORD_LIMIT 1073741824.0f /* 2^30 */
static int32_t texel_coord(float f) {
  if (!(f == f)) return 0;
  if (f > TEXEL_COORD_LIMIT) return (int32_t)TEXEL_COORD_LIMIT;
  if (f < -TEXEL_COORD_LIMIT) return -(int32_t)TEXEL_COORD_LIMIT;
  return (int32_t)f;
}

void tex_sample(const Tex_Image *img, const Tex_Sampler *s, const float coords[3], float layer, float dref,
                bool shadow, const int32_t offset[3], uint32_t out[4]) {
  if (!img || !img->valid) {
    out[0] = out[1] = out[2] = 0;
    out[3] = u32f(1.0f);
    return;
  }
  float u, v;
  uint32_t lay;
  resolve_coords(img, coords, layer, &u, &v, &lay);
  const int32_t ox = offset ? offset[0] : 0, oy = offset ? offset[1] : 0;
  const bool linear = s && s->mag_filter == 2u && !is_integer_type(img->header.data_type[0]);
  const bool do_compare = shadow && s && s->depth_compare;
  uint32_t texel[4];
  if (!linear) {
    texel_at(img, s, texel_coord(floorf(u)) + ox, texel_coord(floorf(v)) + oy, lay, texel);
    if (do_compare) {
      texel[0] = u32f(compare(s->compare_func, dref, f32(texel[0])) ? 1.0f : 0.0f);
      texel[1] = texel[2] = texel[0];
    }
    apply_swizzle(img, texel, out);
    return;
  }
  const float x = u - 0.5f, y = v - 0.5f;
  const float fx = floorf(x), fy = floorf(y);
  const float ax = x - fx, ay = y - fy;
  const int32_t x0 = texel_coord(fx) + ox, y0 = texel_coord(fy) + oy;
  if (img->rgba8 && !do_compare && x0 >= 0 && y0 >= 0 && x0 + 1 < (int32_t)img->width &&
      y0 + 1 < (int32_t)img->height && lay < img->layers) {
    /* All four taps inside an RGBA8 image: no wrapping, direct reads
     * (the same arithmetic as the general path below). */
    const float *table = unorm8_table();
    const uint8_t *p00 = img->texels + img->layer_bytes * lay + (uint64_t)y0 * img->row_bytes + (uint64_t)x0 * 4u;
    const uint8_t *p01 = p00 + img->row_bytes;
    for (uint32_t c = 0; c < 4; c++) {
      const float a = table[p00[c]], b = table[p00[4u + c]], cc = table[p01[c]], d = table[p01[4u + c]];
      const float top = a + (b - a) * ax, bottom = cc + (d - cc) * ax;
      texel[c] = u32f(top + (bottom - top) * ay);
    }
    apply_swizzle(img, texel, out);
    return;
  }
  uint32_t t00[4], t10[4], t01[4], t11[4];
  texel_at(img, s, x0, y0, lay, t00);
  texel_at(img, s, x0 + 1, y0, lay, t10);
  texel_at(img, s, x0, y0 + 1, lay, t01);
  texel_at(img, s, x0 + 1, y0 + 1, lay, t11);
  if (img->height <= 1u) {
    memcpy(t01, t00, sizeof(t01));
    memcpy(t11, t10, sizeof(t11));
  }
  for (uint32_t c = 0; c < 4; c++) {
    float a = f32(t00[c]), b = f32(t10[c]), cc = f32(t01[c]), d = f32(t11[c]);
    if (do_compare) {
      a = compare(s->compare_func, dref, f32(t00[0])) ? 1.0f : 0.0f;
      b = compare(s->compare_func, dref, f32(t10[0])) ? 1.0f : 0.0f;
      cc = compare(s->compare_func, dref, f32(t01[0])) ? 1.0f : 0.0f;
      d = compare(s->compare_func, dref, f32(t11[0])) ? 1.0f : 0.0f;
    }
    const float top = a + (b - a) * ax, bottom = cc + (d - cc) * ax;
    texel[c] = u32f(top + (bottom - top) * ay);
  }
  apply_swizzle(img, texel, out);
}

void tex_sample_batch(const Tex_Image *img, const Tex_Sampler *s, const float *u, const float *v, uint32_t count,
                      uint32_t (*out)[4]) {
  const bool fast = img && img->valid && img->rgba8 && s && s->mag_filter == 2u &&
                    !is_integer_type(img->header.data_type[0]) && img->height > 1u && img->layers > 0u &&
                    img->header.type != TEX_TYPE_CUBE && img->header.type != TEX_TYPE_CUBE_ARRAY;
  if (!fast) {
    for (uint32_t i = 0; i < count; i++) {
      const float coords[3] = {u[i], v[i], 0.0f};
      tex_sample(img, s, coords, 0.0f, 0.0f, false, NULL, out[i]);
    }
    return;
  }
  /* tex_sample's RGBA8 bilinear interior path, hoisted; any other texel
   * (an edge, the border) takes tex_sample itself. */
  const float *table = unorm8_table();
  const float sx = img->header.normalized ? (float)img->width : 1.0f, sy = img->header.normalized ? (float)img->height : 1.0f;
  const bool scale = img->header.normalized;
  const int32_t max_x = (int32_t)img->width - 1, max_y = (int32_t)img->height - 1;
  const uint8_t *texels = img->texels;
  const uint64_t row_bytes = img->row_bytes;
  const uint8_t *sw = img->header.swizzle;
  const bool identity = sw[0] == TEX_SOURCE_R && sw[1] == TEX_SOURCE_G && sw[2] == TEX_SOURCE_B && sw[3] == TEX_SOURCE_A;
  for (uint32_t i = 0; i < count; i++) {
    const float x = (scale ? u[i] * sx : u[i]) - 0.5f, y = (scale ? v[i] * sy : v[i]) - 0.5f;
    const float fx = floorf(x), fy = floorf(y);
    if (!(fx >= 0.0f && fy >= 0.0f && fx < (float)max_x && fy < (float)max_y)) { /* also NaN */
      const float coords[3] = {u[i], v[i], 0.0f};
      tex_sample(img, s, coords, 0.0f, 0.0f, false, NULL, out[i]);
      continue;
    }
    const int32_t x0 = (int32_t)fx, y0 = (int32_t)fy;
    const float ax = x - fx, ay = y - fy;
    const uint8_t *p00 = texels + (uint64_t)y0 * row_bytes + (uint64_t)x0 * 4u;
    const uint8_t *p01 = p00 + row_bytes;
    uint32_t texel[4];
    for (uint32_t c = 0; c < 4; c++) {
      const float a = table[p00[c]], b = table[p00[4u + c]], cc = table[p01[c]], d = table[p01[4u + c]];
      const float top = a + (b - a) * ax, bottom = cc + (d - cc) * ax;
      texel[c] = u32f(top + (bottom - top) * ay);
    }
    if (identity) memcpy(out[i], texel, sizeof(texel));
    else apply_swizzle(img, texel, out[i]);
  }
}

void tex_gather(const Tex_Image *img, const Tex_Sampler *s, const float coords[3], float layer, uint32_t component,
                float dref, bool shadow, const int32_t offset[3], uint32_t out[4]) {
  if (!img || !img->valid) {
    out[0] = out[1] = out[2] = out[3] = 0;
    return;
  }
  float u, v;
  uint32_t lay;
  resolve_coords(img, coords, layer, &u, &v, &lay);
  const int32_t x0 = texel_coord(floorf(u - 0.5f)) + (offset ? offset[0] : 0);
  const int32_t y0 = texel_coord(floorf(v - 0.5f)) + (offset ? offset[1] : 0);
  /* GL order: (x0,y1), (x1,y1), (x1,y0), (x0,y0). */
  const int32_t xs[4] = {x0, x0 + 1, x0 + 1, x0};
  const int32_t ys[4] = {y0 + 1, y0 + 1, y0, y0};
  for (uint32_t i = 0; i < 4; i++) {
    uint32_t texel[4], sw[4];
    texel_at(img, s, xs[i], ys[i], lay, texel);
    apply_swizzle(img, texel, sw);
    out[i] = sw[component & 3u];
    if (shadow && s && s->depth_compare) out[i] = u32f(compare(s->compare_func, dref, f32(texel[0])) ? 1.0f : 0.0f);
  }
}

void tex_texel(const Tex_Image *img, uint32_t x, uint32_t y, uint32_t layer, uint32_t out[4]) {
  texel_at(img, NULL, (int32_t)x, (int32_t)y, layer, out);
}

void tex_fetch(const Tex_Image *img, const int32_t coords[3], int32_t layer, uint32_t out[4]) {
  if (!img || !img->valid || coords[0] < 0 || coords[1] < 0 || (uint32_t)coords[0] >= img->width ||
      (uint32_t)coords[1] >= img->height) {
    out[0] = out[1] = out[2] = out[3] = 0;
    return;
  }
  uint32_t texel[4];
  texel_at(img, NULL, coords[0], coords[1], layer < 0 ? 0u : (uint32_t)layer, texel);
  apply_swizzle(img, texel, out);
}
