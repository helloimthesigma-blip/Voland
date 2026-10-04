/**
 * Save archives (§15 persistence): one save filesystem's whole tree as a
 * single byte string, so the host stores a save as ONE file - written
 * atomically (temp + swap) - and never half of one. Taken when the guest
 * commits its save (IFileSystem::Commit), which is when Horizon makes
 * save data durable; uncommitted writes are not persisted, as on a Switch.
 *
 *   Archive (little-endian):
 *     u32 magic SAVE_ARCHIVE_MAGIC ("VSAV"), u32 version, u32 entry count, u32 reserved
 *     per entry: u32 kind (0 directory, 1 file), u32 path bytes, path (absolute,
 *                no NUL), u64 size (files), then `size` bytes of data
 *   Parents come before children (depth-first order), so a restore can
 *   create them in order.
 */
#ifndef SWITCH_HLE_FS_SAVE_ARCHIVE_H
#define SWITCH_HLE_FS_SAVE_ARCHIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "hle/fs/ramfs.h"

#define SAVE_ARCHIVE_MAGIC 0x56415356u /* "VSAV" */
#define SAVE_ARCHIVE_VERSION 1u
#define SAVE_ARCHIVE_HEADER_BYTES 16u
#define SAVE_ARCHIVE_KIND_DIRECTORY 0u
#define SAVE_ARCHIVE_KIND_FILE 1u

/* Where archive bytes go: `write` appends; false = stop (out of room). */
typedef struct Save_Archive_Sink {
  void *user;
  bool (*write)(void *user, const void *bytes, uint64_t size);
} Save_Archive_Sink;

/* Streams the tree under `root` into `sink`. Returns the archive's size,
 * or 0 if the sink refused or the tree is deeper than the walker allows. */
uint64_t save_archive_write(const Ramfs_Pool *pool, uint32_t root, const Save_Archive_Sink *sink);

/* True if `bytes` is a well-formed archive (checked before any change). */
bool save_archive_valid(const uint8_t *bytes, uint64_t size);

/* Replaces the tree under `root` with the archive's contents. The archive
 * is validated first; an invalid one changes nothing (FS_RESULT_INVALID_PATH).
 * Returns 0 or a Horizon fs result. */
uint32_t save_archive_restore(Ramfs_Pool *pool, uint32_t root, const uint8_t *bytes, uint64_t size);

#endif /* SWITCH_HLE_FS_SAVE_ARCHIVE_H */
