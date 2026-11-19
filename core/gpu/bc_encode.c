#include "gpu/bc_encode.h"

#define CHANNELS 3
#define BYTE_MAX 255
#define R5_MAX 31
#define G6_MAX 63
#define RED_SHIFT 11
#define GREEN_SHIFT 5
#define COLOR_PALETTE 4u
#define ALPHA_PALETTE 8u
#define ALPHA_STEPS 7
#define INDEX_BITS_COLOR 2u
#define INDEX_BITS_ALPHA 3u
#define INSET_SHIFT 4 /* the box is inset by 1/16 of its extent: the endpoints sit inside the cluster */

static uint16_t pack565(const int c[CHANNELS]) {
  const int r = (c[0] * R5_MAX + BYTE_MAX / 2) / BYTE_MAX;
  const int g = (c[1] * G6_MAX + BYTE_MAX / 2) / BYTE_MAX;
  const int b = (c[2] * R5_MAX + BYTE_MAX / 2) / BYTE_MAX;
  return (uint16_t)((r << RED_SHIFT) | (g << GREEN_SHIFT) | b);
}

/* The 8-bit colour a decoder expands a 565 value to (bit replication). */
static void unpack565(uint16_t v, int c[CHANNELS]) {
  const int r = (v >> RED_SHIFT) & R5_MAX, g = (v >> GREEN_SHIFT) & G6_MAX, b = v & R5_MAX;
  c[0] = (r << 3) | (r >> 2);
  c[1] = (g << 2) | (g >> 4);
  c[2] = (b << 3) | (b >> 2);
}

static int clamp_byte(int v) { return v < 0 ? 0 : v > BYTE_MAX ? BYTE_MAX : v; }

static void encode_color(const uint8_t px[BC_BLOCK_TEXELS][4], uint8_t out[BC1_BLOCK_BYTES]) {
  int lo[CHANNELS] = {BYTE_MAX, BYTE_MAX, BYTE_MAX}, hi[CHANNELS] = {0, 0, 0}, sum[CHANNELS] = {0, 0, 0};
  for (uint32_t i = 0; i < BC_BLOCK_TEXELS; i++)
    for (int c = 0; c < CHANNELS; c++) {
      const int v = px[i][c];
      if (v < lo[c]) lo[c] = v;
      if (v > hi[c]) hi[c] = v;
      sum[c] += v;
    }
  /* The diagonal runs low-to-high in the widest channel; a channel that
   * falls as it rises runs high-to-low. */
  int widest = 0;
  for (int c = 1; c < CHANNELS; c++)
    if (hi[c] - lo[c] > hi[widest] - lo[widest]) widest = c;
  int from[CHANNELS], to[CHANNELS];
  for (int c = 0; c < CHANNELS; c++) {
    int cov = 0;
    for (uint32_t i = 0; i < BC_BLOCK_TEXELS; i++)
      cov += (px[i][widest] * (int)BC_BLOCK_TEXELS - sum[widest]) * (px[i][c] * (int)BC_BLOCK_TEXELS - sum[c]) / (int)BC_BLOCK_TEXELS;
    const int inset = (hi[c] - lo[c]) >> INSET_SHIFT;
    from[c] = cov < 0 ? hi[c] - inset : lo[c] + inset;
    to[c] = cov < 0 ? lo[c] + inset : hi[c] - inset;
  }
  uint16_t c0 = pack565(to), c1 = pack565(from);
  if (c0 < c1) {
    const uint16_t s = c0;
    c0 = c1;
    c1 = s;
  }
  uint32_t indices = 0;
  if (c0 != c1) { /* equal endpoints: every index 0 */
    int pal[COLOR_PALETTE][CHANNELS];
    unpack565(c0, pal[0]);
    unpack565(c1, pal[1]);
    for (int c = 0; c < CHANNELS; c++) {
      pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
      pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
    }
    for (uint32_t i = 0; i < BC_BLOCK_TEXELS; i++) {
      uint32_t best = 0;
      int best_d = -1;
      for (uint32_t k = 0; k < COLOR_PALETTE; k++) {
        int d = 0;
        for (int c = 0; c < CHANNELS; c++) d += (px[i][c] - pal[k][c]) * (px[i][c] - pal[k][c]);
        if (best_d < 0 || d < best_d) {
          best_d = d;
          best = k;
        }
      }
      indices |= best << (INDEX_BITS_COLOR * i);
    }
  }
  out[0] = (uint8_t)c0;
  out[1] = (uint8_t)(c0 >> 8);
  out[2] = (uint8_t)c1;
  out[3] = (uint8_t)(c1 >> 8);
  for (uint32_t b = 0; b < 4u; b++) out[4u + b] = (uint8_t)(indices >> (8u * b));
}

void bc1_encode_block(const uint8_t rgba[BC_BLOCK_TEXELS][4], uint8_t out[BC1_BLOCK_BYTES]) { encode_color(rgba, out); }

void bc3_encode_block(const uint8_t rgba[BC_BLOCK_TEXELS][4], uint8_t out[BC3_BLOCK_BYTES]) {
  int lo = BYTE_MAX, hi = 0;
  for (uint32_t i = 0; i < BC_BLOCK_TEXELS; i++) {
    if (rgba[i][3] < lo) lo = rgba[i][3];
    if (rgba[i][3] > hi) hi = rgba[i][3];
  }
  uint64_t indices = 0;
  if (hi != lo) { /* a0 > a1: the eight-value mode */
    int pal[ALPHA_PALETTE];
    pal[0] = hi;
    pal[1] = lo;
    for (int k = 1; k < ALPHA_STEPS; k++) pal[k + 1] = ((ALPHA_STEPS - k) * hi + k * lo) / ALPHA_STEPS;
    for (uint32_t i = 0; i < BC_BLOCK_TEXELS; i++) {
      uint32_t best = 0;
      int best_d = BYTE_MAX + 1;
      for (uint32_t k = 0; k < ALPHA_PALETTE; k++) {
        const int d = rgba[i][3] > pal[k] ? rgba[i][3] - pal[k] : pal[k] - rgba[i][3];
        if (d < best_d) {
          best_d = d;
          best = k;
        }
      }
      indices |= (uint64_t)best << (INDEX_BITS_ALPHA * i);
    }
  }
  out[0] = (uint8_t)clamp_byte(hi);
  out[1] = (uint8_t)clamp_byte(lo);
  for (uint32_t b = 0; b < 6u; b++) out[2u + b] = (uint8_t)(indices >> (8u * b));
  encode_color(rgba, out + BC1_BLOCK_BYTES);
}
