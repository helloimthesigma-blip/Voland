/**
 * BC1 / BC3 encoder (gpu/bc_encode): blocks decoded back by the format's
 * rules (565 bit replication, 4-colour and 8-alpha palettes) - a solid
 * colour, two colours, gradients along each channel and against each
 * other, and alpha ramps stay within the formats' quantisation error.
 */
#include <stdio.h>
#include <stdlib.h>

#include "gpu/bc_encode.h"

static int g_failures;

#define CHECK(cond, ...)                \
  do {                                  \
    if (!(cond)) {                      \
      fprintf(stderr, "FAIL: ");        \
      fprintf(stderr, __VA_ARGS__);     \
      fprintf(stderr, "\n");            \
      g_failures++;                     \
    }                                   \
  } while (0)

/* Largest error allowed per channel: the 565 step (8 levels) plus half
 * the gap between palette colours (a sixth of the block's widest range:
 * four colours along it). Alpha: half of its seven gaps, plus rounding. */
#define QUANT_ERROR 8
#define PALETTE_GAPS 6
#define ALPHA_TOLERANCE 20

static void expand565(uint16_t v, int c[3]) {
  const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  c[0] = (r << 3) | (r >> 2);
  c[1] = (g << 2) | (g >> 4);
  c[2] = (b << 3) | (b >> 2);
}

static void decode_color(const uint8_t *in, int out[16][3]) {
  const uint16_t c0 = (uint16_t)(in[0] | in[1] << 8), c1 = (uint16_t)(in[2] | in[3] << 8);
  int pal[4][3];
  expand565(c0, pal[0]);
  expand565(c1, pal[1]);
  for (int c = 0; c < 3; c++) {
    pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
    pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
  }
  const uint32_t idx = (uint32_t)in[4] | (uint32_t)in[5] << 8 | (uint32_t)in[6] << 16 | (uint32_t)in[7] << 24;
  for (int i = 0; i < 16; i++)
    for (int c = 0; c < 3; c++) out[i][c] = pal[(idx >> (2 * i)) & 3u][c];
}

static void decode_alpha(const uint8_t *in, int out[16]) {
  int pal[8];
  pal[0] = in[0];
  pal[1] = in[1];
  for (int k = 1; k < 7; k++) pal[k + 1] = in[0] > in[1] ? ((7 - k) * in[0] + k * in[1]) / 7 : in[0];
  uint64_t idx = 0;
  for (int b = 0; b < 6; b++) idx |= (uint64_t)in[2 + b] << (8 * b);
  for (int i = 0; i < 16; i++) out[i] = pal[(idx >> (3 * i)) & 7u];
}

static int max_color_error(const uint8_t px[16][4], const uint8_t *block) {
  int dec[16][3], worst = 0;
  decode_color(block, dec);
  for (int i = 0; i < 16; i++)
    for (int c = 0; c < 3; c++) {
      const int e = abs(dec[i][c] - px[i][c]);
      if (e > worst) worst = e;
    }
  return worst;
}

static void check_block(const char *name, const uint8_t px[16][4]) {
  int range = 0;
  for (int c = 0; c < 3; c++) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) {
      if (px[i][c] < lo) lo = px[i][c];
      if (px[i][c] > hi) hi = px[i][c];
    }
    if (hi - lo > range) range = hi - lo;
  }
  const int tolerance = QUANT_ERROR + range / PALETTE_GAPS;
  uint8_t b1[BC1_BLOCK_BYTES], b3[BC3_BLOCK_BYTES];
  bc1_encode_block(px, b1);
  bc3_encode_block(px, b3);
  const uint16_t c0 = (uint16_t)(b1[0] | b1[1] << 8), c1 = (uint16_t)(b1[2] | b1[3] << 8);
  CHECK(c0 >= c1, "%s: BC1 endpoints in 4-colour order (%04x %04x)", name, c0, c1);
  const int e1 = max_color_error(px, b1), e3 = max_color_error(px, b3 + 8);
  CHECK(e1 <= tolerance, "%s: BC1 colour error %d", name, e1);
  CHECK(e3 <= tolerance, "%s: BC3 colour error %d", name, e3);
  int alpha[16];
  decode_alpha(b3, alpha);
  for (int i = 0; i < 16; i++) CHECK(abs(alpha[i] - px[i][3]) <= ALPHA_TOLERANCE, "%s: alpha %d decodes to %d", name, px[i][3], alpha[i]);
}

int main(void) {
  uint8_t px[16][4];
  for (int i = 0; i < 16; i++) { px[i][0] = 200; px[i][1] = 100; px[i][2] = 50; px[i][3] = 255; }
  check_block("solid", px);
  for (int i = 0; i < 16; i++) { const int on = i % 2; px[i][0] = on ? 240 : 10; px[i][1] = on ? 10 : 240; px[i][2] = 128; px[i][3] = on ? 0 : 255; }
  check_block("two colours, opposite channels", px);
  for (int i = 0; i < 16; i++) { px[i][0] = (uint8_t)(i * 16); px[i][1] = (uint8_t)(255 - i * 16); px[i][2] = (uint8_t)(i * 8); px[i][3] = (uint8_t)(i * 17); }
  check_block("gradient, green falling", px);
  for (int i = 0; i < 16; i++) { px[i][0] = 30; px[i][1] = (uint8_t)(60 + i * 8); px[i][2] = (uint8_t)(60 + i * 8); px[i][3] = 128; }
  check_block("grey-green ramp", px);
  if (g_failures) {
    fprintf(stderr, "bc_encode_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("bc_encode_test: ok\n");
  return 0;
}
