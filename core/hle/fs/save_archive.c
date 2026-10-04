/**
 * Save archives. See save_archive.h.
 */
#include "hle/fs/save_archive.h"

#include <string.h>

#define ARCHIVE_DEPTH 64u
#define ARCHIVE_PATH_BYTES 0x301u /* Horizon's path limit (fs.h FS_MAX_PATH_BYTES) */
#define ARCHIVE_CHUNK 0x4000u

typedef bool (*Visit)(void *user, Ramfs_Pool *pool, uint32_t node, const char *path);

/* Depth-first over the tree under `root`, parents before children; false
 * if a visit refused or the tree is too deep / a path too long. */
static bool walk(Ramfs_Pool *pool, uint32_t root, Visit visit, void *user) {
  uint32_t stack[ARCHIVE_DEPTH];
  size_t length[ARCHIVE_DEPTH];
  char path[ARCHIVE_PATH_BYTES];
  uint32_t depth = 0;
  path[0] = '\0';
  stack[0] = pool->nodes[root].first_child;
  length[0] = 0;
  for (;;) {
    const uint32_t node = stack[depth];
    if (node == RAMFS_NO_NODE) {
      if (depth == 0) return true;
      depth--;
      path[length[depth]] = '\0';
      stack[depth] = pool->nodes[stack[depth]].next_sibling;
      continue;
    }
    const Ramfs_Node *n = &pool->nodes[node];
    const size_t at = length[depth], name = strlen(n->name);
    if (at + 1u + name >= sizeof(path)) return false;
    path[at] = '/';
    memcpy(path + at + 1u, n->name, name + 1u);
    if (!visit(user, pool, node, path)) return false;
    if (n->is_dir) {
      if (depth + 1u >= ARCHIVE_DEPTH) return false;
      length[depth + 1u] = at + 1u + name;
      stack[++depth] = n->first_child;
      continue;
    }
    path[at] = '\0';
    stack[depth] = n->next_sibling;
  }
}

static bool count_entry(void *user, Ramfs_Pool *pool, uint32_t node, const char *path) {
  (void)pool;
  (void)node;
  (void)path;
  (*(uint32_t *)user)++;
  return true;
}

typedef struct Emit {
  const Save_Archive_Sink *sink;
  uint64_t bytes;
} Emit;

static bool put(Emit *e, const void *bytes, uint64_t size) {
  if (!e->sink->write(e->sink->user, bytes, size)) return false;
  e->bytes += size;
  return true;
}

static bool emit_entry(void *user, Ramfs_Pool *pool, uint32_t node, const char *path) {
  Emit *e = (Emit *)user;
  const Ramfs_Node *n = &pool->nodes[node];
  const uint32_t head[2] = {n->is_dir ? SAVE_ARCHIVE_KIND_DIRECTORY : SAVE_ARCHIVE_KIND_FILE, (uint32_t)strlen(path)};
  const uint64_t size = n->is_dir ? 0u : n->size;
  if (!put(e, head, sizeof(head)) || !put(e, path, head[1]) || !put(e, &size, sizeof(size))) return false;
  static uint8_t chunk[ARCHIVE_CHUNK];
  for (uint64_t at = 0; at < size; at += ARCHIVE_CHUNK) {
    const uint64_t want = size - at < ARCHIVE_CHUNK ? size - at : ARCHIVE_CHUNK;
    uint64_t got = 0;
    if (ramfs_read(pool, node, at, chunk, want, &got) || got != want || !put(e, chunk, want)) return false;
  }
  return true;
}

uint64_t save_archive_write(const Ramfs_Pool *pool, uint32_t root, const Save_Archive_Sink *sink) {
  Ramfs_Pool *p = (Ramfs_Pool *)(uintptr_t)pool; /* ramfs_read takes the pool mutable; nothing changes */
  uint32_t entries = 0;
  if (!walk(p, root, count_entry, &entries)) return 0;
  const uint32_t header[4] = {SAVE_ARCHIVE_MAGIC, SAVE_ARCHIVE_VERSION, entries, 0};
  Emit e = {sink, 0};
  if (!put(&e, header, sizeof(header)) || !walk(p, root, emit_entry, &e)) return 0;
  return e.bytes;
}

/* ---- Reading -------------------------------------------------------- */

typedef struct Entry {
  uint32_t kind;
  const char *path;
  uint32_t path_bytes;
  const uint8_t *data;
  uint64_t size;
} Entry;

static uint32_t rd32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

static uint64_t rd64(const uint8_t *p) {
  uint64_t v;
  memcpy(&v, p, 8);
  return v;
}

/* The entry at *at (advanced past it); false if it runs off the end or is malformed. */
static bool next_entry(const uint8_t *bytes, uint64_t size, uint64_t *at, Entry *out) {
  if (size - *at < 8u) return false;
  out->kind = rd32(bytes + *at);
  out->path_bytes = rd32(bytes + *at + 4u);
  *at += 8u;
  if (out->kind > SAVE_ARCHIVE_KIND_FILE || out->path_bytes < 2u || out->path_bytes >= ARCHIVE_PATH_BYTES ||
      size - *at < (uint64_t)out->path_bytes + 8u)
    return false;
  out->path = (const char *)bytes + *at;
  if (out->path[0] != '/' || memchr(out->path, '\0', out->path_bytes)) return false;
  *at += out->path_bytes;
  out->size = rd64(bytes + *at);
  *at += 8u;
  if (out->kind == SAVE_ARCHIVE_KIND_DIRECTORY ? out->size != 0 : size - *at < out->size) return false;
  out->data = bytes + *at;
  *at += out->size;
  return true;
}

bool save_archive_valid(const uint8_t *bytes, uint64_t size) {
  if (!bytes || size < SAVE_ARCHIVE_HEADER_BYTES || rd32(bytes) != SAVE_ARCHIVE_MAGIC ||
      rd32(bytes + 4) != SAVE_ARCHIVE_VERSION)
    return false;
  const uint32_t entries = rd32(bytes + 8);
  uint64_t at = SAVE_ARCHIVE_HEADER_BYTES;
  for (uint32_t i = 0; i < entries; i++) {
    Entry e;
    if (!next_entry(bytes, size, &at, &e)) return false;
  }
  return at == size;
}

uint32_t save_archive_restore(Ramfs_Pool *pool, uint32_t root, const uint8_t *bytes, uint64_t size) {
  if (!save_archive_valid(bytes, size)) return FS_RESULT_INVALID_PATH;
  uint32_t rc = ramfs_delete_directory(pool, root, "/", true, true);
  if (rc) return rc; /* a file is open: leave the save alone */
  const uint32_t entries = rd32(bytes + 8);
  uint64_t at = SAVE_ARCHIVE_HEADER_BYTES;
  char path[ARCHIVE_PATH_BYTES];
  for (uint32_t i = 0; i < entries; i++) {
    Entry e;
    (void)next_entry(bytes, size, &at, &e);
    memcpy(path, e.path, e.path_bytes);
    path[e.path_bytes] = '\0';
    if (e.kind == SAVE_ARCHIVE_KIND_DIRECTORY) {
      rc = ramfs_create_directory(pool, root, path);
      if (rc && rc != FS_RESULT_PATH_ALREADY_EXISTS) return rc;
      continue;
    }
    uint32_t node = 0;
    rc = ramfs_create_file(pool, root, path, 0);
    if (!rc) rc = ramfs_lookup(pool, root, path, &node);
    if (!rc && e.size) rc = ramfs_write(pool, node, 0, e.data, e.size);
    if (rc) return rc;
  }
  return 0;
}
