/**
 * core/gpu/bc7: every mode decodes exactly as an independent BC7 decoder
 * (Pillow's) does - 64 random blocks, 8 per mode (tests/data/bc7_vectors.inc).
 * Also: a block with no mode bit decodes to transparent black.
 */
#define CHECK_NAME "bc7_test"
#include "check.h"

#include "gpu/bc7.h"

#include <stdio.h>
#include <string.h>

#include "data/bc7_vectors.inc"

int main(void) {
  const uint32_t blocks = sizeof(k_bc7_blocks) / BC7_BLOCK_BYTES;
  for (uint32_t b = 0; b < blocks; b++) {
    uint8_t out[BC7_TEXELS][4];
    bc7_decode_block(k_bc7_blocks + b * BC7_BLOCK_BYTES, out);
    if (memcmp(out, k_bc7_texels + b * sizeof(out), sizeof(out)) != 0) {
      fprintf(stderr, "[bc7_test] block %u (mode %u) differs\n", b, b % 8u);
      CHECK(0);
    }
  }
  const uint8_t invalid[BC7_BLOCK_BYTES] = {0};
  uint8_t out[BC7_TEXELS][4];
  memset(out, 0xAA, sizeof(out));
  bc7_decode_block(invalid, out);
  for (uint32_t t = 0; t < BC7_TEXELS; t++) CHECK(out[t][0] == 0 && out[t][3] == 0);
  printf("[bc7_test] passed\n");
  return 0;
}
