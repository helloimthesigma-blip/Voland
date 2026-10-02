/**
 * ASTC LDR decoder (gpu/astc): hand-built blocks with known decodings -
 * a void-extent block, an HDR void-extent and a reserved block mode
 * (error colour), and a single-partition 4x4 block (RGB direct endpoints,
 * a 4x4 grid of 2-bit weights) whose every texel is computed from the
 * spec's unquantization and interpolation rules.
 */
#include <stdio.h>
#include <string.h>

#include "gpu/astc.h"

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

static void set_bits(uint8_t *block, uint32_t start, uint32_t n, uint32_t value) {
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t at = start + i;
    if ((value >> i) & 1u) block[at / 8u] |= (uint8_t)(1u << (at % 8u));
    else block[at / 8u] &= (uint8_t)~(1u << (at % 8u));
  }
}

static bool is_error(const uint8_t texel[4]) {
  return texel[0] == 0xFF && texel[1] == 0x00 && texel[2] == 0xFF && texel[3] == 0xFF;
}

static void test_void_extent(void) {
  uint8_t block[ASTC_BLOCK_BYTES];
  memset(block, 0xFF, sizeof(block));          /* extent coordinates all ones: "no extent" */
  set_bits(block, 0, 9, 0x1FCu);               /* void-extent marker */
  set_bits(block, 9, 1, 0);                    /* LDR */
  set_bits(block, 64, 16, 0x1234u);            /* R */
  set_bits(block, 80, 16, 0xABCDu);            /* G */
  set_bits(block, 96, 16, 0x00FFu);            /* B */
  set_bits(block, 112, 16, 0xFF00u);           /* A */
  uint8_t out[6 * 6][4];
  CHECK(astc_decode_block(block, 6, 6, false, out), "LDR void extent decodes");
  for (uint32_t i = 0; i < 36u; i++)
    CHECK(out[i][0] == 0x12 && out[i][1] == 0xAB && out[i][2] == 0x00 && out[i][3] == 0xFF, "void-extent texel %u", i);
  set_bits(block, 9, 1, 1); /* HDR */
  CHECK(!astc_decode_block(block, 6, 6, false, out) && is_error(out[0]), "HDR void extent is the error colour (LDR)");
}

static void test_reserved_mode(void) {
  uint8_t block[ASTC_BLOCK_BYTES];
  memset(block, 0, sizeof(block)); /* block mode 0: reserved */
  uint8_t out[4 * 4][4];
  CHECK(!astc_decode_block(block, 4, 4, false, out), "reserved block mode is illegal");
  for (uint32_t i = 0; i < 16u; i++) CHECK(is_error(out[i]), "reserved block texel %u is magenta", i);
}

/* 4x4 footprint, 4x4 weight grid, weight range 0..3 (2 bits), one
 * partition, CEM 8 (RGB direct) with 8-bit endpoints black -> white. */
static void test_rgb_direct(void) {
  uint8_t block[ASTC_BLOCK_BYTES];
  memset(block, 0, sizeof(block));
  /* Block mode: bits[1:0]=rho2..1=10, bits[3:2]=00 (W=B+4, H=A+2),
   * bit4=rho0=0, A(6:5)=2, B(8:7)=0, P=DP=0. */
  set_bits(block, 0, 11, 0x042u);
  set_bits(block, 11, 2, 0);   /* one partition */
  set_bits(block, 13, 4, 8);   /* CEM 8 */
  /* 128 - 17 - 32 weight bits = 79 bits for 6 values: 8-bit values. */
  const uint32_t values[6] = {0, 255, 0, 255, 0, 255};
  for (uint32_t i = 0; i < 6u; i++) set_bits(block, 17u + 8u * i, 8, values[i]);
  /* Weights, stored bit-reversed from bit 127: weight k = k % 4. */
  for (uint32_t k = 0; k < 16u; k++) {
    const uint32_t w = k % 4u;
    for (uint32_t b = 0; b < 2u; b++) set_bits(block, 127u - (2u * k + b), 1, (w >> b) & 1u);
  }
  uint8_t out[16][4];
  CHECK(astc_decode_block(block, 4, 4, false, out), "RGB direct block decodes");
  /* Weights 0..3 -> 0, 21, 43, 64 -> (65535 * i + 32) / 64 >> 8. */
  static const uint8_t expect[4] = {0, 84, 171, 255};
  for (uint32_t k = 0; k < 16u; k++) {
    const uint8_t e = expect[k % 4u];
    CHECK(out[k][0] == e && out[k][1] == e && out[k][2] == e && out[k][3] == 0xFF, "texel %u = %u,%u,%u,%u (want %u)", k,
          out[k][0], out[k][1], out[k][2], out[k][3], e);
  }
  /* Same block on an 8x8 footprint: the 4x4 grid is infilled. */
  uint8_t big[64][4];
  CHECK(astc_decode_block(block, 8, 8, false, big), "grid smaller than footprint decodes");
  CHECK(big[0][0] == 0 && big[7][0] == 255, "infill keeps the corner weights (%u, %u)", big[0][0], big[7][0]);
}

int main(void) {
  test_void_extent();
  test_reserved_mode();
  test_rgb_direct();
  if (g_failures) {
    fprintf(stderr, "astc_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("astc_test: ok\n");
  return 0;
}
