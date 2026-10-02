#include "hle/fs/ramfs.h"

#include <string.h>

#define RAMFS_ARENA_SLACK 64u

bool ramfs_pool_init(Ramfs_Pool *pool, uint64_t capacity) {
  memset(pool, 0, sizeof(*pool));
  const uint32_t blocks = (uint32_t)(capacity / RAMFS_BLOCK_BYTES);
  const size_t bytes = (size_t)blocks * RAMFS_BLOCK_BYTES + (size_t)blocks * sizeof(uint32_t) +
                       RAMFS_MAX_NODES * sizeof(Ramfs_Node) + 3u * RAMFS_ARENA_SLACK;
  if (!arena_create(&pool->arena, bytes)) return false;
  pool->blocks = ARENA_ALLOC_ARRAY(&pool->arena, uint8_t, (size_t)blocks * RAMFS_BLOCK_BYTES);
  pool->next_block = ARENA_ALLOC_ARRAY(&pool->arena, uint32_t, blocks);
  pool->nodes = ARENA_ALLOC_ARRAY(&pool->arena, Ramfs_Node, RAMFS_MAX_NODES);
  if (!pool->blocks || !pool->next_block || !pool->nodes) {
    arena_destroy(&pool->arena);
    return false;
  }
  memset(pool->nodes, 0, RAMFS_MAX_NODES * sizeof(Ramfs_Node));
  pool->block_count = blocks;
  for (uint32_t i = 0; i < blocks; i++) pool->next_block[i] = i + 1u < blocks ? i + 1u : RAMFS_NO_BLOCK;
  pool->free_block_head = blocks ? 0 : RAMFS_NO_BLOCK;
  pool->free_block_count = blocks;
  return true;
}

void ramfs_pool_destroy(Ramfs_Pool *pool) {
  arena_destroy(&pool->arena);
  memset(pool, 0, sizeof(*pool));
}

/* ------------------------------------------------------------------ */
/* Nodes and blocks.                                                   */
/* ------------------------------------------------------------------ */

static uint32_t node_alloc(Ramfs_Pool *pool, bool is_dir, const char *name, size_t name_length) {
  for (uint32_t i = 0; i < RAMFS_MAX_NODES; i++) {
    Ramfs_Node *n = &pool->nodes[i];
    if (n->used) continue;
    memset(n, 0, sizeof(*n));
    n->used = true;
    n->is_dir = is_dir;
    n->parent = RAMFS_NO_NODE;
    n->first_child = RAMFS_NO_NODE;
    n->next_sibling = RAMFS_NO_NODE;
    n->first_block = RAMFS_NO_BLOCK;
    memcpy(n->name, name, name_length);
    return i;
  }
  return RAMFS_NO_NODE;
}

static void free_chain(Ramfs_Pool *pool, uint32_t block) {
  while (block != RAMFS_NO_BLOCK) {
    const uint32_t next = pool->next_block[block];
    pool->next_block[block] = pool->free_block_head;
    pool->free_block_head = block;
    pool->free_block_count++;
    block = next;
  }
}

static uint32_t block_alloc(Ramfs_Pool *pool) {
  const uint32_t block = pool->free_block_head;
  if (block == RAMFS_NO_BLOCK) return RAMFS_NO_BLOCK;
  pool->free_block_head = pool->next_block[block];
  pool->next_block[block] = RAMFS_NO_BLOCK;
  pool->free_block_count--;
  memset(pool->blocks + (uint64_t)block * RAMFS_BLOCK_BYTES, 0, RAMFS_BLOCK_BYTES);
  return block;
}

static uint32_t blocks_for(uint64_t size) { return (uint32_t)((size + RAMFS_BLOCK_BYTES - 1u) / RAMFS_BLOCK_BYTES); }

static void unlink_child(Ramfs_Pool *pool, uint32_t node) {
  Ramfs_Node *n = &pool->nodes[node];
  if (n->parent == RAMFS_NO_NODE) return;
  uint32_t *link = &pool->nodes[n->parent].first_child;
  while (*link != RAMFS_NO_NODE && *link != node) link = &pool->nodes[*link].next_sibling;
  if (*link == node) *link = n->next_sibling;
  n->parent = RAMFS_NO_NODE;
  n->next_sibling = RAMFS_NO_NODE;
}

static void link_child(Ramfs_Pool *pool, uint32_t dir, uint32_t node) {
  pool->nodes[node].parent = dir;
  pool->nodes[node].next_sibling = pool->nodes[dir].first_child;
  pool->nodes[dir].first_child = node;
}

static void free_subtree(Ramfs_Pool *pool, uint32_t node) {
  Ramfs_Node *n = &pool->nodes[node];
  uint32_t child = n->first_child;
  while (child != RAMFS_NO_NODE) {
    const uint32_t next = pool->nodes[child].next_sibling;
    free_subtree(pool, child);
    child = next;
  }
  free_chain(pool, n->first_block);
  memset(n, 0, sizeof(*n));
}

uint32_t ramfs_create_filesystem(Ramfs_Pool *pool, uint32_t *root) {
  const uint32_t node = node_alloc(pool, true, "", 0);
  if (node == RAMFS_NO_NODE) return FS_RESULT_ALLOCATION_TABLE_FULL;
  *root = node;
  return 0;
}

void ramfs_destroy_filesystem(Ramfs_Pool *pool, uint32_t root) {
  if (root < RAMFS_MAX_NODES && pool->nodes[root].used) free_subtree(pool, root);
}

/* ------------------------------------------------------------------ */
/* Paths.                                                              */
/* ------------------------------------------------------------------ */

static uint32_t find_child(const Ramfs_Pool *pool, uint32_t dir, const char *name, size_t length) {
  for (uint32_t c = pool->nodes[dir].first_child; c != RAMFS_NO_NODE; c = pool->nodes[c].next_sibling) {
    if (strlen(pool->nodes[c].name) == length && memcmp(pool->nodes[c].name, name, length) == 0) return c;
  }
  return RAMFS_NO_NODE;
}

/* Walks every component but the last. leaf/leaf_length name the last
 * component (length 0 for the root itself). */
static uint32_t walk_parent(const Ramfs_Pool *pool, uint32_t root, const char *path, uint32_t *parent,
                            const char **leaf, size_t *leaf_length) {
  uint32_t dir = root;
  const char *p = path;
  *leaf = "";
  *leaf_length = 0;
  for (;;) {
    while (*p == '/') p++;
    if (!*p) return 0;
    const char *start = p;
    while (*p && *p != '/') p++;
    const size_t length = (size_t)(p - start);
    if (length >= RAMFS_NAME_BYTES) return FS_RESULT_TOO_LONG_PATH;
    const char *rest = p;
    while (*rest == '/') rest++;
    const bool last = *rest == 0;
    if (length == 1 && start[0] == '.') {
      if (last) return 0;
      continue;
    }
    if (length == 2 && start[0] == '.' && start[1] == '.') {
      if (dir != root) dir = pool->nodes[dir].parent;
      if (last) {
        *parent = dir;
        return 0;
      }
      continue;
    }
    if (last) {
      *parent = dir;
      *leaf = start;
      *leaf_length = length;
      return 0;
    }
    const uint32_t child = find_child(pool, dir, start, length);
    if (child == RAMFS_NO_NODE || !pool->nodes[child].is_dir) return FS_RESULT_PATH_NOT_FOUND;
    dir = child;
    *parent = dir;
  }
}

uint32_t ramfs_lookup(const Ramfs_Pool *pool, uint32_t root, const char *path, uint32_t *node) {
  uint32_t parent = root;
  const char *leaf;
  size_t length;
  const uint32_t rc = walk_parent(pool, root, path, &parent, &leaf, &length);
  if (rc) return rc;
  if (!length) {
    *node = parent;
    return 0;
  }
  const uint32_t child = find_child(pool, parent, leaf, length);
  if (child == RAMFS_NO_NODE) return FS_RESULT_PATH_NOT_FOUND;
  *node = child;
  return 0;
}

static uint32_t create_node(Ramfs_Pool *pool, uint32_t root, const char *path, bool is_dir, uint32_t *out) {
  uint32_t parent = root;
  const char *leaf;
  size_t length;
  const uint32_t rc = walk_parent(pool, root, path, &parent, &leaf, &length);
  if (rc) return rc;
  if (!length) return FS_RESULT_PATH_ALREADY_EXISTS; /* the root, or a trailing ".." */
  if (find_child(pool, parent, leaf, length) != RAMFS_NO_NODE) return FS_RESULT_PATH_ALREADY_EXISTS;
  const uint32_t node = node_alloc(pool, is_dir, leaf, length);
  if (node == RAMFS_NO_NODE) return FS_RESULT_ALLOCATION_TABLE_FULL;
  link_child(pool, parent, node);
  *out = node;
  return 0;
}

uint32_t ramfs_create_file(Ramfs_Pool *pool, uint32_t root, const char *path, uint64_t size) {
  pool->generation++;
  uint32_t node = 0;
  uint32_t rc = create_node(pool, root, path, false, &node);
  if (rc) return rc;
  rc = ramfs_set_size(pool, node, size);
  if (rc) {
    unlink_child(pool, node);
    free_subtree(pool, node);
  }
  return rc;
}

uint32_t ramfs_create_directory(Ramfs_Pool *pool, uint32_t root, const char *path) {
  pool->generation++;
  uint32_t node = 0;
  return create_node(pool, root, path, true, &node);
}

uint32_t ramfs_delete_file(Ramfs_Pool *pool, uint32_t root, const char *path) {
  pool->generation++;
  uint32_t node = 0;
  const uint32_t rc = ramfs_lookup(pool, root, path, &node);
  if (rc) return rc;
  if (pool->nodes[node].is_dir) return FS_RESULT_PATH_NOT_FOUND;
  if (pool->nodes[node].open_count) return FS_RESULT_TARGET_LOCKED;
  unlink_child(pool, node);
  free_subtree(pool, node);
  return 0;
}

static bool subtree_has_open_files(const Ramfs_Pool *pool, uint32_t node) {
  if (pool->nodes[node].open_count) return true;
  for (uint32_t c = pool->nodes[node].first_child; c != RAMFS_NO_NODE; c = pool->nodes[c].next_sibling) {
    if (subtree_has_open_files(pool, c)) return true;
  }
  return false;
}

uint32_t ramfs_delete_directory(Ramfs_Pool *pool, uint32_t root, const char *path, bool recursive, bool clean_only) {
  pool->generation++;
  uint32_t node = 0;
  const uint32_t rc = ramfs_lookup(pool, root, path, &node);
  if (rc) return rc;
  Ramfs_Node *n = &pool->nodes[node];
  if (!n->is_dir) return FS_RESULT_PATH_NOT_FOUND;
  if (!recursive && n->first_child != RAMFS_NO_NODE) return FS_RESULT_DIRECTORY_NOT_EMPTY;
  if (subtree_has_open_files(pool, node)) return FS_RESULT_TARGET_LOCKED;
  if (clean_only) {
    uint32_t child = n->first_child;
    while (child != RAMFS_NO_NODE) {
      const uint32_t next = pool->nodes[child].next_sibling;
      free_subtree(pool, child);
      child = next;
    }
    n->first_child = RAMFS_NO_NODE;
    return 0;
  }
  if (node == root) return FS_RESULT_INVALID_PATH;
  unlink_child(pool, node);
  free_subtree(pool, node);
  return 0;
}

static bool is_ancestor(const Ramfs_Pool *pool, uint32_t maybe_ancestor, uint32_t node) {
  for (uint32_t n = node; n != RAMFS_NO_NODE; n = pool->nodes[n].parent) {
    if (n == maybe_ancestor) return true;
  }
  return false;
}

uint32_t ramfs_rename(Ramfs_Pool *pool, uint32_t root, const char *from, const char *to, bool directory) {
  pool->generation++;
  uint32_t node = 0;
  uint32_t rc = ramfs_lookup(pool, root, from, &node);
  if (rc) return rc;
  if (pool->nodes[node].is_dir != directory || node == root) return FS_RESULT_PATH_NOT_FOUND;
  uint32_t parent = root;
  const char *leaf;
  size_t length;
  rc = walk_parent(pool, root, to, &parent, &leaf, &length);
  if (rc) return rc;
  if (!length || find_child(pool, parent, leaf, length) != RAMFS_NO_NODE) return FS_RESULT_PATH_ALREADY_EXISTS;
  if (directory && is_ancestor(pool, node, parent)) return FS_RESULT_INVALID_PATH;
  unlink_child(pool, node);
  memset(pool->nodes[node].name, 0, RAMFS_NAME_BYTES);
  memcpy(pool->nodes[node].name, leaf, length);
  link_child(pool, parent, node);
  return 0;
}

/* ------------------------------------------------------------------ */
/* File data.                                                          */
/* ------------------------------------------------------------------ */

/* The block holding byte `offset` (which must be < the allocated span). */
static uint32_t block_at(const Ramfs_Pool *pool, const Ramfs_Node *n, uint64_t offset) {
  uint32_t block = n->first_block;
  for (uint64_t i = offset / RAMFS_BLOCK_BYTES; i && block != RAMFS_NO_BLOCK; i--) block = pool->next_block[block];
  return block;
}

uint32_t ramfs_set_size(Ramfs_Pool *pool, uint32_t node, uint64_t size) {
  pool->generation++;
  Ramfs_Node *n = &pool->nodes[node];
  if (n->is_dir) return FS_RESULT_PATH_NOT_FOUND;
  n->version++;
  const uint32_t have = blocks_for(n->size), want = blocks_for(size);
  if (want > have) {
    if (want - have > pool->free_block_count) return FS_RESULT_USABLE_SPACE_NOT_ENOUGH;
    uint32_t *link = &n->first_block;
    while (*link != RAMFS_NO_BLOCK) link = &pool->next_block[*link];
    for (uint32_t i = have; i < want; i++) {
      *link = block_alloc(pool);
      link = &pool->next_block[*link];
    }
  } else if (want < have) {
    uint32_t *link = &n->first_block;
    for (uint32_t i = 0; i < want; i++) link = &pool->next_block[*link];
    free_chain(pool, *link);
    *link = RAMFS_NO_BLOCK;
  }
  /* Bytes past a shrink point inside the last kept block must read as
   * zero if the file grows again. */
  if (size < n->size && size % RAMFS_BLOCK_BYTES) {
    const uint32_t block = block_at(pool, n, size);
    if (block != RAMFS_NO_BLOCK) {
      const uint64_t within = size % RAMFS_BLOCK_BYTES;
      memset(pool->blocks + (uint64_t)block * RAMFS_BLOCK_BYTES + within, 0, RAMFS_BLOCK_BYTES - within);
    }
  }
  n->size = size;
  return 0;
}

uint32_t ramfs_read(Ramfs_Pool *pool, uint32_t node, uint64_t offset, void *out, uint64_t size, uint64_t *read) {
  const Ramfs_Node *n = &pool->nodes[node];
  *read = 0;
  if (n->is_dir) return FS_RESULT_PATH_NOT_FOUND;
  if (offset > n->size) return FS_RESULT_OUT_OF_RANGE;
  if (size > n->size - offset) size = n->size - offset;
  uint32_t block = size ? block_at(pool, n, offset) : RAMFS_NO_BLOCK;
  uint64_t within = offset % RAMFS_BLOCK_BYTES, done = 0;
  while (done < size && block != RAMFS_NO_BLOCK) {
    const uint64_t chunk = size - done < RAMFS_BLOCK_BYTES - within ? size - done : RAMFS_BLOCK_BYTES - within;
    memcpy((uint8_t *)out + done, pool->blocks + (uint64_t)block * RAMFS_BLOCK_BYTES + within, (size_t)chunk);
    done += chunk;
    within = 0;
    block = pool->next_block[block];
  }
  *read = done;
  return 0;
}

uint32_t ramfs_write(Ramfs_Pool *pool, uint32_t node, uint64_t offset, const void *src, uint64_t size) {
  pool->generation++;
  Ramfs_Node *n = &pool->nodes[node];
  if (n->is_dir) return FS_RESULT_PATH_NOT_FOUND;
  n->version++;
  if (offset + size > n->size) {
    const uint32_t rc = ramfs_set_size(pool, node, offset + size);
    if (rc) return rc;
  }
  uint32_t block = size ? block_at(pool, n, offset) : RAMFS_NO_BLOCK;
  uint64_t within = offset % RAMFS_BLOCK_BYTES, done = 0;
  while (done < size && block != RAMFS_NO_BLOCK) {
    const uint64_t chunk = size - done < RAMFS_BLOCK_BYTES - within ? size - done : RAMFS_BLOCK_BYTES - within;
    memcpy(pool->blocks + (uint64_t)block * RAMFS_BLOCK_BYTES + within, (const uint8_t *)src + done, (size_t)chunk);
    done += chunk;
    within = 0;
    block = pool->next_block[block];
  }
  return 0;
}

uint64_t ramfs_free_bytes(const Ramfs_Pool *pool) { return (uint64_t)pool->free_block_count * RAMFS_BLOCK_BYTES; }
uint64_t ramfs_total_bytes(const Ramfs_Pool *pool) { return (uint64_t)pool->block_count * RAMFS_BLOCK_BYTES; }
