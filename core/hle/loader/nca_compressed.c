/**
 * NCA compressed storage: see nca_compressed.h.
 */
#include "hle/loader/nca_compressed.h"

#include "third_party/lz4/lz4.h"

#include <limits.h>
#include <string.h>

#define BUCKET_VERSION 1u
#define BUCKET_OFFSETS_PER_NODE ((NCA_BUCKET_NODE_BYTES - NCA_BUCKET_HEADER_BYTES) / 8u)
#define BUCKET_ENTRIES_PER_SET ((NCA_BUCKET_NODE_BYTES - NCA_BUCKET_HEADER_BYTES) / NCA_COMPRESSION_ENTRY_BYTES)

/* The loader is single-threaded (byte_source.h): one node buffer. */
static uint8_t g_node[NCA_BUCKET_NODE_BYTES];

typedef struct Tree_Shape {
  uint32_t entry_count;
  uint32_t set_count;
  uint64_t virtual_size;
} Tree_Shape;

static Error read_shape(const Byte_Source *raw, uint64_t table_offset, uint64_t table_size,
                        const uint8_t bucket_header[NCA_BUCKET_HEADER_BYTES], Tree_Shape *out) {
  if (byte_source_le32(bucket_header) != NCA_BUCKET_MAGIC || byte_source_le32(bucket_header + 4) != BUCKET_VERSION) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: compression table header is not BKTR v1");
  }
  const int32_t entries = (int32_t)byte_source_le32(bucket_header + 8);
  if (entries <= 0) return ERR(RESULT_INVALID_ARGUMENT, "nca: compression table has no entries");
  out->entry_count = (uint32_t)entries;
  out->set_count = (out->entry_count + BUCKET_ENTRIES_PER_SET - 1u) / BUCKET_ENTRIES_PER_SET;
  if (out->set_count > BUCKET_OFFSETS_PER_NODE) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: compression table needs an L2 level (not supported)");
  }
  if (table_size < (uint64_t)NCA_BUCKET_NODE_BYTES * (1u + out->set_count) || table_offset > raw->size ||
      table_size > raw->size - table_offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: compression table outside the section");
  }
  Error err = byte_source_read(raw, table_offset, g_node, NCA_BUCKET_NODE_BYTES);
  if (!error_is_ok(err)) return err;
  if (byte_source_le32(g_node + 4) != out->set_count) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: compression table L1 node disagrees with its header");
  }
  out->virtual_size = byte_source_le64(g_node + 8);
  return OK;
}

/* Calls `visit` for every entry in order (decoded from the entry sets). */
typedef Error (*Entry_Visit)(void *user, uint32_t index, const NCA_Compressed_Entry *entry);

static Error walk_entries(const Byte_Source *raw, uint64_t table_offset, const Tree_Shape *shape, Entry_Visit visit,
                          void *user) {
  uint32_t index = 0;
  for (uint32_t set = 0; set < shape->set_count; set++) {
    const uint64_t at = table_offset + (uint64_t)NCA_BUCKET_NODE_BYTES * (1u + set);
    Error err = byte_source_read(raw, at, g_node, NCA_BUCKET_NODE_BYTES);
    if (!error_is_ok(err)) return err;
    const uint32_t count = byte_source_le32(g_node + 4);
    if (count == 0 || count > BUCKET_ENTRIES_PER_SET || index + count > shape->entry_count) {
      return ERR(RESULT_INVALID_ARGUMENT, "nca: compression entry set malformed");
    }
    for (uint32_t i = 0; i < count; i++, index++) {
      const uint8_t *e = g_node + NCA_BUCKET_HEADER_BYTES + i * NCA_COMPRESSION_ENTRY_BYTES;
      const NCA_Compressed_Entry entry = {
          .virtual_offset = byte_source_le64(e),
          .physical_offset = byte_source_le64(e + 8),
          .type = e[16],
          .physical_size = byte_source_le32(e + 20),
      };
      err = visit(user, index, &entry);
      if (!error_is_ok(err)) return err;
    }
  }
  if (index != shape->entry_count) return ERR(RESULT_INVALID_ARGUMENT, "nca: compression entry count mismatch");
  return OK;
}

typedef struct Measure {
  const Byte_Source *raw;
  uint64_t virtual_size;
  NCA_Compressed_Entry previous;
  bool has_previous;
  uint32_t max_block;
} Measure;

static Error check_and_measure(Measure *m, const NCA_Compressed_Entry *entry, uint64_t next_virtual) {
  if (next_virtual < entry->virtual_offset) return ERR(RESULT_INVALID_ARGUMENT, "nca: compression entries out of order");
  const uint64_t span = next_virtual - entry->virtual_offset;
  if (entry->type != NCA_COMPRESSION_NONE && entry->type != NCA_COMPRESSION_ZEROS &&
      entry->type != NCA_COMPRESSION_LZ4) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: unknown compression type");
  }
  const uint64_t physical = entry->type == NCA_COMPRESSION_NONE ? span : entry->physical_size;
  if (entry->type != NCA_COMPRESSION_ZEROS &&
      (entry->physical_offset > m->raw->size || physical > m->raw->size - entry->physical_offset)) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: compressed block outside the section");
  }
  if (entry->type == NCA_COMPRESSION_LZ4) {
    if (span > NCA_COMPRESSION_MAX_BLOCK || entry->physical_size > NCA_COMPRESSION_MAX_BLOCK) {
      return ERR(RESULT_INVALID_ARGUMENT, "nca: compressed block too large");
    }
    if (span > m->max_block) m->max_block = (uint32_t)span;
    if (entry->physical_size > m->max_block) m->max_block = entry->physical_size;
  }
  return OK;
}

static Error measure_visit(void *user, uint32_t index, const NCA_Compressed_Entry *entry) {
  (void)index;
  Measure *m = (Measure *)user;
  if (m->has_previous) {
    const Error err = check_and_measure(m, &m->previous, entry->virtual_offset);
    if (!error_is_ok(err)) return err;
  } else if (entry->virtual_offset != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: compression table does not start at 0");
  }
  m->previous = *entry;
  m->has_previous = true;
  return OK;
}

Error nca_compressed_measure(const Byte_Source *raw, uint64_t table_offset, uint64_t table_size,
                             const uint8_t bucket_header[NCA_BUCKET_HEADER_BYTES], uint32_t *entry_count,
                             uint32_t *max_block) {
  Tree_Shape shape;
  Error err = read_shape(raw, table_offset, table_size, bucket_header, &shape);
  if (!error_is_ok(err)) return err;
  Measure m;
  memset(&m, 0, sizeof(m));
  m.raw = raw;
  m.virtual_size = shape.virtual_size;
  err = walk_entries(raw, table_offset, &shape, measure_visit, &m);
  if (!error_is_ok(err)) return err;
  err = check_and_measure(&m, &m.previous, shape.virtual_size);
  if (!error_is_ok(err)) return err;
  *entry_count = shape.entry_count;
  *max_block = m.max_block;
  return OK;
}

uint64_t nca_compressed_arena_bytes(uint32_t entry_count, uint32_t max_block) {
  const uint64_t slack = 64u; /* alignment */
  return (uint64_t)entry_count * sizeof(NCA_Compressed_Entry) + 2u * (uint64_t)max_block + 3u * slack;
}

static Error store_visit(void *user, uint32_t index, const NCA_Compressed_Entry *entry) {
  NCA_Compressed *c = (NCA_Compressed *)user;
  c->entries[index] = *entry;
  return OK;
}

/* Last entry whose virtual offset <= `offset`. */
static uint32_t find_entry(const NCA_Compressed *c, uint64_t offset) {
  uint32_t lo = 0, hi = c->entry_count;
  while (hi - lo > 1u) {
    const uint32_t mid = lo + (hi - lo) / 2u;
    if (c->entries[mid].virtual_offset <= offset) lo = mid;
    else hi = mid;
  }
  return lo;
}

static Error compressed_read(void *user, uint64_t offset, void *out, uint64_t size) {
  NCA_Compressed *c = (NCA_Compressed *)user;
  uint8_t *to = (uint8_t *)out;
  uint32_t i = find_entry(c, offset);
  while (size > 0) {
    const NCA_Compressed_Entry *e = &c->entries[i];
    const uint64_t end = i + 1u < c->entry_count ? c->entries[i + 1u].virtual_offset : c->virtual_size;
    const uint64_t within = offset - e->virtual_offset;
    const uint64_t n = end - offset < size ? end - offset : size;
    Error err = OK;
    switch (e->type) {
    case NCA_COMPRESSION_NONE:
      err = byte_source_read(c->raw, e->physical_offset + within, to, n);
      break;
    case NCA_COMPRESSION_ZEROS:
      memset(to, 0, (size_t)n);
      break;
    default: { /* LZ4: decompress the whole block once, serve from the cache */
      if (c->cached != (int64_t)i) {
        err = byte_source_read(c->raw, e->physical_offset, c->block_in, e->physical_size);
        if (!error_is_ok(err)) return err;
        const int span = (int)(end - e->virtual_offset);
        const int got = LZ4_decompress_safe((const char *)c->block_in, (char *)c->block_out, (int)e->physical_size, span);
        if (got != span) {
          c->cached = -1;
          return ERR(RESULT_IO_ERROR, "nca: LZ4 block did not decompress to its size");
        }
        c->cached = (int64_t)i;
      }
      memcpy(to, c->block_out + within, (size_t)n);
      break;
    }
    }
    if (!error_is_ok(err)) return err;
    to += n;
    offset += n;
    size -= n;
    i++;
  }
  return OK;
}

Error nca_compressed_open(NCA_Compressed *out, const Byte_Source *raw, uint64_t table_offset, uint64_t table_size,
                          const uint8_t bucket_header[NCA_BUCKET_HEADER_BYTES], Arena *arena) {
  memset(out, 0, sizeof(*out));
  uint32_t entry_count = 0, max_block = 0;
  Error err = nca_compressed_measure(raw, table_offset, table_size, bucket_header, &entry_count, &max_block);
  if (!error_is_ok(err)) return err;
  Tree_Shape shape;
  err = read_shape(raw, table_offset, table_size, bucket_header, &shape);
  if (!error_is_ok(err)) return err;
  out->entries = ARENA_ALLOC_ARRAY(arena, NCA_Compressed_Entry, entry_count);
  out->block_in = max_block ? ARENA_ALLOC_ARRAY(arena, uint8_t, max_block) : NULL;
  out->block_out = max_block ? ARENA_ALLOC_ARRAY(arena, uint8_t, max_block) : NULL;
  if (!out->entries || (max_block && (!out->block_in || !out->block_out))) {
    return ERR(RESULT_OUT_OF_MEMORY, "nca: compression table does not fit its arena");
  }
  out->entry_count = entry_count;
  out->block_capacity = max_block;
  out->virtual_size = shape.virtual_size;
  out->raw = raw;
  out->cached = -1;
  err = walk_entries(raw, table_offset, &shape, store_visit, out);
  if (!error_is_ok(err)) return err;
  out->source.user = out;
  out->source.size = out->virtual_size;
  out->source.read = compressed_read;
  return OK;
}
