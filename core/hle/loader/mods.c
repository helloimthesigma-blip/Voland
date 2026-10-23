#include "hle/loader/mods.h"

#include <stdio.h>
#include <string.h>

#include "common/log.h"
#include "hle/loader/romfs.h"

/* Open-addressed path index: twice MODS_MAX_FILES slots keeps chains short
 * at the cap. Slot value 0 = empty, else file index + 1. */
#define MODS_INDEX_SLOTS (MODS_MAX_FILES * 2u)

static uint32_t *index_slots(const Mod_Set *set) {
  return (uint32_t *)set->arena.base;
}

static uint32_t hash_bytes(const char *s, uint32_t length) {
  uint32_t h = 2166136261u; /* FNV-1a */
  for (uint32_t i = 0; i < length; i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

void mod_set_bind(Mod_Set *set, Mod_Read_Fn read, void *user) {
  set->read = read;
  set->user = user;
}

void mod_set_clear(Mod_Set *set) {
  if (set->live) arena_destroy(&set->arena);
  set->live = false;
  set->files = NULL;
  set->count = 0;
}

static Error mod_file_read(void *user, uint64_t offset, void *out, uint64_t size) {
  const Mod_File *file = (const Mod_File *)user;
  if (!file->set->read) return ERR(RESULT_IO_ERROR, "mods: no read callback");
  return file->set->read(file->set->user, file->id, offset, out, size);
}

static Error ensure_live(Mod_Set *set) {
  if (set->live) return OK;
  if (!arena_create(&set->arena, MODS_ARENA_BYTES)) return ERR(RESULT_OUT_OF_MEMORY, "mods: arena");
  set->live = true;
  uint32_t *slots = ARENA_ALLOC_ARRAY(&set->arena, uint32_t, MODS_INDEX_SLOTS);
  set->files = ARENA_ALLOC_ARRAY(&set->arena, Mod_File, MODS_MAX_FILES);
  if (!slots || !set->files) {
    mod_set_clear(set);
    return ERR(RESULT_OUT_OF_MEMORY, "mods: arena");
  }
  memset(slots, 0, sizeof(uint32_t) * MODS_INDEX_SLOTS);
  set->count = 0;
  return OK;
}

/* The slot holding `path`, or the empty slot where it would go. */
static uint32_t *find_slot(const Mod_Set *set, const char *path, uint32_t length) {
  uint32_t *slots = index_slots(set);
  uint32_t i = hash_bytes(path, length) & (MODS_INDEX_SLOTS - 1u);
  for (;;) {
    if (slots[i] == 0) return &slots[i];
    const Mod_File *f = &set->files[slots[i] - 1u];
    if (f->path_length == length && memcmp(f->path, path, length) == 0) return &slots[i];
    i = (i + 1u) & (MODS_INDEX_SLOTS - 1u);
  }
}

Error mod_set_add(Mod_Set *set, const char *path, uint64_t size, uint32_t id) {
  if (!set || !path) return ERR(RESULT_INVALID_ARGUMENT, "mod_set_add: NULL");
  while (*path == '/' || *path == '\\') path++;
  const size_t length = strlen(path);
  if (length == 0 || length >= MODS_MAX_PATH_BYTES) return ERR(RESULT_INVALID_ARGUMENT, "mod_set_add: bad path length");
  char normalized[MODS_MAX_PATH_BYTES];
  for (size_t i = 0; i <= length; i++) normalized[i] = path[i] == '\\' ? '/' : path[i];
  /* Reject empty, "." and ".." components: they would alias other paths. */
  const char *component = normalized;
  for (size_t i = 0; i <= length; i++) {
    if (normalized[i] == '/' || normalized[i] == '\0') {
      const size_t n = (size_t)(&normalized[i] - component);
      if (n == 0 || (n == 1 && component[0] == '.') || (n == 2 && component[0] == '.' && component[1] == '.')) {
        return ERR(RESULT_INVALID_ARGUMENT, "mod_set_add: empty or dot path component");
      }
      component = &normalized[i + 1];
    }
  }
  Error err = ensure_live(set);
  if (!error_is_ok(err)) return err;
  uint32_t *slot = find_slot(set, normalized, (uint32_t)length);
  if (*slot != 0) return OK; /* an earlier (higher-priority) mod has it */
  if (set->count >= MODS_MAX_FILES) return ERR(RESULT_OUT_OF_MEMORY, "mods: too many files");
  char *copy = ARENA_ALLOC_ARRAY(&set->arena, char, length + 1);
  if (!copy) return ERR(RESULT_OUT_OF_MEMORY, "mods: arena");
  memcpy(copy, normalized, length + 1);
  Mod_File *file = &set->files[set->count];
  file->path = copy;
  file->path_length = (uint32_t)length;
  file->id = id;
  file->size = size;
  file->set = set;
  file->source = (Byte_Source){file, size, mod_file_read};
  set->count++;
  *slot = set->count;
  return OK;
}

const Mod_File *mod_set_find(const Mod_Set *set, const char *path) {
  if (!set || !set->live || !path) return NULL;
  const uint32_t *slot = find_slot(set, path, (uint32_t)strlen(path));
  return *slot ? &set->files[*slot - 1u] : NULL;
}

static bool has_prefix(const Mod_File *f, const char *prefix, size_t n) {
  return f->path_length >= n && memcmp(f->path, prefix, n) == 0;
}

static bool has_suffix(const Mod_File *f, const char *suffix) {
  const size_t n = strlen(suffix);
  if (f->path_length < n) return false;
  const char *tail = f->path + f->path_length - n;
  for (size_t i = 0; i < n; i++) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != suffix[i]) return false;
  }
  return true;
}

uint32_t mod_set_count_prefix(const Mod_Set *set, const char *prefix) {
  if (!set || !set->live) return 0;
  const size_t n = strlen(prefix);
  uint32_t count = 0;
  for (uint32_t i = 0; i < set->count; i++) count += has_prefix(&set->files[i], prefix, n) ? 1u : 0u;
  return count;
}

Error mod_file_read_all(const Mod_File *file, Arena *arena, uint8_t **out, uint64_t *size) {
  if (file->size > MODS_MAX_TEXT_FILE_BYTES) return ERR(RESULT_INVALID_ARGUMENT, "mods: patch file too large");
  uint8_t *buffer = ARENA_ALLOC_ARRAY(arena, uint8_t, (size_t)file->size + 1);
  if (!buffer) return ERR(RESULT_OUT_OF_MEMORY, "mods: scratch arena");
  Error err = byte_source_read(&file->source, 0, buffer, file->size);
  if (!error_is_ok(err)) return err;
  buffer[file->size] = 0;
  *out = buffer;
  *size = file->size;
  return OK;
}

static bool component_is(const char *c, size_t n, const char *name) {
  if (strlen(name) != n) return false;
  for (size_t i = 0; i < n; i++) {
    char ch = c[i];
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    if (ch != name[i]) return false;
  }
  return true;
}

static bool component_is_title(const char *c, size_t n, uint64_t *title) {
  if (n != 16) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    const char ch = c[i];
    const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
    if (d < 0) return false;
    v = v << 4 | (uint64_t)d;
  }
  *title = v;
  return true;
}

static bool ends_with_ci(const char *s, size_t n, const char *suffix) {
  const size_t k = strlen(suffix);
  return n >= k && component_is(s + n - k, k, suffix);
}

bool mods_normalize_path(const char *mod_name, const char *path, uint64_t title_id, char *out, size_t out_size) {
  if (!path || !out || out_size == 0) return false;
  char buf[MODS_MAX_PATH_BYTES];
  size_t n = strlen(path);
  if (n >= sizeof(buf)) return false;
  for (size_t i = 0; i <= n; i++) buf[i] = path[i] == '\\' ? '/' : path[i];
  /* Split into components. */
  const char *parts[64];
  size_t lens[64];
  uint32_t count = 0;
  for (size_t i = 0, start = 0; i <= n; i++) {
    if (buf[i] == '/' || buf[i] == '\0') {
      if (i > start) {
        if (count >= 64) return false;
        parts[count] = &buf[start];
        lens[count] = i - start;
        count++;
      }
      start = i + 1;
    }
  }
  if (count == 0) return false;
  const char *mod = mod_name && *mod_name ? mod_name : "mod";
  const char *file = parts[count - 1];
  const size_t file_len = lens[count - 1];
  for (uint32_t i = 0; i < count; i++) {
    uint64_t title = 0;
    if (component_is_title(parts[i], lens[i], &title) && title_id != 0 && title != title_id) return false;
    if (component_is(parts[i], lens[i], "romfs")) {
      if (i + 1 >= count) return false;
      /* Everything after romfs/, case kept. */
      const char *rest = parts[i + 1];
      const size_t rest_len = (size_t)(buf + n - rest);
      return snprintf(out, out_size, "romfs/%.*s", (int)rest_len, rest) < (int)out_size;
    }
    if (component_is(parts[i], lens[i], "exefs_patches")) break; /* handled below by suffix */
    if (component_is(parts[i], lens[i], "exefs") && i + 2 == count) {
      if (ends_with_ci(file, file_len, ".ips") || ends_with_ci(file, file_len, ".pchtxt")) break;
      return snprintf(out, out_size, "exefs/%.*s", (int)file_len, file) < (int)out_size;
    }
    if (component_is(parts[i], lens[i], "cheats") && i + 2 == count && ends_with_ci(file, file_len, ".txt")) {
      return snprintf(out, out_size, "cheats/%s/%.*s", mod, (int)file_len, file) < (int)out_size;
    }
  }
  if (ends_with_ci(file, file_len, ".ips") || ends_with_ci(file, file_len, ".pchtxt")) {
    return snprintf(out, out_size, "exefs/%s/%.*s", mod, (int)file_len, file) < (int)out_size;
  }
  return false;
}

/* ------------------------------------------------------------------ */
/* LayeredFS.                                                          */
/* ------------------------------------------------------------------ */

#define LAYERED_DATA_START ((uint64_t)0x200)
#define LAYERED_FILE_ALIGN ((uint64_t)16)
#define LAYERED_NONE 0xFFFFFFFFu

typedef struct L_Dir {
  const char *name;
  uint32_t name_length;
  uint32_t parent;
  uint32_t first_dir, last_dir, first_file, last_file, next;
  uint32_t out_offset;
} L_Dir;

typedef struct L_File {
  const char *name;
  uint32_t name_length;
  uint32_t parent;
  uint32_t next;
  const Byte_Source *from; /* NULL = base data region */
  uint64_t base_offset;    /* relative to the base file data offset */
  uint64_t size;
  uint64_t out_data;       /* relative to the layered file data offset */
  uint32_t out_offset;
} L_File;

typedef struct L_Tree {
  L_Dir *dirs;
  uint32_t dir_count, dir_cap;
  L_File *files;
  uint32_t file_count, file_cap;
  /* (parent, name) -> entry index; dirs and files share the table with
   * the top bit marking files. */
  uint32_t *slots;
  uint32_t slot_mask;
} L_Tree;

static uint32_t node_hash(uint32_t parent, const char *name, uint32_t length) {
  return hash_bytes(name, length) ^ (parent * 0x9E3779B1u);
}

#define L_FILE_BIT 0x80000000u

static uint32_t *l_slot(L_Tree *t, uint32_t parent, const char *name, uint32_t length, bool file) {
  uint32_t i = node_hash(parent, name, length) & t->slot_mask;
  for (;;) {
    const uint32_t v = t->slots[i];
    if (v == LAYERED_NONE) return &t->slots[i];
    if (((v & L_FILE_BIT) != 0) == file) {
      const uint32_t idx = v & ~L_FILE_BIT;
      const char *n = file ? t->files[idx].name : t->dirs[idx].name;
      const uint32_t nl = file ? t->files[idx].name_length : t->dirs[idx].name_length;
      const uint32_t p = file ? t->files[idx].parent : t->dirs[idx].parent;
      if (p == parent && nl == length && memcmp(n, name, length) == 0) return &t->slots[i];
    }
    i = (i + 1u) & t->slot_mask;
  }
}

static uint32_t l_add_dir(L_Tree *t, uint32_t parent, const char *name, uint32_t length) {
  uint32_t *slot = l_slot(t, parent, name, length, false);
  if (*slot != LAYERED_NONE) return *slot;
  if (t->dir_count >= t->dir_cap) return LAYERED_NONE;
  const uint32_t idx = t->dir_count++;
  L_Dir *d = &t->dirs[idx];
  memset(d, 0, sizeof(*d));
  d->name = name;
  d->name_length = length;
  d->parent = parent;
  d->first_dir = d->last_dir = d->first_file = d->last_file = d->next = LAYERED_NONE;
  *slot = idx;
  L_Dir *p = &t->dirs[parent];
  if (p->last_dir == LAYERED_NONE) p->first_dir = idx;
  else t->dirs[p->last_dir].next = idx;
  p->last_dir = idx;
  return idx;
}

/* Returns the file's index; `*existed` says whether it was already there. */
static uint32_t l_add_file(L_Tree *t, uint32_t parent, const char *name, uint32_t length, bool *existed) {
  uint32_t *slot = l_slot(t, parent, name, length, true);
  if (*slot != LAYERED_NONE) {
    *existed = true;
    return *slot & ~L_FILE_BIT;
  }
  *existed = false;
  if (t->file_count >= t->file_cap) return LAYERED_NONE;
  const uint32_t idx = t->file_count++;
  L_File *f = &t->files[idx];
  memset(f, 0, sizeof(*f));
  f->name = name;
  f->name_length = length;
  f->parent = parent;
  f->next = LAYERED_NONE;
  *slot = idx | L_FILE_BIT;
  L_Dir *p = &t->dirs[parent];
  if (p->last_file == LAYERED_NONE) p->first_file = idx;
  else t->files[p->last_file].next = idx;
  p->last_file = idx;
  return idx;
}

/* Copies the base tree in, depth first, preserving sibling order. */
static Error load_base(L_Tree *t, const RomFS *fs, uint64_t *data_extent) {
  /* Explicit stack of (base dir offset, tree dir index). */
  uint32_t stack_cap = t->dir_cap;
  uint32_t *stack_base = t->slots + (t->slot_mask + 1u); /* scratch after the slots */
  uint32_t *stack_tree = stack_base + stack_cap;
  uint32_t depth = 0;
  stack_base[depth] = ROMFS_ROOT_DIR_OFFSET;
  stack_tree[depth] = 0;
  depth++;
  while (depth > 0) {
    depth--;
    const uint32_t base_dir = stack_base[depth];
    const uint32_t tree_dir = stack_tree[depth];
    RomFS_Dir_Entry dir;
    Error err = romfs_dir_entry(fs, base_dir, &dir);
    if (!error_is_ok(err)) return err;
    for (uint32_t off = dir.first_child_file, steps = 0; off != ROMFS_NO_ENTRY; steps++) {
      if (steps > t->file_cap) return ERR(RESULT_INVALID_ARGUMENT, "layeredfs: cyclic file list");
      RomFS_File_Entry fe;
      err = romfs_file_entry(fs, off, &fe);
      if (!error_is_ok(err)) return err;
      bool existed = false;
      const uint32_t idx = l_add_file(t, tree_dir, fe.name, fe.name_length, &existed);
      if (idx == LAYERED_NONE) return ERR(RESULT_OUT_OF_MEMORY, "layeredfs: file cap");
      t->files[idx].from = NULL;
      t->files[idx].base_offset = fe.data_offset - fs->file_data_offset;
      t->files[idx].size = fe.data_size;
      if (fe.data_offset + fe.data_size > *data_extent) *data_extent = fe.data_offset + fe.data_size;
      off = fe.next_sibling;
    }
    /* Push children in reverse so they are visited in order (the tree
     * itself keeps order through l_add_dir's append). */
    for (uint32_t off = dir.first_child_dir, steps = 0; off != ROMFS_NO_ENTRY; steps++) {
      if (steps > t->dir_cap) return ERR(RESULT_INVALID_ARGUMENT, "layeredfs: cyclic dir list");
      RomFS_Dir_Entry child;
      err = romfs_dir_entry(fs, off, &child);
      if (!error_is_ok(err)) return err;
      const uint32_t idx = l_add_dir(t, tree_dir, child.name, child.name_length);
      if (idx == LAYERED_NONE || depth >= stack_cap) return ERR(RESULT_OUT_OF_MEMORY, "layeredfs: dir cap");
      stack_base[depth] = off;
      stack_tree[depth] = idx;
      depth++;
      off = child.next_sibling;
    }
  }
  return OK;
}

static uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

static uint32_t bucket_count(uint32_t entries) {
  if (entries < 3) return 3;
  if (entries < 19) return entries | 1u;
  uint32_t n = entries | 1u;
  /* Small odd numbers divisible by 3, 5, 7, 11, 13 spread poorly. */
  while (n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0) n += 2;
  return n;
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}

static Error layered_read(void *user, uint64_t offset, void *out, uint64_t size) {
  const Layered_RomFS *l = (const Layered_RomFS *)user;
  uint8_t *dst = (uint8_t *)out;
  /* Binary search the first segment ending after `offset`. */
  uint32_t lo = 0, hi = l->segment_count;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (l->segments[mid].start + l->segments[mid].size <= offset) lo = mid + 1;
    else hi = mid;
  }
  while (size > 0) {
    const Layered_Segment *s = lo < l->segment_count ? &l->segments[lo] : NULL;
    if (!s || offset < s->start) {
      /* Alignment padding between segments reads as zero. */
      const uint64_t gap_end = s ? s->start : l->source.size;
      const uint64_t n = gap_end - offset < size ? gap_end - offset : size;
      memset(dst, 0, (size_t)n);
      dst += n; offset += n; size -= n;
      continue;
    }
    const uint64_t within = offset - s->start;
    const uint64_t n = s->size - within < size ? s->size - within : size;
    if (s->from) {
      Error err = byte_source_read(s->from, s->from_offset + within, dst, n);
      if (!error_is_ok(err)) return err;
    } else {
      memcpy(dst, s->bytes + within, (size_t)n);
    }
    dst += n; offset += n; size -= n;
    lo++;
  }
  return OK;
}

Error layeredfs_build(const Byte_Source *base, const Mod_Set *mods, Arena *arena, Layered_RomFS *out) {
  memset(out, 0, sizeof(*out));
  const uint32_t mod_files = mod_set_count_prefix(mods, "romfs/");
  if (mod_files == 0) return ERR(RESULT_NOT_FOUND, "layeredfs: no romfs mods");

  RomFS fs;
  bool have_base = false;
  if (base) {
    Error err = romfs_open(base, arena, &fs);
    if (!error_is_ok(err)) return err;
    have_base = true;
  }
  /* Caps: every base entry, plus each mod file and one dir per path
   * component of it. */
  uint32_t extra_dirs = 0;
  for (uint32_t i = 0; i < mods->count; i++) {
    const Mod_File *f = &mods->files[i];
    if (!has_prefix(f, "romfs/", 6)) continue;
    for (uint32_t c = 6; c < f->path_length; c++) extra_dirs += f->path[c] == '/' ? 1u : 0u;
  }
  L_Tree t;
  memset(&t, 0, sizeof(t));
  t.dir_cap = 1u + extra_dirs + (have_base ? fs.dir_table_size / ROMFS_DIR_ENTRY_FIXED_SIZE : 0u);
  t.file_cap = mod_files + (have_base ? fs.file_table_size / ROMFS_FILE_ENTRY_FIXED_SIZE : 0u);
  uint32_t slots = 16;
  while (slots < 2u * (t.dir_cap + t.file_cap)) slots <<= 1;
  t.slot_mask = slots - 1u;
  t.dirs = ARENA_ALLOC_ARRAY(arena, L_Dir, t.dir_cap);
  t.files = ARENA_ALLOC_ARRAY(arena, L_File, t.file_cap);
  /* Slots plus the base walk's two stacks. */
  t.slots = ARENA_ALLOC_ARRAY(arena, uint32_t, (size_t)slots + 2u * t.dir_cap);
  if (!t.dirs || !t.files || !t.slots) return ERR(RESULT_OUT_OF_MEMORY, "layeredfs: arena");
  memset(t.slots, 0xFF, sizeof(uint32_t) * slots);
  memset(&t.dirs[0], 0, sizeof(L_Dir));
  t.dirs[0].name = "";
  t.dirs[0].first_dir = t.dirs[0].last_dir = t.dirs[0].first_file = t.dirs[0].last_file = t.dirs[0].next = LAYERED_NONE;
  t.dir_count = 1;

  uint64_t base_extent = 0;
  if (have_base) {
    base_extent = fs.file_data_offset;
    Error err = load_base(&t, &fs, &base_extent);
    if (!error_is_ok(err)) return err;
  }
  const uint64_t base_data_bytes = have_base ? base_extent - fs.file_data_offset : 0;

  /* Overlay the mods. */
  for (uint32_t i = 0; i < mods->count; i++) {
    const Mod_File *mf = &mods->files[i];
    if (!has_prefix(mf, "romfs/", 6)) continue;
    uint32_t dir = 0;
    const char *p = mf->path + 6;
    const char *end = mf->path + mf->path_length;
    for (;;) {
      const char *slash = memchr(p, '/', (size_t)(end - p));
      if (!slash) break;
      dir = l_add_dir(&t, dir, p, (uint32_t)(slash - p));
      if (dir == LAYERED_NONE) return ERR(RESULT_OUT_OF_MEMORY, "layeredfs: dir cap");
      p = slash + 1;
    }
    if (p == end) continue;
    bool existed = false;
    const uint32_t idx = l_add_file(&t, dir, p, (uint32_t)(end - p), &existed);
    if (idx == LAYERED_NONE) return ERR(RESULT_OUT_OF_MEMORY, "layeredfs: file cap");
    if (existed) out->replaced++;
    else out->added++;
    t.files[idx].from = &mf->source;
    t.files[idx].size = mf->size;
  }

  /* Lay out file data: the base region verbatim, then each mod file. */
  uint64_t cursor = (base_data_bytes + LAYERED_FILE_ALIGN - 1) & ~(LAYERED_FILE_ALIGN - 1);
  uint32_t mod_segments = 0;
  for (uint32_t i = 0; i < t.file_count; i++) {
    L_File *f = &t.files[i];
    if (!f->from) {
      f->out_data = f->base_offset;
      continue;
    }
    f->out_data = cursor;
    cursor = (cursor + f->size + LAYERED_FILE_ALIGN - 1) & ~(LAYERED_FILE_ALIGN - 1);
    mod_segments++;
  }
  const uint64_t data_bytes = cursor;

  /* Entry offsets (depth-first order from the tree's own lists). */
  uint32_t dir_table = 0;
  for (uint32_t i = 0; i < t.dir_count; i++) {
    t.dirs[i].out_offset = dir_table;
    dir_table += ROMFS_DIR_ENTRY_FIXED_SIZE + align4(t.dirs[i].name_length);
  }
  uint32_t file_table = 0;
  for (uint32_t i = 0; i < t.file_count; i++) {
    t.files[i].out_offset = file_table;
    file_table += ROMFS_FILE_ENTRY_FIXED_SIZE + align4(t.files[i].name_length);
  }
  const uint32_t dir_buckets = bucket_count(t.dir_count);
  const uint32_t file_buckets = bucket_count(t.file_count);
  const uint64_t tables_bytes = (uint64_t)dir_buckets * 4 + dir_table + (uint64_t)file_buckets * 4 + file_table;
  if (tables_bytes > ROMFS_MAX_TABLE_BYTES) return ERR(RESULT_OUT_OF_MEMORY, "layeredfs: tables too large");
  uint8_t *tables = ARENA_ALLOC_ARRAY(arena, uint8_t, (size_t)tables_bytes);
  uint8_t *header = ARENA_ALLOC_ARRAY(arena, uint8_t, (size_t)LAYERED_DATA_START);
  if (!tables || !header) return ERR(RESULT_OUT_OF_MEMORY, "layeredfs: arena");
  memset(tables, 0, (size_t)tables_bytes);
  memset(header, 0, (size_t)LAYERED_DATA_START);
  uint8_t *dir_hash = tables;
  uint8_t *dir_entries = dir_hash + (size_t)dir_buckets * 4;
  uint8_t *file_hash = dir_entries + dir_table;
  uint8_t *file_entries = file_hash + (size_t)file_buckets * 4;
  memset(dir_hash, 0xFF, (size_t)dir_buckets * 4);
  memset(file_hash, 0xFF, (size_t)file_buckets * 4);

  for (uint32_t i = 0; i < t.dir_count; i++) {
    const L_Dir *d = &t.dirs[i];
    uint8_t *e = dir_entries + d->out_offset;
    const uint32_t parent = t.dirs[d->parent].out_offset;
    put32(e + 0x00, parent);
    put32(e + 0x04, d->next == LAYERED_NONE ? ROMFS_NO_ENTRY : t.dirs[d->next].out_offset);
    put32(e + 0x08, d->first_dir == LAYERED_NONE ? ROMFS_NO_ENTRY : t.dirs[d->first_dir].out_offset);
    put32(e + 0x0C, d->first_file == LAYERED_NONE ? ROMFS_NO_ENTRY : t.files[d->first_file].out_offset);
    const uint32_t bucket = romfs_path_hash(parent, d->name, d->name_length) % dir_buckets;
    memcpy(e + 0x10, dir_hash + bucket * 4, 4);
    put32(dir_hash + bucket * 4, d->out_offset);
    put32(e + 0x14, d->name_length);
    memcpy(e + 0x18, d->name, d->name_length);
  }
  for (uint32_t i = 0; i < t.file_count; i++) {
    const L_File *f = &t.files[i];
    uint8_t *e = file_entries + f->out_offset;
    const uint32_t parent = t.dirs[f->parent].out_offset;
    put32(e + 0x00, parent);
    put32(e + 0x04, f->next == LAYERED_NONE ? ROMFS_NO_ENTRY : t.files[f->next].out_offset);
    put64(e + 0x08, f->out_data);
    put64(e + 0x10, f->size);
    const uint32_t bucket = romfs_path_hash(parent, f->name, f->name_length) % file_buckets;
    memcpy(e + 0x18, file_hash + bucket * 4, 4);
    put32(file_hash + bucket * 4, f->out_offset);
    put32(e + 0x1C, f->name_length);
    memcpy(e + 0x20, f->name, f->name_length);
  }

  const uint64_t tables_start = (LAYERED_DATA_START + data_bytes + 3) & ~(uint64_t)3;
  const uint64_t dir_hash_off = tables_start;
  const uint64_t dir_table_off = dir_hash_off + (uint64_t)dir_buckets * 4;
  const uint64_t file_hash_off = dir_table_off + dir_table;
  const uint64_t file_table_off = file_hash_off + (uint64_t)file_buckets * 4;
  put64(header + 0x00, ROMFS_HEADER_SIZE);
  put64(header + 0x08, dir_hash_off);
  put64(header + 0x10, (uint64_t)dir_buckets * 4);
  put64(header + 0x18, dir_table_off);
  put64(header + 0x20, dir_table);
  put64(header + 0x28, file_hash_off);
  put64(header + 0x30, (uint64_t)file_buckets * 4);
  put64(header + 0x38, file_table_off);
  put64(header + 0x40, file_table);
  put64(header + 0x48, LAYERED_DATA_START);

  /* Segments in ascending order: header, base data, mod files, tables. */
  const uint32_t segment_cap = 3u + mod_segments;
  out->segments = ARENA_ALLOC_ARRAY(arena, Layered_Segment, segment_cap);
  if (!out->segments) return ERR(RESULT_OUT_OF_MEMORY, "layeredfs: arena");
  uint32_t n = 0;
  out->segments[n++] = (Layered_Segment){0, LAYERED_DATA_START, NULL, 0, header};
  if (base_data_bytes > 0) {
    out->segments[n++] = (Layered_Segment){LAYERED_DATA_START, base_data_bytes, base, fs.file_data_offset, NULL};
  }
  for (uint32_t i = 0; i < t.file_count; i++) {
    const L_File *f = &t.files[i];
    if (!f->from || f->size == 0) continue;
    out->segments[n++] = (Layered_Segment){LAYERED_DATA_START + f->out_data, f->size, f->from, 0, NULL};
  }
  out->segments[n++] = (Layered_Segment){tables_start, tables_bytes, NULL, 0, tables};
  out->segment_count = n;
  out->source = (Byte_Source){out, tables_start + tables_bytes, layered_read};
  return OK;
}

/* ------------------------------------------------------------------ */
/* ExeFS patches.                                                      */
/* ------------------------------------------------------------------ */

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool mods_build_id_matches(const char *hex, uint32_t hex_length, const uint8_t module_id[32]) {
  uint32_t digits = 0;
  while (digits < hex_length && digits < 64 && hex_value(hex[digits]) >= 0) digits++;
  /* Fewer than 8 bytes is not an id; an odd digit count is malformed. */
  if (digits < 16 || (digits & 1u)) return false;
  for (uint32_t i = 0; i < digits / 2; i++) {
    const uint8_t b = (uint8_t)(hex_value(hex[2 * i]) << 4 | hex_value(hex[2 * i + 1]));
    if (b != module_id[i]) return false;
  }
  return true;
}

static void write_record(uint8_t *image, uint64_t size, uint64_t file_offset, const uint8_t *bytes, uint64_t n,
                         uint32_t *written) {
  if (file_offset < MODS_NSO_HEADER_BYTES) return;
  const uint64_t at = file_offset - MODS_NSO_HEADER_BYTES;
  if (at >= size || n > size - at) return;
  memcpy(image + at, bytes, (size_t)n);
  (*written)++;
}

static void fill_record(uint8_t *image, uint64_t size, uint64_t file_offset, uint8_t value, uint64_t n,
                        uint32_t *written) {
  if (file_offset < MODS_NSO_HEADER_BYTES) return;
  const uint64_t at = file_offset - MODS_NSO_HEADER_BYTES;
  if (at >= size || n > size - at) return;
  memset(image + at, value, (size_t)n);
  (*written)++;
}

static uint32_t apply_ips(const uint8_t *p, uint64_t n, bool ips32, uint8_t *image, uint64_t size) {
  uint32_t written = 0;
  const uint32_t off_bytes = ips32 ? 4u : 3u;
  uint64_t i = ips32 ? 5u : 5u; /* "IPS32" / "PATCH" */
  while (i + off_bytes <= n) {
    uint64_t offset = 0;
    for (uint32_t b = 0; b < off_bytes; b++) offset = offset << 8 | p[i + b];
    if ((ips32 && offset == 0x45454F46u) || (!ips32 && offset == 0x454F46u)) break; /* "EEOF" / "EOF" */
    i += off_bytes;
    if (i + 2 > n) break;
    const uint32_t len = (uint32_t)p[i] << 8 | p[i + 1];
    i += 2;
    if (len == 0) { /* RLE */
      if (i + 3 > n) break;
      const uint32_t count = (uint32_t)p[i] << 8 | p[i + 1];
      fill_record(image, size, offset, p[i + 2], count, &written);
      i += 3;
    } else {
      if (i + len > n) break;
      write_record(image, size, offset, p + i, len, &written);
      i += len;
    }
  }
  return written;
}

/* IPSwitch text patches. */
static uint32_t apply_pchtxt(const char *text, uint64_t n, const uint8_t module_id[32], uint8_t *image, uint64_t size) {
  uint32_t written = 0;
  bool id_ok = false, enabled = true;
  int64_t shift = 0;
  uint64_t i = 0;
  while (i < n) {
    uint64_t end = i;
    while (end < n && text[end] != '\n') end++;
    const char *line = text + i;
    uint64_t len = end - i;
    i = end + 1;
    while (len > 0 && (line[0] == ' ' || line[0] == '\t')) { line++; len--; }
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) len--;
    if (len == 0 || (len >= 2 && line[0] == '/' && line[1] == '/')) continue;
    if (line[0] == '@') {
      if (len > 8 && memcmp(line, "@nsobid-", 8) == 0) {
        id_ok = mods_build_id_matches(line + 8, (uint32_t)(len - 8), module_id);
      } else if (len >= 8 && memcmp(line, "@enabled", 8) == 0) {
        enabled = true;
      } else if (len >= 9 && memcmp(line, "@disabled", 9) == 0) {
        enabled = false;
      } else if (len >= 5 && memcmp(line, "@stop", 5) == 0) {
        break;
      } else if (len > 18 && memcmp(line, "@flag offset_shift", 18) == 0) {
        const char *v = line + 18;
        uint64_t vl = len - 18;
        while (vl > 0 && *v == ' ') { v++; vl--; }
        int64_t value = 0;
        bool neg = false;
        if (vl > 0 && *v == '-') { neg = true; v++; vl--; }
        if (vl > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
          for (uint64_t k = 2; k < vl && hex_value(v[k]) >= 0; k++) value = value * 16 + hex_value(v[k]);
        } else {
          for (uint64_t k = 0; k < vl && v[k] >= '0' && v[k] <= '9'; k++) value = value * 10 + (v[k] - '0');
        }
        shift = neg ? -value : value;
      }
      continue;
    }
    if (!id_ok || !enabled) continue;
    /* "<hex offset> <hex bytes>" or "<hex offset> "string"". */
    uint64_t k = 0;
    uint64_t offset = 0;
    uint32_t digits = 0;
    while (k < len && hex_value(line[k]) >= 0 && digits < 16) { offset = offset << 4 | (uint64_t)hex_value(line[k]); k++; digits++; }
    if (digits == 0 || k >= len || (line[k] != ' ' && line[k] != '\t')) continue;
    while (k < len && (line[k] == ' ' || line[k] == '\t')) k++;
    uint8_t bytes[512];
    uint32_t count = 0;
    if (k < len && line[k] == '"') {
      k++;
      while (k < len && line[k] != '"' && count < sizeof(bytes)) {
        char c = line[k++];
        if (c == '\\' && k < len) {
          const char e = line[k++];
          c = e == 'n' ? '\n' : e == '0' ? '\0' : e == 't' ? '\t' : e;
        }
        bytes[count++] = (uint8_t)c;
      }
    } else {
      while (k + 1 < len && count < sizeof(bytes) && hex_value(line[k]) >= 0 && hex_value(line[k + 1]) >= 0) {
        bytes[count++] = (uint8_t)(hex_value(line[k]) << 4 | hex_value(line[k + 1]));
        k += 2;
      }
    }
    const int64_t at = (int64_t)offset + shift;
    if (count > 0 && at >= 0) write_record(image, size, (uint64_t)at, bytes, count, &written);
  }
  return written;
}

uint32_t mods_apply_patch(const uint8_t *patch, uint64_t patch_size, const uint8_t module_id[32], uint8_t *image,
                          uint64_t size) {
  if (patch_size >= 5 && memcmp(patch, "IPS32", 5) == 0) return apply_ips(patch, patch_size, true, image, size);
  if (patch_size >= 5 && memcmp(patch, "PATCH", 5) == 0) return apply_ips(patch, patch_size, false, image, size);
  return apply_pchtxt((const char *)patch, patch_size, module_id, image, size);
}

/* The file-name part of a path. */
static const char *base_name(const Mod_File *f, uint32_t *length) {
  const char *slash = NULL;
  for (uint32_t i = 0; i < f->path_length; i++) if (f->path[i] == '/') slash = &f->path[i];
  const char *name = slash ? slash + 1 : f->path;
  *length = (uint32_t)(f->path + f->path_length - name);
  return name;
}

uint32_t mods_patch_module(const Mod_Set *set, const char *module_name, const uint8_t module_id[32], uint8_t *image,
                           uint64_t size, Arena *scratch) {
  if (!set || !set->live) return 0;
  uint32_t applied = 0;
  const size_t mark = scratch->used_bytes;
  for (uint32_t i = 0; i < set->count; i++) {
    const Mod_File *f = &set->files[i];
    const bool ips = has_suffix(f, ".ips");
    const bool pchtxt = has_suffix(f, ".pchtxt");
    if (!ips && !pchtxt) continue;
    uint32_t name_length = 0;
    const char *name = base_name(f, &name_length);
    if (ips && !mods_build_id_matches(name, name_length, module_id)) continue;
    uint8_t *bytes = NULL;
    uint64_t n = 0;
    Error err = mod_file_read_all(f, scratch, &bytes, &n);
    if (!error_is_ok(err)) {
      log_warn("[mods] %s: %s", f->path, err.message);
      scratch->used_bytes = mark;
      continue;
    }
    const uint32_t records = mods_apply_patch(bytes, n, module_id, image, size);
    scratch->used_bytes = mark;
    if (records > 0) {
      log_notice("mods: patch %s applied to %s (%u change%s)", f->path, module_name, records, records == 1 ? "" : "s");
      applied++;
    }
  }
  return applied;
}
