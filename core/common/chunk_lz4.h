/**
 * LZ4 block compression of save-state chunks (platform/web/workers/
 * savestate.ts stores each non-zero 64 KiB chunk this way). Uses the
 * vendored LZ4 (third_party/lz4) with a static compression state: no
 * allocation. Compression is not thread-safe (one static state); the web
 * CPU worker calls it while the machine is frozen.
 */
#ifndef SWITCH_COMMON_CHUNK_LZ4_H
#define SWITCH_COMMON_CHUNK_LZ4_H

#include <stdbool.h>
#include <stdint.h>

/* The most bytes chunk_lz4_compress writes for `size` input bytes. */
uint32_t chunk_lz4_bound(uint32_t size);

/* Compresses `size` bytes at `src` into `dst` (at most `capacity`
 * bytes); returns the compressed size, or 0 if it would not be smaller
 * than the input (store the chunk raw) or does not fit. */
uint32_t chunk_lz4_compress(const void *src, uint32_t size, void *dst, uint32_t capacity);

/* Decompresses the `size` bytes at `src` into exactly `expected` bytes at
 * `dst`; false if the data is damaged or decodes to another length. */
bool chunk_lz4_decompress(const void *src, uint32_t size, void *dst, uint32_t expected);

#endif
