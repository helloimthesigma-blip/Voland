/**
 * Tegra block-linear surface layout (§13): the tiling NVIDIA's display
 * and texture units read, and what libnx's framebuffer and NVN render
 * targets use. A surface is a grid of blocks, row-major; a block is
 * 64 bytes wide and 2^block_height_log2 GOBs tall; a GOB is 64 bytes x
 * 8 rows (512 bytes) in the 16Bx2 arrangement:
 *
 *   offset in GOB = (x / 32) * 256 + ((y % 8) / 2) * 64
 *                 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16)
 *
 * with x a byte column within the GOB. Pure functions over host memory;
 * the compositor copies the guest surface into host memory first.
 */
#ifndef SWITCH_GPU_BLOCK_LINEAR_H
#define SWITCH_GPU_BLOCK_LINEAR_H

#include <stdint.h>

#define BLOCK_LINEAR_GOB_WIDTH 64u  /* bytes */
#define BLOCK_LINEAR_GOB_HEIGHT 8u  /* rows */
#define BLOCK_LINEAR_GOB_BYTES 512u

/* Bytes a block-linear surface of `width_bytes` x `height` occupies
 * (width rounded up to GOBs, height to blocks). */
uint64_t block_linear_size(uint32_t width_bytes, uint32_t height, uint32_t block_height_log2);

/* Byte offset of (x_bytes, y) in a block-linear surface whose rows are
 * `width_bytes` wide (any value; rounded up to whole GOBs). */
uint64_t block_linear_offset(uint32_t x_bytes, uint32_t y, uint32_t width_bytes, uint32_t block_height_log2);

/* Block-linear `src` (block_linear_size bytes) -> pitch-linear `dst`
 * rows of `dst_stride` bytes; copies `width_bytes` of each of `height`
 * rows. */
void block_linear_to_pitch(const uint8_t *src, uint8_t *dst, uint32_t dst_stride, uint32_t width_bytes,
                           uint32_t height, uint32_t block_height_log2);

/* The inverse (tests, and guest-visible uploads later). */
void pitch_to_block_linear(const uint8_t *src, uint32_t src_stride, uint8_t *dst, uint32_t width_bytes,
                           uint32_t height, uint32_t block_height_log2);

#endif /* SWITCH_GPU_BLOCK_LINEAR_H */
