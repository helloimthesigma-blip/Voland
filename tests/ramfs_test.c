/**
 * ramfs (hle/fs/ramfs.h): paths, files across block boundaries, sizing,
 * directories, rename, locking, space accounting and Horizon results.
 */
#define CHECK_NAME "ramfs_test"
#include "check.h"

#include "hle/fs/ramfs.h"

#include <string.h>

static Ramfs_Pool g_pool;
static uint8_t g_buf[3 * RAMFS_BLOCK_BYTES];

int main(void) {
  CHECK(ramfs_pool_init(&g_pool, 16u * RAMFS_BLOCK_BYTES));
  uint32_t root = 0, other = 0, node = 0;
  CHECK(ramfs_create_filesystem(&g_pool, &root) == 0);
  CHECK(ramfs_create_filesystem(&g_pool, &other) == 0);

  /* Directories and paths. */
  CHECK(ramfs_create_directory(&g_pool, root, "/switch") == 0);
  CHECK(ramfs_create_directory(&g_pool, root, "/switch") == FS_RESULT_PATH_ALREADY_EXISTS);
  CHECK(ramfs_create_directory(&g_pool, root, "/a/b") == FS_RESULT_PATH_NOT_FOUND);
  CHECK(ramfs_create_file(&g_pool, root, "//switch/./app.nro", 10) == 0);
  CHECK(ramfs_lookup(&g_pool, root, "/switch/../switch/app.nro", &node) == 0);
  CHECK(!g_pool.nodes[node].is_dir && g_pool.nodes[node].size == 10);
  CHECK(ramfs_lookup(&g_pool, other, "/switch/app.nro", &node) == FS_RESULT_PATH_NOT_FOUND); /* filesystems are separate */
  CHECK(ramfs_lookup(&g_pool, root, "/../..", &node) == 0 && node == root);

  /* Data across blocks; growth zero-fills; shrink then grow reads zeros. */
  CHECK(ramfs_lookup(&g_pool, root, "/switch/app.nro", &node) == 0);
  for (uint32_t i = 0; i < sizeof(g_buf); i++) g_buf[i] = (uint8_t)(i * 13u);
  CHECK(ramfs_write(&g_pool, node, RAMFS_BLOCK_BYTES - 100, g_buf, 2u * RAMFS_BLOCK_BYTES) == 0);
  CHECK(g_pool.nodes[node].size == 3u * RAMFS_BLOCK_BYTES - 100);
  static uint8_t back[3 * RAMFS_BLOCK_BYTES];
  uint64_t read = 0;
  CHECK(ramfs_read(&g_pool, node, RAMFS_BLOCK_BYTES - 100, back, sizeof(back), &read) == 0);
  CHECK(read == 2u * RAMFS_BLOCK_BYTES && memcmp(back, g_buf, read) == 0);
  CHECK(ramfs_read(&g_pool, node, 0, back, 10, &read) == 0 && read == 10 && back[0] == 0 && back[9] == 0);
  CHECK(ramfs_set_size(&g_pool, node, 5) == 0);
  CHECK(ramfs_set_size(&g_pool, node, RAMFS_BLOCK_BYTES) == 0);
  CHECK(ramfs_read(&g_pool, node, 0, back, RAMFS_BLOCK_BYTES, &read) == 0 && back[5] == 0 && back[RAMFS_BLOCK_BYTES - 1] == 0);
  CHECK(ramfs_read(&g_pool, node, RAMFS_BLOCK_BYTES + 1, back, 1, &read) == FS_RESULT_OUT_OF_RANGE);

  /* Space: 16 blocks; a 1-block file holds one. */
  CHECK(ramfs_free_bytes(&g_pool) == 15u * RAMFS_BLOCK_BYTES);
  CHECK(ramfs_create_file(&g_pool, root, "/big", 16u * RAMFS_BLOCK_BYTES) == FS_RESULT_USABLE_SPACE_NOT_ENOUGH);
  CHECK(ramfs_lookup(&g_pool, root, "/big", &node) == FS_RESULT_PATH_NOT_FOUND); /* no half-made file */

  /* Rename, then directory deletion rules. */
  CHECK(ramfs_rename(&g_pool, root, "/switch/app.nro", "/app2.nro", false) == 0);
  CHECK(ramfs_lookup(&g_pool, root, "/app2.nro", &node) == 0);
  CHECK(ramfs_rename(&g_pool, root, "/switch", "/switch/inner", true) == FS_RESULT_INVALID_PATH);
  CHECK(ramfs_create_file(&g_pool, root, "/switch/x", 0) == 0);
  CHECK(ramfs_delete_directory(&g_pool, root, "/switch", false, false) == FS_RESULT_DIRECTORY_NOT_EMPTY);
  g_pool.nodes[node].open_count = 1;
  CHECK(ramfs_delete_file(&g_pool, root, "/app2.nro") == FS_RESULT_TARGET_LOCKED);
  g_pool.nodes[node].open_count = 0;
  CHECK(ramfs_delete_file(&g_pool, root, "/app2.nro") == 0);
  CHECK(ramfs_delete_directory(&g_pool, root, "/switch", true, true) == 0); /* clean: keeps the dir */
  CHECK(ramfs_lookup(&g_pool, root, "/switch", &node) == 0 && g_pool.nodes[node].first_child == RAMFS_NO_NODE);
  CHECK(ramfs_delete_directory(&g_pool, root, "/switch", false, false) == 0);
  CHECK(ramfs_free_bytes(&g_pool) == ramfs_total_bytes(&g_pool));
  ramfs_destroy_filesystem(&g_pool, root);
  ramfs_destroy_filesystem(&g_pool, other);
  ramfs_pool_destroy(&g_pool);
  printf("[ramfs_test] passed\n");
  return 0;
}
