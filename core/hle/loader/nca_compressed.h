/**
 * NCA compressed storage (§12 loader): newer titles store a section's
 * filesystem as blocks that are raw, all-zero or LZ4 compressed. The
 * section's data region (the hash layer's data level) holds that stream;
 * the FsHeader's CompressionInfo names a bucket-tree table inside it,
 * mapping each virtual (decompressed) range to its block. The virtual view
 * is the filesystem (RomFS). Table and physical offsets count from the
 * data region's start. Layout confirmed against a real title: the table
 * sits at its stated offset from the data region, not the section.
 *
 * This is a storage format, not a cipher: the input must already be
 * decrypted (§1.6). Layout per the public NCA documentation (switchbrew's
 * "NCA" page); the code is Voland's own.
 *
 *   Table: nodes of NCA_BUCKET_NODE_BYTES. Node 0 is the L1 node
 *     {s32 index, s32 entry set count, s64 virtual end} + s64 start offset
 *     per entry set; entry sets follow (one node each, when the set count
 *     fits one L1 node - no L2 level), each
 *     {s32 index, s32 entry count, s64 end} + entries.
 *   Entry (0x18): s64 virtual offset, s64 physical offset (from the
 *     data region's start), u8 type (0 none, 1 zeros, 3 LZ4), s8 level,
 *     u16 reserved, u32 physical size. An entry spans to the next entry's
 *     virtual offset (the last: to the tree's virtual end).
 */
#ifndef SWITCH_HLE_LOADER_NCA_COMPRESSED_H
#define SWITCH_HLE_LOADER_NCA_COMPRESSED_H

#include <stdbool.h>
#include <stdint.h>

#include "common/arena.h"
#include "common/result.h"
#include "hle/loader/byte_source.h"

#define NCA_BUCKET_NODE_BYTES 0x4000u
#define NCA_BUCKET_HEADER_BYTES 0x10u
#define NCA_BUCKET_MAGIC 0x52544B42u /* "BKTR" */
#define NCA_COMPRESSION_ENTRY_BYTES 0x18u
#define NCA_COMPRESSION_NONE 0u
#define NCA_COMPRESSION_ZEROS 1u
#define NCA_COMPRESSION_LZ4 3u
#define NCA_COMPRESSION_MAX_BLOCK (16u * 1024u * 1024u) /* sanity bound on one block */

typedef struct NCA_Compressed_Entry {
  uint64_t virtual_offset;
  uint64_t physical_offset;
  uint32_t physical_size;
  uint8_t type;
} NCA_Compressed_Entry;

typedef struct NCA_Compressed {
  Byte_Source source;            /* the virtual (decompressed) view */
  const Byte_Source *raw;        /* the section's raw bytes, from its start */
  NCA_Compressed_Entry *entries;
  uint32_t entry_count;
  uint64_t virtual_size;
  uint8_t *block_in;             /* one physical block */
  uint8_t *block_out;            /* the decompressed block cached in it */
  uint32_t block_capacity;
  int64_t cached;                /* entry whose block is in block_out, or -1 */
} NCA_Compressed;

/* Arena bytes nca_compressed_open needs for a table of `entry_count`
 * entries whose largest block is `max_block` bytes. */
uint64_t nca_compressed_arena_bytes(uint32_t entry_count, uint32_t max_block);

/* Reads the bucket-tree header `bucket_header` (16 bytes from the
 * FsHeader) and the table at [table_offset, + table_size) of `raw`, and
 * prepares `out->source`. Storage comes from `arena`.
 *   RESULT_INVALID_ARGUMENT bad magic/version, malformed nodes, entries
 *                           out of order or outside `raw`, an L2 level,
 *                           an unknown compression type
 *   RESULT_OUT_OF_MEMORY    the arena is too small
 *   RESULT_IO_ERROR         from `raw` */
Error nca_compressed_open(NCA_Compressed *out, const Byte_Source *raw, uint64_t table_offset, uint64_t table_size,
                          const uint8_t bucket_header[NCA_BUCKET_HEADER_BYTES], Arena *arena);

/* Entry count and largest block of the table, for sizing the arena
 * before nca_compressed_open (reads only the L1 node and entry sets). */
Error nca_compressed_measure(const Byte_Source *raw, uint64_t table_offset, uint64_t table_size,
                             const uint8_t bucket_header[NCA_BUCKET_HEADER_BYTES], uint32_t *entry_count,
                             uint32_t *max_block);

#endif /* SWITCH_HLE_LOADER_NCA_COMPRESSED_H */
