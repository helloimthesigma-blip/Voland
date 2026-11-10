#include "common/chunk_lz4.h"

#include "third_party/lz4/lz4.h"

/* LZ4's own trade of speed for ratio: 1 is its default (the best ratio of
 * the fast mode). */
#define CHUNK_LZ4_ACCELERATION 1

static LZ4_stream_t g_state;

uint32_t chunk_lz4_bound(uint32_t size) { return (uint32_t)LZ4_COMPRESSBOUND(size); }

uint32_t chunk_lz4_compress(const void *src, uint32_t size, void *dst, uint32_t capacity) {
  const int written = LZ4_compress_fast_extState(&g_state, (const char *)src, (char *)dst, (int)size, (int)capacity,
                                                 CHUNK_LZ4_ACCELERATION);
  return written > 0 && (uint32_t)written < size ? (uint32_t)written : 0;
}

bool chunk_lz4_decompress(const void *src, uint32_t size, void *dst, uint32_t expected) {
  return LZ4_decompress_safe((const char *)src, (char *)dst, (int)size, (int)expected) == (int)expected;
}
