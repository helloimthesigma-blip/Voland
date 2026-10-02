/**
 * fsp-srv - the filesystem service (§12, §15). Registered as "fsp-srv".
 *
 * IFileSystemProxy:
 *   1 SetCurrentProcess, 18 OpenSdCardFileSystem, 51/52/53 Open*SaveData-
 *   FileSystem (created on first open - Voland has no separate creation
 *   step to fail), 22/23 Create*SaveDataFileSystem (succeed; idempotent),
 *   200 OpenDataStorageByCurrentProcess (the program's RomFS - an NRO's
 *   asset RomFS or the program NCA's RomFS section, as raw bytes the
 *   title parses itself), 202 OpenDataStorageByDataId (system archives:
 *   not shipped, TargetNotFound - §1.6), 203 OpenPatchDataStorage-
 *   ByCurrentProcess (same as 200; no updates yet), 1003/1004/1005/1006/
 *   1011 access-log and auto-creation knobs.
 * IFileSystem: Create/Delete File/Directory(+Recursively), Rename*,
 *   GetEntryType, OpenFile, OpenDirectory, Commit, Get{Free,Total}Space-
 *   Size, CleanDirectoryRecursively, GetFileTimeStampRaw (zero times).
 * IFile: Read, Write (extends only with OpenMode_Append), Flush, SetSize,
 *   GetSize, OperateRange (zeros).
 * IDirectory: Read (0x310-byte DirectoryEntry records), GetEntryCount.
 * IStorage: Read, GetSize (read-only; Write is UnsupportedOperation).
 *
 * Backing store: ramfs (hle/fs/ramfs.h) - the SD card and every save
 * live in one pool owned by the Emulator, so they persist across program
 * reloads in a session. Persisting them to OPFS / a host directory is the
 * platform's job (Phase 6 save management); the CLI can seed the SD card
 * from a host directory. Paths arrive in 0x301-byte X buffers.
 */
#ifndef SWITCH_HLE_SERVICES_FS_FS_H
#define SWITCH_HLE_SERVICES_FS_FS_H

#include <stdbool.h>
#include <stdint.h>

#include "hle/fs/ramfs.h"
#include "hle/kernel/ipc.h"
#include "hle/loader/byte_source.h"
#include "hle/services/sm/sm.h"

#define FS_MAX_PATH_BYTES 0x301u
#define FS_MAX_OPEN_FILES 256u
#define FS_MAX_OPEN_DIRECTORIES 64u
#define FS_MAX_SAVES 32u
#define FS_BOUNCE_BYTES 0x10000u
#define FS_DIRECTORY_ENTRY_BYTES 0x310u
#define FS_OPEN_MODE_READ 1u
#define FS_OPEN_MODE_WRITE 2u
#define FS_OPEN_MODE_APPEND 4u
#define FS_DIR_MODE_DIRS 1u
#define FS_DIR_MODE_FILES 2u
#define FS_STORAGE_CONTENT_ROMFS 0u /* IStorage object state */

typedef struct Fs_Open_File {
  bool used;
  uint32_t node;
  uint32_t mode;
} Fs_Open_File;

typedef struct Fs_Open_Directory {
  bool used;
  uint32_t node;
  uint32_t mode;
  uint32_t cursor; /* next child node to return, RAMFS_NO_NODE at the end */
} Fs_Open_Directory;

typedef struct Fs_Save {
  bool used;
  uint8_t space;
  uint8_t key[0x40]; /* SaveDataAttribute as sent */
  uint32_t root;
} Fs_Save;

#define FS_MAX_BIS_PARTITIONS 8u

typedef struct Fs_Bis {
  uint32_t partition; /* BisPartitionId (0 = unused slot) */
  uint32_t root;
} Fs_Bis;

typedef struct Fs_State {
  Service_Interface proxy;
  Service_Interface filesystem;  /* object state: ramfs root node */
  Service_Interface file;        /* object state: open-file slot */
  Service_Interface directory;   /* object state: open-directory slot */
  Service_Interface storage;     /* object state: FS_STORAGE_* */
  Ramfs_Pool *pool;              /* the Emulator's; outlives processes */
  uint32_t sd_root;
  Fs_Save saves[FS_MAX_SAVES];   /* survive reloads with the pool */
  Fs_Bis bis[FS_MAX_BIS_PARTITIONS]; /* empty NAND partitions (§1.6: no NAND image) */
  const Byte_Source *romfs;      /* the program's RomFS bytes, or NULL */
  Fs_Open_File files[FS_MAX_OPEN_FILES];
  Fs_Open_Directory directories[FS_MAX_OPEN_DIRECTORIES];
  uint8_t bounce[FS_BOUNCE_BYTES];
} Fs_State;

/* Once per Emulator: interfaces, the SD card root, no saves. */
void fs_init(Fs_State *state, Ramfs_Pool *pool);
/* Per process: closes every open file/directory and sets the RomFS. */
void fs_reset_process(Fs_State *state, const Byte_Source *romfs);
Error fs_register(Fs_State *state, SM_Registry *registry);

#endif /* SWITCH_HLE_SERVICES_FS_FS_H */
