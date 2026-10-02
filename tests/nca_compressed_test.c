/**
 * NCA compressed storage (hle/loader/nca_compressed.h) over a synthetic
 * data region: a raw block, an all-zero block and an LZ4 block, mapped by
 * a one-entry-set bucket table. Reads that straddle blocks, the cached
 * LZ4 block, and the malformed-table rejections.
 */
#define CHECK_NAME "nca_compressed_test"
#include "check.h"

#include "common/arena.h"
#include "hle/loader/nca_compressed.h"
#include "third_party/lz4/lz4.h"

#include <stdio.h>
#include <string.h>

#define RAW_BYTES 0x100u
#define ZERO_BYTES 0x200u
#define LZ4_BYTES 0x1000u
#define VIRTUAL_BYTES (RAW_BYTES + ZERO_BYTES + LZ4_BYTES)
#define TABLE_OFFSET 0x2000u
#define REGION_BYTES (TABLE_OFFSET + 2u * NCA_BUCKET_NODE_BYTES)

static uint8_t g_region[REGION_BYTES];
static uint8_t g_expected[VIRTUAL_BYTES];
static uint8_t g_header[NCA_BUCKET_HEADER_BYTES];

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void put_entry(uint8_t *p, uint64_t virt, uint64_t phys, uint8_t type, uint32_t phys_size) {
  put64(p, virt);
  put64(p + 8, phys);
  p[16] = type;
  put32(p + 20, phys_size);
}

/* Builds the region: raw bytes at 0, the LZ4 block after them, the table
 * at TABLE_OFFSET. Returns the LZ4 block's compressed size. */
static uint32_t build(void) {
  memset(g_region, 0xEE, sizeof(g_region));
  for (uint32_t i = 0; i < RAW_BYTES; i++) g_expected[i] = (uint8_t)(i * 7u + 1u);
  memset(g_expected + RAW_BYTES, 0, ZERO_BYTES);
  for (uint32_t i = 0; i < LZ4_BYTES; i++) g_expected[RAW_BYTES + ZERO_BYTES + i] = (uint8_t)((i / 64u) ^ (i & 3u));
  memcpy(g_region, g_expected, RAW_BYTES);
  const int packed = LZ4_compress_default((const char *)g_expected + RAW_BYTES + ZERO_BYTES, (char *)g_region + RAW_BYTES,
                                          (int)LZ4_BYTES, (int)(TABLE_OFFSET - RAW_BYTES));
  CHECK(packed > 0 && (uint32_t)packed < LZ4_BYTES);
  uint8_t *l1 = g_region + TABLE_OFFSET;
  memset(l1, 0, 2u * NCA_BUCKET_NODE_BYTES);
  put32(l1, 0);
  put32(l1 + 4, 1); /* one entry set */
  put64(l1 + 8, VIRTUAL_BYTES);
  put64(l1 + 16, 0);
  uint8_t *set = l1 + NCA_BUCKET_NODE_BYTES;
  put32(set, 0);
  put32(set + 4, 3);
  put64(set + 8, VIRTUAL_BYTES);
  uint8_t *e = set + NCA_BUCKET_HEADER_BYTES;
  put_entry(e, 0, 0, NCA_COMPRESSION_NONE, RAW_BYTES);
  put_entry(e + NCA_COMPRESSION_ENTRY_BYTES, RAW_BYTES, 0, NCA_COMPRESSION_ZEROS, 0);
  put_entry(e + 2u * NCA_COMPRESSION_ENTRY_BYTES, RAW_BYTES + ZERO_BYTES, RAW_BYTES, NCA_COMPRESSION_LZ4,
            (uint32_t)packed);
  memset(g_header, 0, sizeof(g_header));
  put32(g_header, NCA_BUCKET_MAGIC);
  put32(g_header + 4, 1);
  put32(g_header + 8, 3);
  return (uint32_t)packed;
}

int main(void) {
  build();
  const Byte_Source raw = byte_source_from_memory(g_region, sizeof(g_region));
  uint32_t entries = 0, max_block = 0;
  CHECK_OK(nca_compressed_measure(&raw, TABLE_OFFSET, 2u * NCA_BUCKET_NODE_BYTES, g_header, &entries, &max_block));
  CHECK(entries == 3 && max_block == LZ4_BYTES);

  Arena arena;
  CHECK(arena_create(&arena, nca_compressed_arena_bytes(entries, max_block)));
  NCA_Compressed c;
  CHECK_OK(nca_compressed_open(&c, &raw, TABLE_OFFSET, 2u * NCA_BUCKET_NODE_BYTES, g_header, &arena));
  CHECK(c.source.size == VIRTUAL_BYTES);

  /* Whole view, then reads straddling raw/zero and zero/LZ4, and two
   * reads inside the (now cached) LZ4 block. */
  static uint8_t out[VIRTUAL_BYTES];
  CHECK_OK(byte_source_read(&c.source, 0, out, VIRTUAL_BYTES));
  CHECK(memcmp(out, g_expected, VIRTUAL_BYTES) == 0);
  memset(out, 0xAA, sizeof(out));
  CHECK_OK(byte_source_read(&c.source, RAW_BYTES - 8u, out, 16));
  CHECK(memcmp(out, g_expected + RAW_BYTES - 8u, 16) == 0);
  CHECK_OK(byte_source_read(&c.source, RAW_BYTES + ZERO_BYTES - 4u, out, 64));
  CHECK(memcmp(out, g_expected + RAW_BYTES + ZERO_BYTES - 4u, 64) == 0);
  CHECK_OK(byte_source_read(&c.source, VIRTUAL_BYTES - 100u, out, 100));
  CHECK(memcmp(out, g_expected + VIRTUAL_BYTES - 100u, 100) == 0);
  CHECK(c.cached == 2);
  arena_destroy(&arena);

  /* Rejections: bad magic, an L1 node disagreeing with the header, an
   * unknown compression type, a block outside the region. */
  CHECK(arena_create(&arena, nca_compressed_arena_bytes(entries, max_block)));
  uint8_t bad[NCA_BUCKET_HEADER_BYTES];
  memcpy(bad, g_header, sizeof(bad));
  bad[0] ^= 1u;
  CHECK_CODE(nca_compressed_open(&c, &raw, TABLE_OFFSET, 2u * NCA_BUCKET_NODE_BYTES, bad, &arena),
             RESULT_INVALID_ARGUMENT);
  put32(g_region + TABLE_OFFSET + 4, 2);
  CHECK_CODE(nca_compressed_open(&c, &raw, TABLE_OFFSET, 3u * NCA_BUCKET_NODE_BYTES, g_header, &arena),
             RESULT_INVALID_ARGUMENT);
  build();
  g_region[TABLE_OFFSET + NCA_BUCKET_NODE_BYTES + NCA_BUCKET_HEADER_BYTES + 16u] = 7; /* first entry's type */
  CHECK_CODE(nca_compressed_open(&c, &raw, TABLE_OFFSET, 2u * NCA_BUCKET_NODE_BYTES, g_header, &arena),
             RESULT_INVALID_ARGUMENT);
  build();
  put64(g_region + TABLE_OFFSET + NCA_BUCKET_NODE_BYTES + NCA_BUCKET_HEADER_BYTES + 2u * NCA_COMPRESSION_ENTRY_BYTES + 8u,
        REGION_BYTES); /* LZ4 block past the region */
  CHECK_CODE(nca_compressed_open(&c, &raw, TABLE_OFFSET, 2u * NCA_BUCKET_NODE_BYTES, g_header, &arena),
             RESULT_INVALID_ARGUMENT);
  arena_destroy(&arena);
  printf("[nca_compressed_test] all tests passed\n");
  return 0;
}
