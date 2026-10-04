/**
 * Save archives and commit snapshots (§15, hle/fs/save_archive.h,
 * fs_commit_save): a save tree round-trips through one archive, damaged
 * archives change nothing, and IFileSystem::Commit's snapshot holds the
 * committed state - later uncommitted writes do not reach it.
 */
#define CHECK_NAME "save_archive_test"
#include "check.h"

#include "hle/fs/save_archive.h"
#include "hle/services/fs/fs.h"

#include <string.h>

static Ramfs_Pool g_pool;
static uint8_t g_buffer[1u << 20];

typedef struct Buffer_Sink {
  uint64_t used;
} Buffer_Sink;

static bool buffer_write(void *user, const void *bytes, uint64_t size) {
  Buffer_Sink *b = (Buffer_Sink *)user;
  if (b->used + size > sizeof(g_buffer)) return false;
  memcpy(g_buffer + b->used, bytes, size);
  b->used += size;
  return true;
}

static void put_file(uint32_t root, const char *path, const char *text) {
  uint32_t node = 0;
  (void)ramfs_delete_file(&g_pool, root, path);
  CHECK(ramfs_create_file(&g_pool, root, path, 0) == 0);
  CHECK(ramfs_lookup(&g_pool, root, path, &node) == 0);
  CHECK(ramfs_write(&g_pool, node, 0, text, strlen(text)) == 0);
}

static bool file_is(uint32_t root, const char *path, const char *text) {
  uint32_t node = 0;
  char got[256] = {0};
  uint64_t read = 0;
  if (ramfs_lookup(&g_pool, root, path, &node)) return false;
  (void)ramfs_read(&g_pool, node, 0, got, sizeof(got) - 1u, &read);
  return read == strlen(text) && memcmp(got, text, read) == 0;
}

static uint64_t archive_of(uint32_t root) {
  Buffer_Sink sink = {0};
  const Save_Archive_Sink s = {&sink, buffer_write};
  return save_archive_write(&g_pool, root, &s);
}

static void test_round_trip(void) {
  uint32_t a = 0, b = 0;
  CHECK(ramfs_create_filesystem(&g_pool, &a) == 0 && ramfs_create_filesystem(&g_pool, &b) == 0);
  CHECK(ramfs_create_directory(&g_pool, a, "/slot1") == 0);
  CHECK(ramfs_create_directory(&g_pool, a, "/empty") == 0);
  put_file(a, "/slot1/user1.dat", "progress: bench 3");
  put_file(a, "/settings.dat", "volume 8");
  put_file(a, "/zero.dat", "");
  const uint64_t size = archive_of(a);
  CHECK(size > SAVE_ARCHIVE_HEADER_BYTES && save_archive_valid(g_buffer, size));
  put_file(b, "/stale.dat", "old");
  CHECK(save_archive_restore(&g_pool, b, g_buffer, size) == 0);
  CHECK(file_is(b, "/slot1/user1.dat", "progress: bench 3") && file_is(b, "/settings.dat", "volume 8"));
  CHECK(file_is(b, "/zero.dat", ""));
  uint32_t node = 0;
  CHECK(ramfs_lookup(&g_pool, b, "/empty", &node) == 0 && g_pool.nodes[node].is_dir);
  CHECK(ramfs_lookup(&g_pool, b, "/stale.dat", &node) != 0); /* replaced, not merged */
  /* Damaged archives are refused whole: nothing changes. */
  for (uint64_t cut = 1; cut < size; cut += 7u) CHECK(!save_archive_valid(g_buffer, size - cut));
  g_buffer[SAVE_ARCHIVE_HEADER_BYTES + 4] = 0xFF; /* a path length far past the end */
  CHECK(save_archive_restore(&g_pool, b, g_buffer, size) != 0);
  CHECK(file_is(b, "/slot1/user1.dat", "progress: bench 3"));
  CHECK(!save_archive_valid(g_buffer, 3));
}

static void test_commit_snapshot(void) {
  static Fs_State fs;
  fs_init(&fs, &g_pool);
  uint8_t key[FS_SAVE_ATTRIBUTE_BYTES];
  memset(key, 0, sizeof(key));
  key[0] = 0x13; /* program id low byte, say */
  uint32_t root = 0;
  CHECK(fs_save_root(&fs, 1, key, true, &root) == 0);
  put_file(root, "/save.dat", "committed");
  CHECK(fs.save_commits == 0);
  CHECK(fs_commit_save(&fs, root) == 0 && fs.save_commits == 1u);
  put_file(root, "/save.dat", "half-writ"); /* after the commit: not durable */
  char path[FS_SAVE_NAME_BYTES + 1u];
  path[0] = '/';
  fs_save_name(&fs.saves[0], path + 1);
  CHECK(strncmp(path, "/01-13", 6) == 0 && strlen(path) == FS_SAVE_NAME_BYTES);
  uint32_t node = 0;
  CHECK(ramfs_lookup(&g_pool, fs.committed_root, path, &node) == 0);
  uint64_t read = 0;
  CHECK(ramfs_read(&g_pool, node, 0, g_buffer, g_pool.nodes[node].size, &read) == 0);
  uint32_t check = 0;
  CHECK(ramfs_create_filesystem(&g_pool, &check) == 0);
  CHECK(save_archive_restore(&g_pool, check, g_buffer, read) == 0 && file_is(check, "/save.dat", "committed"));
  /* Committing the SD card (not a save) snapshots nothing. */
  CHECK(fs_commit_save(&fs, fs.sd_root) == 0 && fs.save_commits == 1u);
}

int main(void) {
  CHECK(ramfs_pool_init(&g_pool, 64u * RAMFS_BLOCK_BYTES));
  test_round_trip();
  test_commit_snapshot();
  printf("[save_archive_test] passed\n");
  return 0;
}
