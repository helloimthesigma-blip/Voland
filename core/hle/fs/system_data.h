/**
 * System data archives a title mounts by data id (fsp-srv
 * OpenDataStorageByDataId), synthesized - Voland ships no Nintendo system
 * content (§1.6). An archive is listed here only when a title refuses to
 * run without it and a stand-in of Voland's own making satisfies it:
 *
 *   0100000000000802 MiiModel: the Mii renderer's resources. A title
 *   that draws Miis (Mario Kart 8 Deluxe) mounts it at start-up and aborts
 *   when it is missing. The stand-in has the archive's files, each holding
 *   only a resource header (its signature and version, no model data), so
 *   the mount and the opens succeed; Miis render without textures/shapes.
 *
 * The archives are RomFS images built once into static storage; the
 * returned source stays valid for the process.
 */
#ifndef SWITCH_HLE_FS_SYSTEM_DATA_H
#define SWITCH_HLE_FS_SYSTEM_DATA_H

#include <stdbool.h>
#include <stdint.h>

#include "hle/loader/byte_source.h"

#define SYSTEM_DATA_MII_MODEL ((uint64_t)0x0100000000000802ull)

/* True (and *out set) when Voland synthesizes `data_id`. */
bool system_data_open(uint64_t data_id, Byte_Source *out);

/* A RomFS image with the given files in its root directory, into `out`
 * (at most `capacity` bytes). Returns the image size, 0 when it does not
 * fit. Exposed for tests. */
typedef struct System_Data_File {
  const char *name;
  const uint8_t *bytes;
  uint32_t size;
} System_Data_File;
uint64_t system_data_build_romfs(const System_Data_File *files, uint32_t count, uint8_t *out, uint64_t capacity);

#endif /* SWITCH_HLE_FS_SYSTEM_DATA_H */
