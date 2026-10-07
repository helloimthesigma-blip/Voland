#include "gpu/compute.h"

#include <string.h>

/* A field of the multi-word QMD: bits [lo, lo + width) (width <= 32). */
static uint32_t qmd_field(const uint32_t qmd[COMPUTE_QMD_WORDS], uint32_t lo, uint32_t width) {
  const uint32_t word = lo / 32u, shift = lo % 32u;
  uint64_t v = qmd[word];
  if (word + 1u < COMPUTE_QMD_WORDS) v |= (uint64_t)qmd[word + 1u] << 32;
  v >>= shift;
  return (uint32_t)(width >= 32u ? v : v & ((1ull << width) - 1ull));
}

/* QMD v01_07 bit positions (NVIDIA open-gpu-doc, clb1c0qmd.h). */
#define QMD_PROGRAM_OFFSET 256u
#define QMD_CTA_RASTER_WIDTH 384u
#define QMD_CTA_RASTER_HEIGHT 416u
#define QMD_CTA_RASTER_DEPTH 432u
#define QMD_SHARED_MEMORY_SIZE 544u
#define QMD_SHARED_MEMORY_SIZE_BITS 18u
#define QMD_CTA_THREAD_DIMENSION0 592u
#define QMD_CTA_THREAD_DIMENSION1 608u
#define QMD_CTA_THREAD_DIMENSION2 624u
#define QMD_CONSTANT_BUFFER_VALID 640u
#define QMD_CONSTANT_BUFFER_ADDR_LOWER 928u /* + 64 * i */
#define QMD_CONSTANT_BUFFER_ADDR_UPPER 960u
#define QMD_CONSTANT_BUFFER_ADDR_UPPER_BITS 8u
#define QMD_CONSTANT_BUFFER_SIZE 975u
#define QMD_CONSTANT_BUFFER_SIZE_BITS 17u
#define QMD_CONSTANT_BUFFER_STRIDE 64u
#define QMD_SHADER_LOCAL_MEMORY_LOW_SIZE 1440u
#define QMD_SHADER_LOCAL_MEMORY_SIZE_BITS 24u
#define QMD_BARRIER_COUNT 1467u
#define QMD_BARRIER_COUNT_BITS 5u
#define QMD_REGISTER_COUNT 1496u
#define QMD_HALF_BITS 16u
#define QMD_BYTE_BITS 8u

bool compute_qmd_parse(const uint32_t qmd[COMPUTE_QMD_WORDS], Compute_Launch *out) {
  memset(out, 0, sizeof(*out));
  out->program_offset = qmd_field(qmd, QMD_PROGRAM_OFFSET, 32u);
  out->grid[0] = qmd_field(qmd, QMD_CTA_RASTER_WIDTH, 32u);
  out->grid[1] = qmd_field(qmd, QMD_CTA_RASTER_HEIGHT, QMD_HALF_BITS);
  out->grid[2] = qmd_field(qmd, QMD_CTA_RASTER_DEPTH, QMD_HALF_BITS);
  out->shared_bytes = qmd_field(qmd, QMD_SHARED_MEMORY_SIZE, QMD_SHARED_MEMORY_SIZE_BITS);
  out->block[0] = qmd_field(qmd, QMD_CTA_THREAD_DIMENSION0, QMD_HALF_BITS);
  out->block[1] = qmd_field(qmd, QMD_CTA_THREAD_DIMENSION1, QMD_HALF_BITS);
  out->block[2] = qmd_field(qmd, QMD_CTA_THREAD_DIMENSION2, QMD_HALF_BITS);
  out->local_bytes = qmd_field(qmd, QMD_SHADER_LOCAL_MEMORY_LOW_SIZE, QMD_SHADER_LOCAL_MEMORY_SIZE_BITS);
  out->barrier_count = qmd_field(qmd, QMD_BARRIER_COUNT, QMD_BARRIER_COUNT_BITS);
  out->register_count = qmd_field(qmd, QMD_REGISTER_COUNT, QMD_BYTE_BITS);
  for (uint32_t i = 0; i < COMPUTE_CBUFS; i++) {
    if (!qmd_field(qmd, QMD_CONSTANT_BUFFER_VALID + i, 1u)) continue;
    const uint32_t base = i * QMD_CONSTANT_BUFFER_STRIDE;
    out->cbuf_valid |= 1u << i;
    out->cbuf_address[i] = (uint64_t)qmd_field(qmd, QMD_CONSTANT_BUFFER_ADDR_LOWER + base, 32u) |
                           ((uint64_t)qmd_field(qmd, QMD_CONSTANT_BUFFER_ADDR_UPPER + base,
                                                QMD_CONSTANT_BUFFER_ADDR_UPPER_BITS) << 32);
    out->cbuf_size[i] = qmd_field(qmd, QMD_CONSTANT_BUFFER_SIZE + base, QMD_CONSTANT_BUFFER_SIZE_BITS);
  }
  const uint64_t threads = (uint64_t)out->block[0] * out->block[1] * out->block[2];
  const uint64_t blocks = (uint64_t)out->grid[0] * out->grid[1] * out->grid[2];
  return threads > 0 && threads <= COMPUTE_MAX_BLOCK_THREADS && blocks > 0;
}
