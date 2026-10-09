/**
 * The AArch32 SVC calling convention (hle/kernel/svc32.h) and the
 * synthesized system data archives (hle/fs/system_data.h).
 */
#include "check.h"

#include <string.h>

#include "common/arena.h"
#include "hle/fs/ramfs.h"
#include "hle/fs/system_data.h"
#include "hle/kernel/svc32.h"
#include "hle/loader/romfs.h"

static void set(CPU_Register_File *r, const uint32_t *v, uint32_t n) {
  memset(r, 0, sizeof(*r));
  for (uint32_t i = 0; i < n; i++) r->x[i] = v[i];
}

static void test_get_info(void) {
  /* GetInfo: r1 id, r2 handle, r0:r3 sub-id -> x1 id, x2 handle, x3 sub-id;
   * out x1 (64-bit) -> r1:r2, r3 back as it was. */
  CPU_Register_File r;
  const uint32_t in[] = {0x11111111u, 2u, 0xFFFF8001u, 0x22222222u};
  set(&r, in, 4);
  Svc32_Frame frame;
  svc32_enter(&r, 0x29, &frame);
  CHECK(r.x[1] == 2u && r.x[2] == 0xFFFF8001u && r.x[3] == 0x2222222211111111ull);
  r.x[0] = 0;
  r.x[1] = 0x123456789ABCDEF0ull;
  svc32_exit(&r, 0x29, &frame);
  CHECK(r.x[0] == 0 && r.x[1] == 0x9ABCDEF0u && r.x[2] == 0x12345678u && r.x[3] == 0x22222222u);
}

static void test_create_thread(void) {
  /* CreateThread: r0 priority, r1 entry, r2 arg, r3 stack, r4 core ->
   * x4 priority, x5 core; r4/r5 (callee-saved) restored after. */
  CPU_Register_File r;
  const uint32_t in[] = {44u, 0x1000u, 0x2000u, 0x3000u, 0xFFFFFFFEu, 0x55u};
  set(&r, in, 6);
  Svc32_Frame frame;
  svc32_enter(&r, 0x08, &frame);
  CHECK(r.x[1] == 0x1000u && r.x[2] == 0x2000u && r.x[3] == 0x3000u && r.x[4] == 44u && r.x[5] == 0xFFFFFFFEu);
  r.x[0] = 0;
  r.x[1] = 0x1A0030u; /* the handle */
  svc32_exit(&r, 0x08, &frame);
  CHECK(r.x[1] == 0x1A0030u && r.x[4] == 0xFFFFFFFEu && r.x[5] == 0x55u);
}

static void test_timeouts_and_ticks(void) {
  CPU_Register_File r;
  Svc32_Frame frame;
  /* WaitSynchronization: timeout r0:r3 -> x3. */
  const uint32_t wait[] = {0xFFFFFFFFu, 0x8000u, 1u, 0xFFFFFFFFu};
  set(&r, wait, 4);
  svc32_enter(&r, 0x18, &frame);
  CHECK(r.x[3] == UINT64_MAX && r.x[1] == 0x8000u && r.x[2] == 1u);
  svc32_exit(&r, 0x18, &frame);
  CHECK(r.x[3] == 0xFFFFFFFFu);
  /* SleepThread: r0:r1 nanoseconds -> x0. */
  const uint32_t sleep[] = {0x89ABCDEFu, 0x1u};
  set(&r, sleep, 2);
  svc32_enter(&r, 0x0B, &frame);
  CHECK(r.x[0] == 0x189ABCDEFull);
  /* GetSystemTick: x0 -> r0:r1. */
  set(&r, sleep, 0);
  svc32_enter(&r, 0x1E, &frame);
  r.x[0] = 0x0000000500000007ull;
  svc32_exit(&r, 0x1E, &frame);
  CHECK(r.x[0] == 7u && r.x[1] == 5u);
  /* A call with the same registers in both conventions passes through. */
  const uint32_t close[] = {0x18002u, 0xAAu};
  set(&r, close, 2);
  svc32_enter(&r, 0x16, &frame);
  CHECK(r.x[0] == 0x18002u && r.x[1] == 0xAAu);
}

static void test_system_data(void) {
  CHECK(!system_data_open(0x0100000000000800ull, &(Byte_Source){0}));
  Byte_Source src;
  CHECK(system_data_open(SYSTEM_DATA_MII_MODEL, &src));
  Arena arena;
  CHECK(arena_create(&arena, 1u << 16));
  RomFS fs;
  CHECK_OK(romfs_open(&src, &arena, &fs));
  static const char *names[] = {"NXTextureLowLinear.dat", "NXTextureLowSRGB.dat", "NXTextureMidLinear.dat",
                                "NXTextureMidSRGB.dat", "NXShapeHigh.dat", "NXShapeMid.dat"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    RomFS_File_Entry e;
    CHECK_OK(romfs_find_file(&fs, names[i], &e));
    uint8_t header[8];
    CHECK(e.data_size == 16u);
    CHECK_OK(romfs_read_file(&fs, &e, 0, header, sizeof(header)));
    CHECK(!memcmp(header, i < 4u ? "NFTR" : "NFSR", 4u) && header[4] == 1u);
  }
  RomFS_File_Entry missing;
  CHECK(romfs_find_file(&fs, "NXShapeLow.dat", &missing).code == RESULT_NOT_FOUND);
  arena_destroy(&arena);
}

/* The user's own archive on the SD card wins: a bare RomFS here. */
static void test_user_system_data(void) {
  Ramfs_Pool pool;
  CHECK(ramfs_pool_init(&pool, 1u << 20));
  uint32_t root = RAMFS_NO_NODE;
  CHECK(ramfs_create_filesystem(&pool, &root) == 0);
  Byte_Source src;
  CHECK(!system_data_open_user(&pool, root, SYSTEM_DATA_MII_MODEL, &src));
  static const uint8_t body[] = "user data";
  const System_Data_File files[] = {{"Only.dat", body, sizeof(body)}};
  static uint8_t image[1024];
  const uint64_t size = system_data_build_romfs(files, 1, image, sizeof(image));
  CHECK(size > 0);
  CHECK(ramfs_create_directory(&pool, root, "/systemdata") == 0);
  CHECK(ramfs_create_file(&pool, root, "/systemdata/0100000000000802.romfs", size) == 0);
  uint32_t node = RAMFS_NO_NODE;
  CHECK(ramfs_lookup(&pool, root, "/systemdata/0100000000000802.romfs", &node) == 0);
  CHECK(ramfs_write(&pool, node, 0, image, size) == 0);
  uint64_t id = 1;
  const Byte_Source image_source = byte_source_from_memory(image, size);
  CHECK(system_data_identify(&image_source, &id) == SYSTEM_DATA_ROMFS && id == 0);
  static const uint8_t junk[0x80] = {1, 2, 3};
  const Byte_Source junk_source = byte_source_from_memory(junk, sizeof(junk));
  CHECK(system_data_identify(&junk_source, &id) == SYSTEM_DATA_UNKNOWN);
  CHECK(system_data_open_user(&pool, root, SYSTEM_DATA_MII_MODEL, &src));
  CHECK(src.size == size);
  Arena arena;
  CHECK(arena_create(&arena, 1u << 16));
  RomFS fs;
  CHECK_OK(romfs_open(&src, &arena, &fs));
  RomFS_File_Entry e;
  CHECK_OK(romfs_find_file(&fs, "Only.dat", &e));
  CHECK(e.data_size == sizeof(body));
  arena_destroy(&arena);
  ramfs_pool_destroy(&pool);
}

int main(void) {
  test_user_system_data();
  test_get_info();
  test_create_thread();
  test_timeouts_and_ticks();
  test_system_data();
  return 0;
}
