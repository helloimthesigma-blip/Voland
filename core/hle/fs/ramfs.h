/**
 * ramfs - an in-memory hierarchical filesystem (§15): the backing store
 * for the emulated SD card and save data until the platform persistence
 * hooks (OPFS on the web, a host directory natively) land. One pool holds
 * every filesystem's nodes and data blocks; a filesystem is a root
 * directory node in it. The pool lives with the Emulator, so the SD card
 * and saves survive a program reload within a session.
 *
 * Storage: fixed-size data blocks chained FAT-style (`next[]`), carved
 * from one arena reserved at init - no allocation after that (§3). Nodes
 * live in a fixed table; names are limited to RAMFS_NAME_BYTES - 1.
 *
 * Paths are Horizon fs paths: absolute, '/'-separated; empty components
 * and "." are ignored, ".." climbs (never above the root).
 *
 * Errors are Horizon fs Results (module 2) so the service can return them
 * as-is.
 */
#ifndef SWITCH_HLE_FS_RAMFS_H
#define SWITCH_HLE_FS_RAMFS_H

#include <stdbool.h>
#include <stdint.h>

#include "common/arena.h"

#define RAMFS_MAX_NODES 8192u
#define RAMFS_NAME_BYTES 256u
#define RAMFS_BLOCK_BYTES 0x4000u
#define RAMFS_NO_NODE UINT32_MAX
#define RAMFS_NO_BLOCK UINT32_MAX

/* Horizon fs results (module 2). */
#define FS_MODULE 2u
#define FS_RESULT(desc) ((uint32_t)(((desc) << 9) | FS_MODULE))
#define FS_RESULT_PATH_NOT_FOUND FS_RESULT(1u)
#define FS_RESULT_PATH_ALREADY_EXISTS FS_RESULT(2u)
#define FS_RESULT_TARGET_LOCKED FS_RESULT(7u)
#define FS_RESULT_DIRECTORY_NOT_EMPTY FS_RESULT(8u)
#define FS_RESULT_USABLE_SPACE_NOT_ENOUGH FS_RESULT(30u)
#define FS_RESULT_TARGET_NOT_FOUND FS_RESULT(1002u)
#define FS_RESULT_TOO_LONG_PATH FS_RESULT(6003u)
#define FS_RESULT_INVALID_PATH FS_RESULT(6004u)
#define FS_RESULT_OUT_OF_RANGE FS_RESULT(6061u)
#define FS_RESULT_INVALID_OPEN_MODE FS_RESULT(6072u)
#define FS_RESULT_UNSUPPORTED_OPERATION FS_RESULT(6300u)
#define FS_RESULT_ALLOCATION_TABLE_FULL FS_RESULT(4613u)
#define FS_RESULT_NEED_APPEND FS_RESULT(6201u) /* write past end without OpenMode_Append */

typedef enum Ramfs_Type { RAMFS_DIRECTORY = 0, RAMFS_FILE = 1 } Ramfs_Type; /* = DirectoryEntryType */

typedef struct Ramfs_Node {
  bool used;
  bool is_dir;
  uint32_t parent;
  uint32_t first_child;   /* directories */
  uint32_t next_sibling;
  uint32_t first_block;   /* files */
  uint64_t size;
  uint32_t open_count;    /* open IFile objects (deletion is refused while open) */
  char name[RAMFS_NAME_BYTES];
} Ramfs_Node;

typedef struct Ramfs_Pool {
  Arena arena;
  uint8_t *blocks;
  uint32_t *next_block;
  uint32_t block_count;
  uint32_t free_block_head;
  uint32_t free_block_count;
  Ramfs_Node *nodes; /* RAMFS_MAX_NODES, in the arena */
} Ramfs_Pool;

/* Reserves `capacity` bytes of data blocks. False if the arena fails. */
bool ramfs_pool_init(Ramfs_Pool *pool, uint64_t capacity);
void ramfs_pool_destroy(Ramfs_Pool *pool);

/* A new empty filesystem; its root node id in *root. */
uint32_t ramfs_create_filesystem(Ramfs_Pool *pool, uint32_t *root);
/* Frees a whole filesystem (root included). */
void ramfs_destroy_filesystem(Ramfs_Pool *pool, uint32_t root);

uint32_t ramfs_lookup(const Ramfs_Pool *pool, uint32_t root, const char *path, uint32_t *node);
uint32_t ramfs_create_file(Ramfs_Pool *pool, uint32_t root, const char *path, uint64_t size);
uint32_t ramfs_create_directory(Ramfs_Pool *pool, uint32_t root, const char *path);
uint32_t ramfs_delete_file(Ramfs_Pool *pool, uint32_t root, const char *path);
/* recursive: delete contents too; clean_only: empty it but keep it. */
uint32_t ramfs_delete_directory(Ramfs_Pool *pool, uint32_t root, const char *path, bool recursive, bool clean_only);
uint32_t ramfs_rename(Ramfs_Pool *pool, uint32_t root, const char *from, const char *to, bool directory);

uint32_t ramfs_read(Ramfs_Pool *pool, uint32_t node, uint64_t offset, void *out, uint64_t size, uint64_t *read);
uint32_t ramfs_write(Ramfs_Pool *pool, uint32_t node, uint64_t offset, const void *src, uint64_t size);
uint32_t ramfs_set_size(Ramfs_Pool *pool, uint32_t node, uint64_t size);

uint64_t ramfs_free_bytes(const Ramfs_Pool *pool);
uint64_t ramfs_total_bytes(const Ramfs_Pool *pool);

#endif /* SWITCH_HLE_FS_RAMFS_H */
