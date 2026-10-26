/**
 * NCA indirect storage (§12 loader): an update ("patch") NCA does not
 * carry a whole RomFS. Its RomFS section holds only the changed bytes plus
 * a bucket-tree table (the FsHeader's PatchInfo "indirect" table) mapping
 * each range of the PATCHED (virtual) view to either the BASE game's
 * same-type section or the patch section itself. The patched view, sliced
 * by the patch FsHeader's hash-layer data region (data_offset/data_size,
 * relative to the section start and positions in the virtual view), is
 * the updated RomFS.
 *
 *   base  = the base NCA's RomFS section raw bytes, from the section start
 *   patch = the patch NCA's RomFS section raw bytes, from the section start
 *   table = [table_offset, + table_size) of `patch`
 *
 * This is a storage format, not a cipher: both inputs must already be
 * decrypted (§1.6). Layout per the public
 * NCA documentation (switchbrew's "NCA" page); the code is Voland's own.
 *
 *   Table: nodes of NCA_BUCKET_NODE_BYTES. Node 0 is the L1 node
 *     {s32 index, s32 entry set count, s64 virtual end} + s64 start offset
 *     per entry set; entry sets follow, one node each (no L2 level), each
 *     {s32 index, s32 entry count, s64 end} + entries.
 *   Entry (0x14): s64 virtual offset, s64 physical offset, s32 storage
 *     index (NCA_INDIRECT_STORAGE_BASE / _PATCH). An entry spans to the
 *     next entry's virtual offset (the last: to the tree's virtual end).
 *     A read at virtual v inside an entry comes from its storage at
 *     physical + (v - virtual).
 */
#ifndef SWITCH_HLE_LOADER_NCA_INDIRECT_H
#define SWITCH_HLE_LOADER_NCA_INDIRECT_H

#include <stdint.h>

#include "common/arena.h"
#include "common/result.h"
#include "hle/loader/byte_source.h"
#include "hle/loader/nca_compressed.h" /* NCA_BUCKET_* node layout */

#define NCA_INDIRECT_ENTRY_BYTES 0x14u
#define NCA_INDIRECT_STORAGE_BASE 0u
#define NCA_INDIRECT_STORAGE_PATCH 1u
#define NCA_INDIRECT_STORAGE_COUNT 2u

typedef struct NCA_Indirect_Entry {
  uint64_t virtual_offset;
  uint64_t physical_offset;
  uint32_t storage; /* NCA_INDIRECT_STORAGE_BASE or _PATCH */
} NCA_Indirect_Entry;

typedef struct NCA_Indirect {
  Byte_Source source;                                  /* the patched (virtual) view */
  const Byte_Source *storages[NCA_INDIRECT_STORAGE_COUNT]; /* base, patch */
  NCA_Indirect_Entry *entries;                         /* ascending virtual offsets */
  uint32_t entry_count;
  uint64_t virtual_size;
} NCA_Indirect;

/* Arena bytes nca_indirect_open needs for a table of `entry_count`
 * entries. */
uint64_t nca_indirect_arena_bytes(uint32_t entry_count);

/* Entry count of the table, for sizing the arena before
 * nca_indirect_open. Validates the whole table (see nca_indirect_open). */
Error nca_indirect_measure(const Byte_Source *base, const Byte_Source *patch, uint64_t table_offset,
                           uint64_t table_size, const uint8_t bucket_header[NCA_BUCKET_HEADER_BYTES],
                           uint32_t *entry_count);

/* Reads the bucket-tree header `bucket_header` (16 bytes from the
 * PatchInfo) and the table at [table_offset, + table_size) of `patch`,
 * and prepares `out->source`. Storage comes from `arena`; `base` and
 * `patch` must outlive `out`.
 *   RESULT_INVALID_ARGUMENT NULL args; bad magic/version; malformed nodes;
 *                           a table not starting at virtual 0; entries out
 *                           of order; an unknown storage index; an entry
 *                           whose physical range falls outside its
 *                           storage; an L2 level
 *   RESULT_OUT_OF_MEMORY    the arena is too small
 *   RESULT_IO_ERROR         from `patch` */
Error nca_indirect_open(NCA_Indirect *out, const Byte_Source *base, const Byte_Source *patch, uint64_t table_offset,
                        uint64_t table_size, const uint8_t bucket_header[NCA_BUCKET_HEADER_BYTES], Arena *arena);

#endif /* SWITCH_HLE_LOADER_NCA_INDIRECT_H */
