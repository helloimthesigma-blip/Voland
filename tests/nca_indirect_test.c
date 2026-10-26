/**
 * NCA indirect storage (hle/loader/nca_indirect.h) over synthetic base
 * and patch sections: a hand-built bucket table interleaving both
 * storages, reads that span entries and sit on their boundaries, a
 * two-entry-set table exercising the binary search, and the malformed-
 * table rejections.
 */
#define CHECK_NAME "nca_indirect_test"
#include "check.h"

#include "common/arena.h"
#include "hle/loader/nca_indirect.h"

#include <stdio.h>
#include <string.h>

#define BASE_BYTES 0x3000u
#define PATCH_DATA_BYTES 0x800u
#define TABLE_OFFSET 0x1000u
#define TABLE_BYTES (2u * NCA_BUCKET_NODE_BYTES)
#define PATCH_BYTES (TABLE_OFFSET + 3u * NCA_BUCKET_NODE_BYTES)
#define ENTRY_COUNT 5u
#define VIRTUAL_BYTES 0x1C00u

/* Two-set scenario: more entries than one set holds. */
#define ENTRIES_PER_SET ((NCA_BUCKET_NODE_BYTES - NCA_BUCKET_HEADER_BYTES) / NCA_INDIRECT_ENTRY_BYTES)
#define MANY_ENTRIES (ENTRIES_PER_SET + 100u)
#define MANY_SPAN 0x10u
#define MANY_VIRTUAL (MANY_ENTRIES * MANY_SPAN)

static uint8_t g_base[BASE_BYTES];
static uint8_t g_patch[PATCH_BYTES];
static uint8_t g_expected[MANY_VIRTUAL];
static uint8_t g_header[NCA_BUCKET_HEADER_BYTES];

typedef struct Spec {
  uint64_t virt;
  uint64_t phys;
  uint32_t storage;
} Spec;

/* base 0..0x400 | patch 0..0x200 | base 0x1000..0x1800 | patch 0x200..0x800 | base 0x2000..0x2800 */
static const Spec k_specs[ENTRY_COUNT] = {
    {0x0000, 0x0000, NCA_INDIRECT_STORAGE_BASE},  {0x0400, 0x0000, NCA_INDIRECT_STORAGE_PATCH},
    {0x0600, 0x1000, NCA_INDIRECT_STORAGE_BASE},  {0x0E00, 0x0200, NCA_INDIRECT_STORAGE_PATCH},
    {0x1400, 0x2000, NCA_INDIRECT_STORAGE_BASE},
};

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static uint8_t *entry_at(uint32_t set, uint32_t i) {
  return g_patch + TABLE_OFFSET + NCA_BUCKET_NODE_BYTES * (1u + set) + NCA_BUCKET_HEADER_BYTES +
         i * NCA_INDIRECT_ENTRY_BYTES;
}

static void put_entry(uint8_t *p, const Spec *s) {
  put64(p, s->virt);
  put64(p + 8, s->phys);
  put32(p + 16, s->storage);
}

static void fill_storages(void) {
  for (uint32_t i = 0; i < BASE_BYTES; i++) g_base[i] = (uint8_t)(i * 3u + 1u);
  memset(g_patch, 0xEE, sizeof(g_patch));
  for (uint32_t i = 0; i < PATCH_DATA_BYTES; i++) g_patch[i] = (uint8_t)~(i * 5u + 2u);
  memset(g_patch + TABLE_OFFSET, 0, PATCH_BYTES - TABLE_OFFSET);
}

static void set_header(uint32_t entries) {
  memset(g_header, 0, sizeof(g_header));
  put32(g_header, NCA_BUCKET_MAGIC);
  put32(g_header + 4, 1);
  put32(g_header + 8, entries);
}

/* Lays `count` specs into the table (as many sets as needed) with virtual
 * end `end`, and builds g_expected from them. */
static void build_specs(const Spec *specs, uint32_t count, uint64_t end) {
  fill_storages();
  const uint32_t sets = (count + ENTRIES_PER_SET - 1u) / ENTRIES_PER_SET;
  uint8_t *l1 = g_patch + TABLE_OFFSET;
  put32(l1 + 4, sets);
  put64(l1 + 8, end);
  for (uint32_t set = 0; set < sets; set++) {
    const uint32_t first = set * ENTRIES_PER_SET;
    const uint32_t n = count - first < ENTRIES_PER_SET ? count - first : ENTRIES_PER_SET;
    put64(l1 + NCA_BUCKET_HEADER_BYTES + 8u * set, specs[first].virt);
    uint8_t *node = g_patch + TABLE_OFFSET + NCA_BUCKET_NODE_BYTES * (1u + set);
    put32(node, set);
    put32(node + 4, n);
    put64(node + 8, first + n < count ? specs[first + n].virt : end);
    for (uint32_t i = 0; i < n; i++) put_entry(entry_at(set, i), &specs[first + i]);
  }
  for (uint32_t i = 0; i < count; i++) {
    const uint64_t stop = i + 1u < count ? specs[i + 1u].virt : end;
    const uint8_t *from = specs[i].storage == NCA_INDIRECT_STORAGE_BASE ? g_base : g_patch;
    memcpy(g_expected + specs[i].virt, from + specs[i].phys, (size_t)(stop - specs[i].virt));
  }
  set_header(count);
}

static void build(void) { build_specs(k_specs, ENTRY_COUNT, VIRTUAL_BYTES); }

static void check_read(const NCA_Indirect *ind, uint64_t offset, uint64_t size) {
  static uint8_t out[MANY_VIRTUAL];
  memset(out, 0xAA, sizeof(out));
  CHECK_OK(byte_source_read(&ind->source, offset, out, size));
  CHECK(memcmp(out, g_expected + offset, (size_t)size) == 0);
}

static void test_reads(void) {
  build();
  const Byte_Source base = byte_source_from_memory(g_base, sizeof(g_base));
  const Byte_Source patch = byte_source_from_memory(g_patch, sizeof(g_patch));
  uint32_t entries = 0;
  CHECK_OK(nca_indirect_measure(&base, &patch, TABLE_OFFSET, TABLE_BYTES, g_header, &entries));
  CHECK(entries == ENTRY_COUNT);

  Arena arena;
  CHECK(arena_create(&arena, nca_indirect_arena_bytes(entries)));
  NCA_Indirect ind;
  CHECK_OK(nca_indirect_open(&ind, &base, &patch, TABLE_OFFSET, TABLE_BYTES, g_header, &arena));
  CHECK(ind.source.size == VIRTUAL_BYTES && ind.entry_count == ENTRY_COUNT);
  CHECK(ind.entries[3].storage == NCA_INDIRECT_STORAGE_PATCH && ind.entries[3].physical_offset == 0x200);

  check_read(&ind, 0, VIRTUAL_BYTES);           /* the whole view: every entry */
  for (uint32_t i = 1; i < ENTRY_COUNT; i++) {  /* each boundary */
    check_read(&ind, k_specs[i].virt, 1);       /* first byte of an entry */
    check_read(&ind, k_specs[i].virt - 1u, 1);  /* last byte of the one before */
    check_read(&ind, k_specs[i].virt - 8u, 16); /* straddling it */
  }
  check_read(&ind, 0x3F0, 0x1000); /* spans four entries, both storages */
  check_read(&ind, VIRTUAL_BYTES - 1u, 1);
  uint8_t byte = 0;
  CHECK_CODE(byte_source_read(&ind.source, VIRTUAL_BYTES, &byte, 1), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(byte_source_read(&ind.source, VIRTUAL_BYTES - 4u, &byte, 8), RESULT_INVALID_ARGUMENT);

  /* The data region slice the loader cuts from the patched view. */
  Byte_Source_Slice romfs;
  CHECK_OK(byte_source_slice(&ind.source, 0x500, 0x1000, &romfs));
  uint8_t out[0x1000];
  CHECK_OK(byte_source_read(&romfs.source, 0, out, sizeof(out)));
  CHECK(memcmp(out, g_expected + 0x500, sizeof(out)) == 0);
  arena_destroy(&arena);

  /* Too small an arena. */
  CHECK(arena_create(&arena, sizeof(NCA_Indirect_Entry)));
  CHECK_CODE(nca_indirect_open(&ind, &base, &patch, TABLE_OFFSET, TABLE_BYTES, g_header, &arena),
             RESULT_OUT_OF_MEMORY);
  arena_destroy(&arena);
}

static Spec g_many[MANY_ENTRIES];

static void test_two_sets(void) {
  for (uint32_t i = 0; i < MANY_ENTRIES; i++) {
    const bool from_patch = (i % 3u) == 1u;
    g_many[i] = (Spec){
        .virt = (uint64_t)i * MANY_SPAN,
        .phys = from_patch ? (i * 7u % (PATCH_DATA_BYTES / MANY_SPAN)) * MANY_SPAN
                           : (i * 11u % (BASE_BYTES / MANY_SPAN)) * MANY_SPAN,
        .storage = from_patch ? NCA_INDIRECT_STORAGE_PATCH : NCA_INDIRECT_STORAGE_BASE,
    };
  }
  build_specs(g_many, MANY_ENTRIES, MANY_VIRTUAL);
  const Byte_Source base = byte_source_from_memory(g_base, sizeof(g_base));
  const Byte_Source patch = byte_source_from_memory(g_patch, sizeof(g_patch));
  const uint64_t table = 3u * NCA_BUCKET_NODE_BYTES;
  uint32_t entries = 0;
  CHECK_OK(nca_indirect_measure(&base, &patch, TABLE_OFFSET, table, g_header, &entries));
  CHECK(entries == MANY_ENTRIES);
  Arena arena;
  CHECK(arena_create(&arena, nca_indirect_arena_bytes(entries)));
  NCA_Indirect ind;
  CHECK_OK(nca_indirect_open(&ind, &base, &patch, TABLE_OFFSET, table, g_header, &arena));
  check_read(&ind, 0, MANY_VIRTUAL);
  const uint64_t set_edge = (uint64_t)ENTRIES_PER_SET * MANY_SPAN;
  check_read(&ind, set_edge - 5u, 10); /* across the entry-set boundary */
  for (uint32_t i = 0; i < MANY_ENTRIES; i += 37u) check_read(&ind, (uint64_t)i * MANY_SPAN + 3u, 2u * MANY_SPAN);
  check_read(&ind, MANY_VIRTUAL - 3u, 3);
  arena_destroy(&arena);
}

static void expect_reject(const char *what) {
  const Byte_Source base = byte_source_from_memory(g_base, sizeof(g_base));
  const Byte_Source patch = byte_source_from_memory(g_patch, sizeof(g_patch));
  Arena arena;
  CHECK(arena_create(&arena, nca_indirect_arena_bytes(ENTRY_COUNT)));
  NCA_Indirect ind;
  const Error err = nca_indirect_open(&ind, &base, &patch, TABLE_OFFSET, TABLE_BYTES, g_header, &arena);
  if (err.code != RESULT_INVALID_ARGUMENT) fprintf(stderr, "[nca_indirect_test] not rejected: %s\n", what);
  CHECK_CODE(err, RESULT_INVALID_ARGUMENT);
  arena_destroy(&arena);
  build();
}

static void test_rejections(void) {
  build();
  g_header[0] ^= 1u;
  expect_reject("bad magic");
  put32(g_header + 4, 2);
  expect_reject("bad version");
  put32(g_header + 8, 0);
  expect_reject("no entries");
  put32(g_patch + TABLE_OFFSET + 4, 2);
  expect_reject("L1 set count disagrees");
  put64(entry_at(0, 2), 0x300); /* entry 2 before entry 1 */
  expect_reject("entries out of order");
  put64(entry_at(0, 0), 0x10);
  expect_reject("table not starting at 0");
  put32(entry_at(0, 1) + 16, 2);
  expect_reject("unknown storage index");
  put64(entry_at(0, 2) + 8, BASE_BYTES - 0x100u); /* 0x800 span past the base */
  expect_reject("base entry out of bounds");
  put64(entry_at(0, 3) + 8, PATCH_BYTES); /* patch entry past the patch */
  expect_reject("patch entry out of bounds");
  put64(g_patch + TABLE_OFFSET + 8, BASE_BYTES); /* last entry runs past the base */
  expect_reject("virtual end past the last entry's storage");
  put64(g_patch + TABLE_OFFSET + 8, 0x1000); /* end before the last entry */
  expect_reject("virtual end before the last entry");
  put32(g_patch + TABLE_OFFSET + NCA_BUCKET_NODE_BYTES + 4, ENTRY_COUNT + 1u);
  expect_reject("entry set count exceeds the header");

  /* Table placement: outside the patch, or smaller than its nodes. */
  const Byte_Source base = byte_source_from_memory(g_base, sizeof(g_base));
  const Byte_Source patch = byte_source_from_memory(g_patch, sizeof(g_patch));
  Arena arena;
  CHECK(arena_create(&arena, nca_indirect_arena_bytes(ENTRY_COUNT)));
  NCA_Indirect ind;
  CHECK_CODE(nca_indirect_open(&ind, &base, &patch, PATCH_BYTES, TABLE_BYTES, g_header, &arena),
             RESULT_INVALID_ARGUMENT);
  CHECK_CODE(nca_indirect_open(&ind, &base, &patch, TABLE_OFFSET, NCA_BUCKET_NODE_BYTES, g_header, &arena),
             RESULT_INVALID_ARGUMENT);
  CHECK_CODE(nca_indirect_open(&ind, NULL, &patch, TABLE_OFFSET, TABLE_BYTES, g_header, &arena),
             RESULT_INVALID_ARGUMENT);
  CHECK_OK(nca_indirect_open(&ind, &base, &patch, TABLE_OFFSET, TABLE_BYTES, g_header, &arena));
  arena_destroy(&arena);
}

int main(void) {
  test_reads();
  test_two_sets();
  test_rejections();
  printf("[nca_indirect_test] all tests passed\n");
  return 0;
}
