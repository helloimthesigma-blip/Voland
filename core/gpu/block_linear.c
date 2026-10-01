#include "gpu/block_linear.h"

#include <string.h>

#define GOB_SPAN 16u /* the contiguous run inside a GOB: x % 16 */

static uint32_t gobs_wide(uint32_t width_bytes) {
  return (width_bytes + BLOCK_LINEAR_GOB_WIDTH - 1u) / BLOCK_LINEAR_GOB_WIDTH;
}

uint64_t block_linear_size(uint32_t width_bytes, uint32_t height, uint32_t block_height_log2) {
  const uint32_t block_rows = BLOCK_LINEAR_GOB_HEIGHT << block_height_log2;
  const uint64_t blocks_high = (height + block_rows - 1u) / block_rows;
  return (uint64_t)gobs_wide(width_bytes) * blocks_high * ((uint64_t)BLOCK_LINEAR_GOB_BYTES << block_height_log2);
}

uint64_t block_linear_offset(uint32_t x, uint32_t y, uint32_t width_bytes, uint32_t block_height_log2) {
  const uint32_t gobs_per_block = 1u << block_height_log2;
  const uint32_t block_rows = BLOCK_LINEAR_GOB_HEIGHT * gobs_per_block;
  const uint64_t block_bytes = (uint64_t)BLOCK_LINEAR_GOB_BYTES * gobs_per_block;
  const uint64_t block = (uint64_t)(y / block_rows) * gobs_wide(width_bytes) + x / BLOCK_LINEAR_GOB_WIDTH;
  const uint32_t gob_in_block = (y % block_rows) / BLOCK_LINEAR_GOB_HEIGHT;
  const uint32_t gx = x % BLOCK_LINEAR_GOB_WIDTH, gy = y % BLOCK_LINEAR_GOB_HEIGHT;
  const uint32_t in_gob = (gx / 32u) * 256u + (gy / 2u) * 64u + ((gx % 32u) / 16u) * 32u + (gy % 2u) * 16u + gx % 16u;
  return block * block_bytes + (uint64_t)gob_in_block * BLOCK_LINEAR_GOB_BYTES + in_gob;
}

/* Rows are walked in 16-byte runs: each run is contiguous in both. */
void block_linear_to_pitch(const uint8_t *src, uint8_t *dst, uint32_t dst_stride, uint32_t width_bytes,
                           uint32_t height, uint32_t block_height_log2) {
  for (uint32_t y = 0; y < height; y++) {
    uint8_t *row = dst + (uint64_t)y * dst_stride;
    for (uint32_t x = 0; x < width_bytes; x += GOB_SPAN) {
      const uint32_t n = width_bytes - x < GOB_SPAN ? width_bytes - x : GOB_SPAN;
      memcpy(row + x, src + block_linear_offset(x, y, width_bytes, block_height_log2), n);
    }
  }
}

void pitch_to_block_linear(const uint8_t *src, uint32_t src_stride, uint8_t *dst, uint32_t width_bytes,
                           uint32_t height, uint32_t block_height_log2) {
  for (uint32_t y = 0; y < height; y++) {
    const uint8_t *row = src + (uint64_t)y * src_stride;
    for (uint32_t x = 0; x < width_bytes; x += GOB_SPAN) {
      const uint32_t n = width_bytes - x < GOB_SPAN ? width_bytes - x : GOB_SPAN;
      memcpy(dst + block_linear_offset(x, y, width_bytes, block_height_log2), row + x, n);
    }
  }
}
