/**
 * Unit tests for core/hle/loader/mods.c: the file set, LayeredFS over a
 * fixture RomFS, and the IPS / IPS32 / pchtxt patch formats.
 */
#define CHECK_NAME "mods_test"
#include "check.h"

#include "common/arena.h"
#include "hle/loader/mods.h"
#include "hle/loader/romfs.h"
#include "loader_fixtures.h"

#include <string.h>

static const char k_link[] = "LINK-BFRES";
static const char k_zelda[] = "ZELDA-BFRES";
static const char k_readme[] = "readme at root";

static const Fixture_RomFS_Entry k_base[] = {
    {"/readme.txt", false, k_readme, sizeof(k_readme) - 1},
    {"/actor", true, NULL, 0},
    {"/actor/Link.bfres", false, k_link, sizeof(k_link) - 1},
    {"/actor/Zelda.bfres", false, k_zelda, sizeof(k_zelda) - 1},
};
#define BASE_COUNT (sizeof(k_base) / sizeof(k_base[0]))

/* Mod file contents, by id. */
static const char *const k_mod_data[] = {
    "MODDED LINK, LONGER THAN THE ORIGINAL",
    "a brand new file",
    "deep new file",
    "lower priority link",
};

static Error mod_read(void *user, uint32_t id, uint64_t offset, void *out, uint64_t size) {
  (void)user;
  if (id >= sizeof(k_mod_data) / sizeof(k_mod_data[0])) return ERR(RESULT_IO_ERROR, "bad id");
  const char *data = k_mod_data[id];
  if (offset + size > strlen(data)) return ERR(RESULT_IO_ERROR, "short");
  memcpy(out, data + offset, (size_t)size);
  return OK;
}

static void add(Mod_Set *set, const char *path, uint32_t id) {
  CHECK_OK(mod_set_add(set, path, strlen(k_mod_data[id]), id));
}

static void check_file(const RomFS *fs, const char *path, const char *expected) {
  RomFS_File_Entry entry;
  CHECK_OK(romfs_find_file(fs, path, &entry));
  CHECK(entry.data_size == strlen(expected));
  char out[64] = {0};
  CHECK_OK(romfs_read_file(fs, &entry, 0, out, entry.data_size));
  CHECK(memcmp(out, expected, entry.data_size) == 0);
}

static void test_set_rules(void) {
  Mod_Set set;
  memset(&set, 0, sizeof(set));
  mod_set_bind(&set, mod_read, NULL);
  add(&set, "/romfs/actor/Link.bfres", 0);
  add(&set, "romfs\\actor\\Link.bfres", 3); /* same path, lower priority: ignored */
  CHECK(set.count == 1);
  CHECK(mod_set_find(&set, "romfs/actor/Link.bfres")->id == 0);
  CHECK_CODE(mod_set_add(&set, "romfs/../x", 1, 1), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(mod_set_add(&set, "romfs//x", 1, 1), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(mod_set_add(&set, "", 1, 1), RESULT_INVALID_ARGUMENT);
  CHECK(mod_set_count_prefix(&set, "romfs/") == 1);
  mod_set_clear(&set);
  CHECK(set.count == 0 && mod_set_find(&set, "romfs/actor/Link.bfres") == NULL);
}

static void test_layeredfs(void) {
  Fixture_Buffer image;
  fixture_build_romfs(k_base, BASE_COUNT, 0, &image);
  const Byte_Source base = byte_source_from_memory(image.bytes, image.size);

  Mod_Set set;
  memset(&set, 0, sizeof(set));
  mod_set_bind(&set, mod_read, NULL);
  add(&set, "romfs/actor/Link.bfres", 0);
  add(&set, "romfs/new.txt", 1);
  add(&set, "romfs/a/b/c/deep.bin", 2);
  add(&set, "exefs/main", 3); /* not RomFS: ignored by LayeredFS */

  Arena arena;
  CHECK(arena_create(&arena, 1024 * 1024));
  Layered_RomFS layered;
  CHECK_OK(layeredfs_build(&base, &set, &arena, &layered));
  CHECK(layered.replaced == 1 && layered.added == 2);

  /* The merged image is a valid RomFS: the reader parses it. */
  Arena read_arena;
  CHECK(arena_create(&read_arena, 1024 * 1024));
  RomFS fs;
  CHECK_OK(romfs_open(&layered.source, &read_arena, &fs));
  check_file(&fs, "/readme.txt", k_readme);
  check_file(&fs, "/actor/Zelda.bfres", k_zelda);
  check_file(&fs, "/actor/Link.bfres", k_mod_data[0]);
  check_file(&fs, "/new.txt", k_mod_data[1]);
  check_file(&fs, "/a/b/c/deep.bin", k_mod_data[2]);
  RomFS_Dir_Entry dir;
  CHECK_OK(romfs_find_dir(&fs, "/a/b", &dir));
  CHECK_CODE(romfs_find_file(&fs, "/main", &(RomFS_File_Entry){0}), RESULT_NOT_FOUND);

  /* Enumeration sees each actor file once. */
  CHECK_OK(romfs_find_dir(&fs, "/actor", &dir));
  uint32_t files = 0;
  for (uint32_t off = dir.first_child_file; off != ROMFS_NO_ENTRY; files++) {
    RomFS_File_Entry e;
    CHECK_OK(romfs_file_entry(&fs, off, &e));
    off = e.next_sibling;
  }
  CHECK(files == 2);

  /* No base: the mods alone form the image. */
  arena_reset(&arena);
  arena_reset(&read_arena);
  CHECK_OK(layeredfs_build(NULL, &set, &arena, &layered));
  CHECK_OK(romfs_open(&layered.source, &read_arena, &fs));
  check_file(&fs, "/new.txt", k_mod_data[1]);

  /* No RomFS files at all: NOT_FOUND, the caller keeps the base. */
  Mod_Set empty;
  memset(&empty, 0, sizeof(empty));
  CHECK_CODE(layeredfs_build(&base, &empty, &arena, &layered), RESULT_NOT_FOUND);

  arena_destroy(&read_arena);
  arena_destroy(&arena);
  mod_set_clear(&set);
  fixture_buffer_free(&image);
}

static const uint8_t k_id[32] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

static void test_build_ids(void) {
  CHECK(mods_build_id_matches("DEADBEEF01020304", 16, k_id));
  CHECK(mods_build_id_matches("deadbeef0102030405060700000000000000000000.ips", 46, k_id));
  CHECK(!mods_build_id_matches("DEADBEEF01020305", 16, k_id));
  CHECK(!mods_build_id_matches("DEADBEEF", 8, k_id)); /* too short to be an id */
}

static void test_ips(void) {
  uint8_t image[0x40];
  memset(image, 0, sizeof(image));
  /* Offsets count from the NSO file start: 0x110 is image byte 0x10. */
  const uint8_t ips[] = {'P', 'A', 'T', 'C', 'H',
                         0x00, 0x01, 0x10, 0x00, 0x02, 0xAA, 0xBB,       /* 2 bytes at 0x10 */
                         0x00, 0x01, 0x20, 0x00, 0x00, 0x00, 0x04, 0xCC, /* RLE 4 x CC at 0x20 */
                         0x00, 0x00, 0x10, 0x00, 0x01, 0x99,             /* inside the header: skipped */
                         0x00, 0x09, 0x00, 0x00, 0x01, 0x77,             /* past the image: skipped */
                         'E', 'O', 'F'};
  CHECK(mods_apply_patch(ips, sizeof(ips), k_id, image, sizeof(image)) == 2);
  CHECK(image[0x10] == 0xAA && image[0x11] == 0xBB && image[0x12] == 0);
  CHECK(image[0x20] == 0xCC && image[0x23] == 0xCC && image[0x24] == 0);

  memset(image, 0, sizeof(image));
  const uint8_t ips32[] = {'I', 'P', 'S', '3', '2', 0x00, 0x00, 0x01, 0x30, 0x00, 0x01, 0x55, 'E', 'E', 'O', 'F'};
  CHECK(mods_apply_patch(ips32, sizeof(ips32), k_id, image, sizeof(image)) == 1);
  CHECK(image[0x30] == 0x55);
}

static void test_pchtxt(void) {
  uint8_t image[0x40];
  memset(image, 0, sizeof(image));
  const char text[] =
      "@nsobid-DEADBEEF0102030405060700\n"
      "@flag offset_shift 0x100\n"
      "// a comment\n"
      "@enabled\n"
      "00000004 11223344\n"
      "00000010 \"Hi\"\n"
      "@disabled\n"
      "00000020 FFFF\n"
      "@enabled\n"
      "00000030 AB\r\n"
      "@stop\n"
      "00000038 EE\n";
  CHECK(mods_apply_patch((const uint8_t *)text, strlen(text), k_id, image, sizeof(image)) == 3);
  CHECK(image[4] == 0x11 && image[7] == 0x44);
  CHECK(image[0x10] == 'H' && image[0x11] == 'i');
  CHECK(image[0x20] == 0 && image[0x30] == 0xAB && image[0x38] == 0);

  /* Another module's id: nothing applies. */
  const uint8_t other[32] = {1};
  memset(image, 0, sizeof(image));
  CHECK(mods_apply_patch((const uint8_t *)text, strlen(text), other, image, sizeof(image)) == 0);
}

static Error patch_read(void *user, uint32_t id, uint64_t offset, void *out, uint64_t size) {
  (void)id;
  memcpy(out, (const uint8_t *)user + offset, (size_t)size);
  return OK;
}

static void test_patch_module(void) {
  const uint8_t ips[] = {'P', 'A', 'T', 'C', 'H', 0x00, 0x01, 0x00, 0x00, 0x01, 0x42, 'E', 'O', 'F'};
  Mod_Set set;
  memset(&set, 0, sizeof(set));
  mod_set_bind(&set, patch_read, (void *)ips);
  CHECK_OK(mod_set_add(&set, "exefs/DEADBEEF01020304.ips", sizeof(ips), 0));
  CHECK_OK(mod_set_add(&set, "exefs/0000000000000000.ips", sizeof(ips), 1));
  Arena scratch;
  CHECK(arena_create(&scratch, 64 * 1024));
  uint8_t image[16] = {0};
  CHECK(mods_patch_module(&set, "main", k_id, image, sizeof(image), &scratch) == 1);
  CHECK(image[0] == 0x42);
  CHECK(scratch.used_bytes == 0);
  arena_destroy(&scratch);
  mod_set_clear(&set);
}

static void expect_norm(const char *path, const char *expected) {
  char out[MODS_MAX_PATH_BYTES];
  const bool ok = mods_normalize_path("MyMod", path, 0x0100A12300000000ull, out, sizeof(out));
  if (!expected) {
    CHECK(!ok);
    return;
  }
  CHECK(ok);
  CHECK(strcmp(out, expected) == 0);
}

static void test_normalize(void) {
  expect_norm("atmosphere/contents/0100A12300000000/romfs/Data/Map.bin", "romfs/Data/Map.bin");
  expect_norm("atmosphere/contents/0100FFFF00000000/romfs/Data/Map.bin", NULL); /* other title */
  expect_norm("ROMFS/Data/Map.bin", "romfs/Data/Map.bin");
  expect_norm("0100A12300000000/exefs/main", "exefs/main");
  expect_norm("exefs/subsdk9", "exefs/subsdk9");
  expect_norm("atmosphere/exefs_patches/60fps/DEADBEEF01020304.ips", "exefs/MyMod/DEADBEEF01020304.ips");
  expect_norm("exefs/DEADBEEF01020304.ips", "exefs/MyMod/DEADBEEF01020304.ips");
  expect_norm("patches/60fps.pchtxt", "exefs/MyMod/60fps.pchtxt");
  expect_norm("atmosphere\\contents\\0100a12300000000\\cheats\\DEADBEEF01020304.txt", "cheats/MyMod/DEADBEEF01020304.txt");
  expect_norm("update/0100A12300000800.nca", "nca/MyMod/0100A12300000800.nca");
  expect_norm("romfs/Data/x.nca", "romfs/Data/x.nca"); /* a game file, not an update */
  expect_norm("readme.txt", NULL);
  expect_norm("romfs", NULL);
}

int main(void) {
  test_normalize();
  test_set_rules();
  test_layeredfs();
  test_build_ids();
  test_ips();
  test_pchtxt();
  test_patch_module();
  printf("[mods_test] all tests passed\n");
  return 0;
}
