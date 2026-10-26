/**
 * NCA indirect storage: see nca_indirect.h.
 */
#include "hle/loader/nca_indirect.h"

#include <stdbool.h>
#include <string.h>

#define BUCKET_VERSION 1u
#define BUCKET_OFFSET_BYTES 8u
#define BUCKET_OFFSETS_PER_NODE ((NCA_BUCKET_NODE_BYTES - NCA_BUCKET_HEADER_BYTES) / BUCKET_OFFSET_BYTES)
#define INDIRECT_ENTRIES_PER_SET ((NCA_BUCKET_NODE_BYTES - NCA_BUCKET_HEADER_BYTES) / NCA_INDIRECT_ENTRY_BYTES)

/* Bucket header / node header field offsets. */
#define BUCKET_HEADER_OFFSET_MAGIC 0u
#define BUCKET_HEADER_OFFSET_VERSION 4u
#define BUCKET_HEADER_OFFSET_ENTRIES 8u
#define NODE_OFFSET_COUNT 4u
#define NODE_OFFSET_END 8u

/* Entry field offsets. */
#define ENTRY_OFFSET_VIRTUAL 0u
#define ENTRY_OFFSET_PHYSICAL 8u
#define ENTRY_OFFSET_STORAGE 16u

/* The loader is single-threaded (byte_source.h): one node buffer. */
static uint8_t g_node[NCA_BUCKET_NODE_BYTES];

typedef struct Tree_Shape {
  uint32_t entry_count;
  uint32_t set_count;
  uint64_t virtual_size;
} Tree_Shape;

static Error read_shape(const Byte_Source *patch, uint64_t table_offset, uint64_t table_size,
                        const uint8_t bucket_header[NCA_BUCKET_HEADER_BYTES], Tree_Shape *out) {
  if (byte_source_le32(bucket_header + BUCKET_HEADER_OFFSET_MAGIC) != NCA_BUCKET_MAGIC ||
      byte_source_le32(bucket_header + BUCKET_HEADER_OFFSET_VERSION) != BUCKET_VERSION) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: indirect table header is not BKTR v1");
  }
  const int32_t entries = (int32_t)byte_source_le32(bucket_header + BUCKET_HEADER_OFFSET_ENTRIES);
  if (entries <= 0) return ERR(RESULT_INVALID_ARGUMENT, "nca: indirect table has no entries");
  out->entry_count = (uint32_t)entries;
  out->set_count = (out->entry_count + INDIRECT_ENTRIES_PER_SET - 1u) / INDIRECT_ENTRIES_PER_SET;
  if (out->set_count > BUCKET_OFFSETS_PER_NODE) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: indirect table needs an L2 level (not supported)");
  }
  if (table_size < (uint64_t)NCA_BUCKET_NODE_BYTES * (1u + out->set_count) || table_offset > patch->size ||
      table_size > patch->size - table_offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: indirect table outside the patch section");
  }
  Error err = byte_source_read(patch, table_offset, g_node, NCA_BUCKET_NODE_BYTES);
  if (!error_is_ok(err)) return err;
  if (byte_source_le32(g_node + NODE_OFFSET_COUNT) != out->set_count) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: indirect table L1 node disagrees with its header");
  }
  out->virtual_size = byte_source_le64(g_node + NODE_OFFSET_END);
  return OK;
}

/* Validates every entry against its successor and its storage; stores
 * them into `store` when non-NULL. */
typedef struct Walk {
  const Byte_Source *const *storages;
  NCA_Indirect_Entry *store;
  NCA_Indirect_Entry previous;
  bool has_previous;
} Walk;

static Error check_entry(const Walk *w, const NCA_Indirect_Entry *entry, uint64_t next_virtual) {
  if (next_virtual < entry->virtual_offset) return ERR(RESULT_INVALID_ARGUMENT, "nca: indirect entries out of order");
  if (entry->storage >= NCA_INDIRECT_STORAGE_COUNT) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: indirect entry names an unknown storage");
  }
  const uint64_t span = next_virtual - entry->virtual_offset;
  const Byte_Source *storage = w->storages[entry->storage];
  if (entry->physical_offset > storage->size || span > storage->size - entry->physical_offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: indirect entry outside its storage");
  }
  return OK;
}

static Error walk_entries(const Byte_Source *patch, uint64_t table_offset, const Tree_Shape *shape, Walk *w) {
  uint32_t index = 0;
  for (uint32_t set = 0; set < shape->set_count; set++) {
    const uint64_t at = table_offset + (uint64_t)NCA_BUCKET_NODE_BYTES * (1u + set);
    Error err = byte_source_read(patch, at, g_node, NCA_BUCKET_NODE_BYTES);
    if (!error_is_ok(err)) return err;
    const uint32_t count = byte_source_le32(g_node + NODE_OFFSET_COUNT);
    if (count == 0 || count > INDIRECT_ENTRIES_PER_SET || count > shape->entry_count - index) {
      return ERR(RESULT_INVALID_ARGUMENT, "nca: indirect entry set malformed");
    }
    for (uint32_t i = 0; i < count; i++, index++) {
      const uint8_t *e = g_node + NCA_BUCKET_HEADER_BYTES + i * NCA_INDIRECT_ENTRY_BYTES;
      const NCA_Indirect_Entry entry = {
          .virtual_offset = byte_source_le64(e + ENTRY_OFFSET_VIRTUAL),
          .physical_offset = byte_source_le64(e + ENTRY_OFFSET_PHYSICAL),
          .storage = byte_source_le32(e + ENTRY_OFFSET_STORAGE),
      };
      if (w->has_previous) {
        err = check_entry(w, &w->previous, entry.virtual_offset);
        if (!error_is_ok(err)) return err;
      } else if (entry.virtual_offset != 0) {
        return ERR(RESULT_INVALID_ARGUMENT, "nca: indirect table does not start at 0");
      }
      if (w->store) w->store[index] = entry;
      w->previous = entry;
      w->has_previous = true;
    }
  }
  if (index != shape->entry_count) return ERR(RESULT_INVALID_ARGUMENT, "nca: indirect entry count mismatch");
  return check_entry(w, &w->previous, shape->virtual_size);
}

static Error walk_table(const Byte_Source *base, const Byte_Source *patch, uint64_t table_offset, uint64_t table_size,
                        const uint8_t bucket_header[NCA_BUCKET_HEADER_BYTES], NCA_Indirect_Entry *store,
                        Tree_Shape *shape) {
  if (!base || !patch || !bucket_header) return ERR(RESULT_INVALID_ARGUMENT, "nca_indirect: NULL argument");
  Error err = read_shape(patch, table_offset, table_size, bucket_header, shape);
  if (!error_is_ok(err)) return err;
  const Byte_Source *const storages[NCA_INDIRECT_STORAGE_COUNT] = {base, patch};
  Walk w;
  memset(&w, 0, sizeof(w));
  w.storages = storages;
  w.store = store;
  return walk_entries(patch, table_offset, shape, &w);
}

Error nca_indirect_measure(const Byte_Source *base, const Byte_Source *patch, uint64_t table_offset,
                           uint64_t table_size, const uint8_t bucket_header[NCA_BUCKET_HEADER_BYTES],
                           uint32_t *entry_count) {
  if (!entry_count) return ERR(RESULT_INVALID_ARGUMENT, "nca_indirect_measure: NULL argument");
  Tree_Shape shape;
  const Error err = walk_table(base, patch, table_offset, table_size, bucket_header, NULL, &shape);
  if (!error_is_ok(err)) return err;
  *entry_count = shape.entry_count;
  return OK;
}

uint64_t nca_indirect_arena_bytes(uint32_t entry_count) {
  const uint64_t slack = 64u; /* alignment */
  return (uint64_t)entry_count * sizeof(NCA_Indirect_Entry) + slack;
}

/* Last entry whose virtual offset <= `offset`. */
static uint32_t find_entry(const NCA_Indirect *ind, uint64_t offset) {
  uint32_t lo = 0, hi = ind->entry_count;
  while (hi - lo > 1u) {
    const uint32_t mid = lo + (hi - lo) / 2u;
    if (ind->entries[mid].virtual_offset <= offset) lo = mid;
    else hi = mid;
  }
  return lo;
}

static Error indirect_read(void *user, uint64_t offset, void *out, uint64_t size) {
  const NCA_Indirect *ind = (const NCA_Indirect *)user;
  uint8_t *to = (uint8_t *)out;
  uint32_t i = find_entry(ind, offset);
  while (size > 0) {
    if (i >= ind->entry_count) return ERR(RESULT_IO_ERROR, "nca: indirect read past the last entry");
    const NCA_Indirect_Entry *e = &ind->entries[i];
    const uint64_t end = i + 1u < ind->entry_count ? ind->entries[i + 1u].virtual_offset : ind->virtual_size;
    const uint64_t n = end - offset < size ? end - offset : size;
    if (n > 0) {
      const Error err =
          byte_source_read(ind->storages[e->storage], e->physical_offset + (offset - e->virtual_offset), to, n);
      if (!error_is_ok(err)) return err;
    }
    to += n;
    offset += n;
    size -= n;
    i++;
  }
  return OK;
}

Error nca_indirect_open(NCA_Indirect *out, const Byte_Source *base, const Byte_Source *patch, uint64_t table_offset,
                        uint64_t table_size, const uint8_t bucket_header[NCA_BUCKET_HEADER_BYTES], Arena *arena) {
  if (!out || !arena) return ERR(RESULT_INVALID_ARGUMENT, "nca_indirect_open: NULL argument");
  memset(out, 0, sizeof(*out));
  uint32_t entry_count = 0;
  Error err = nca_indirect_measure(base, patch, table_offset, table_size, bucket_header, &entry_count);
  if (!error_is_ok(err)) return err;
  out->entries = ARENA_ALLOC_ARRAY(arena, NCA_Indirect_Entry, entry_count);
  if (!out->entries) return ERR(RESULT_OUT_OF_MEMORY, "nca: indirect table does not fit its arena");
  Tree_Shape shape;
  err = walk_table(base, patch, table_offset, table_size, bucket_header, out->entries, &shape);
  if (!error_is_ok(err)) return err;
  out->entry_count = shape.entry_count;
  out->virtual_size = shape.virtual_size;
  out->storages[NCA_INDIRECT_STORAGE_BASE] = base;
  out->storages[NCA_INDIRECT_STORAGE_PATCH] = patch;
  out->source.user = out;
  out->source.size = out->virtual_size;
  out->source.read = indirect_read;
  return OK;
}
