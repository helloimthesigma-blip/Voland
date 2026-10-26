/**
 * Mods: the guest-side data the user layers over a title (docs/DESIGN.md
 * §19). Voland ships the mechanism only - the platform hands the core a
 * list of files, the core applies them at load. Three kinds, told apart by
 * the file's path inside the set (the platform normalizes Atmosphere's and
 * other common folder layouts into these before adding):
 *
 *   romfs/<path>           LayeredFS: replaces or adds <path> in the
 *                          title's RomFS (layeredfs_build below)
 *   exefs/<module>         replaces an ExeFS file outright: main, rtld,
 *                          sdk, subsdk0..9, main.npdm
 *   exefs/<build id>.ips   IPS / IPS32 patch for the NSO whose module id
 *                          starts with <build id> (hex); `*.pchtxt` text
 *                          patches name their NSO with `@nsobid-<hex>`
 *   cheats/<build id>.txt  Atmosphere-format cheat list for the main NSO
 *                          (hle/cheat_vm.h runs them)
 *
 * Priority: the first file added under a path wins, so the platform adds
 * its highest-priority mod first. Everything else about a mod - names,
 * enable flags, ordering, where the bytes live - is the platform's.
 *
 * The bytes never cross into the core as a whole: Mod_Set.read pulls
 * ranges on demand (OPFS / disk), exactly like the game file (§15). All
 * of it is untrusted input for memory safety (§19): patch records are
 * bounds-checked against the image, paths against the caps below.
 */
#ifndef SWITCH_HLE_LOADER_MODS_H
#define SWITCH_HLE_LOADER_MODS_H

#include <stdbool.h>
#include <stdint.h>

#include "common/arena.h"
#include "common/result.h"
#include "hle/loader/byte_source.h"

#define MODS_MAX_FILES 65536u
#define MODS_MAX_PATH_BYTES 0x300u
#define MODS_ARENA_BYTES ((size_t)16 * 1024 * 1024)
/* Set-path prefix of pre-decrypted NCAs in a mod (game updates). */
#define MODS_NCA_PREFIX "nca/"
/* A patch or cheat file is read whole; real ones are a few KB. */
#define MODS_MAX_TEXT_FILE_BYTES ((uint64_t)4 * 1024 * 1024)
/* Horizon NSO header: IPS offsets count from the start of the NSO file,
 * so an image offset is the patch offset minus this. */
#define MODS_NSO_HEADER_BYTES 0x100u

typedef Error (*Mod_Read_Fn)(void *user, uint32_t file_id, uint64_t offset, void *out, uint64_t size);

struct Mod_Set;

typedef struct Mod_File {
  const char *path; /* NUL-terminated, in the set's arena */
  uint32_t path_length;
  uint32_t id;      /* the platform's handle, passed back to read */
  uint64_t size;
  Byte_Source source; /* reads this file through the set */
  const struct Mod_Set *set;
} Mod_File;

typedef struct Mod_Set {
  Arena arena;
  bool live;
  Mod_File *files;
  uint32_t count;
  Mod_Read_Fn read;
  void *user;
} Mod_Set;

/* Sets the read callback. Does not touch the file list. */
void mod_set_bind(Mod_Set *set, Mod_Read_Fn read, void *user);

/* Drops every file (and the arena). */
void mod_set_clear(Mod_Set *set);

/* Adds a file. A path already present is ignored (the earlier add has
 * priority) and returns OK. A leading "/" is dropped and "\" becomes "/".
 *   RESULT_INVALID_ARGUMENT empty or over-long path, "." / ".." component
 *   RESULT_OUT_OF_MEMORY    MODS_MAX_FILES or the arena exhausted */
Error mod_set_add(Mod_Set *set, const char *path, uint64_t size, uint32_t id);

/* Maps a file of a mod folder, as the user unpacked it, to its set path:
 *   atmosphere/contents/<title>/romfs/x  ->  romfs/x    (other titles: skipped)
 *   <title>/exefs/main, exefs/main       ->  exefs/main
 *   [atmosphere/]exefs_patches/<n>/<id>.ips, any *.ips / *.pchtxt
 *                                         ->  exefs/<mod>/<file>
 *   cheats/<id>.txt                       ->  cheats/<mod>/<file>
 *   romfs/x (at any depth)                ->  romfs/x
 *   any *.nca (a game update)             ->  nca/<mod>/<file>
 * Folder names match case-insensitively; what follows romfs/ keeps its
 * case (RomFS lookups are byte-exact). Patches and cheats keep the mod's
 * name so two mods' files for one build id both apply. Returns false for
 * files that are none of these (readmes, images) - the caller skips them. */
bool mods_normalize_path(const char *mod_name, const char *path, uint64_t title_id, char *out, size_t out_size);

/* Exact path lookup; NULL when absent. */
const Mod_File *mod_set_find(const Mod_Set *set, const char *path);

/* Number of files whose path starts with `prefix`. */
uint32_t mod_set_count_prefix(const Mod_Set *set, const char *prefix);

/* Reads a whole small file (patches, cheats) into `arena`, NUL-terminated.
 * RESULT_INVALID_ARGUMENT over MODS_MAX_TEXT_FILE_BYTES. */
Error mod_file_read_all(const Mod_File *file, Arena *arena, uint8_t **out, uint64_t *size);

/* ------------------------------------------------------------------ */
/* LayeredFS.                                                          */
/* ------------------------------------------------------------------ */

typedef struct Layered_Segment {
  uint64_t start;           /* in the layered image */
  uint64_t size;
  const Byte_Source *from;  /* NULL: `bytes` */
  uint64_t from_offset;
  const uint8_t *bytes;
} Layered_Segment;

typedef struct Layered_RomFS {
  Byte_Source source;       /* the merged image; reads through segments */
  Layered_Segment *segments;
  uint32_t segment_count;
  uint32_t replaced;        /* base files a mod overrides */
  uint32_t added;           /* files only a mod has */
} Layered_RomFS;

/* Builds a RomFS image that is `base` (may be NULL: a title without one)
 * with every `romfs/` file of `mods` laid over it. The image's tables and
 * segment list live in `arena`; file bytes stay where they are (base
 * source, mod files) and are read on demand. `base`, `mods` and `arena`
 * must outlive the result.
 *   RESULT_NOT_FOUND        no `romfs/` files in the set (use base as is)
 *   RESULT_INVALID_ARGUMENT a corrupt base RomFS
 *   RESULT_OUT_OF_MEMORY    arena exhausted */
Error layeredfs_build(const Byte_Source *base, const Mod_Set *mods, Arena *arena, Layered_RomFS *out);

/* ------------------------------------------------------------------ */
/* ExeFS patches.                                                      */
/* ------------------------------------------------------------------ */

/* Applies every IPS / IPS32 / pchtxt patch in the set whose build id
 * matches `module_id` to the module's memory image (`image`, `size`
 * bytes, offset 0 = the module base). Records outside the image are
 * skipped. `scratch` is rewound before returning. Returns the number of
 * patch files applied. */
uint32_t mods_patch_module(const Mod_Set *set, const char *module_name, const uint8_t module_id[32],
                           uint8_t *image, uint64_t size, Arena *scratch);

/* True when a patch file's hex build id (from its file name, up to the
 * first non-hex character) is a prefix of `module_id`. Exposed for
 * tests and for the cheat loader, which uses the same convention. */
bool mods_build_id_matches(const char *hex, uint32_t hex_length, const uint8_t module_id[32]);

/* Applies one patch buffer (IPS, IPS32 or pchtxt, detected from its
 * content). For pchtxt, `module_id` must match its @nsobid or nothing is
 * applied. Returns the number of records written. */
uint32_t mods_apply_patch(const uint8_t *patch, uint64_t patch_size, const uint8_t module_id[32],
                          uint8_t *image, uint64_t size);

#endif /* SWITCH_HLE_LOADER_MODS_H */
