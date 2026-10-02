/**
 * fsp-srv over ramfs and the program's RomFS. See fs.h.
 */
#include "hle/services/fs/fs.h"

#include "common/log.h"
#include "hle/services/service_util.h"

#define FS_SAVE_ATTRIBUTE_BYTES 0x40u
#define FS_TIMESTAMP_BYTES 0x20u
#define FS_RANGE_INFO_BYTES 0x40u
#define FS_ENTRY_TYPE_OFFSET 0x304u
#define FS_ENTRY_SIZE_OFFSET 0x308u
#define FS_ACCESS_LOG_MODE_NONE 0u

static Fs_State *state_of(Service_Object *self) { return (Fs_State *)self->interface->service_state; }

/* Path from X buffer `index` (NUL-terminated, at most 0x300 chars). */
static uint32_t read_path(HLE_Context *c, const IPC_Request *req, uint32_t index, char out[FS_MAX_PATH_BYTES]) {
  memset(out, 0, FS_MAX_PATH_BYTES);
  if (index >= req->static_count) return FS_RESULT_INVALID_PATH;
  const IPC_Buffer *buf = &req->statics[index];
  const uint64_t n = buf->size < FS_MAX_PATH_BYTES - 1u ? buf->size : FS_MAX_PATH_BYTES - 1u;
  if (n && !error_is_ok(vmm_read_block(c->vmm, buf->gva, out, n))) return FS_RESULT_INVALID_PATH;
  return 0;
}

/* ------------------------------------------------------------------ */
/* IFileSystemProxy.                                                   */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_open_sd_card(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)req;
  Fs_State *s = state_of(self);
  if (s->sd_root == RAMFS_NO_NODE) return FS_RESULT_TARGET_NOT_FOUND; /* no RAM for it */
  (void)ipc_response_push_object(res, &s->filesystem, s->sd_root);
  return HLE_RESULT_SUCCESS;
}

/* {u8 space, pad[7], SaveDataAttribute attr (0x40)}: find or create. */
static HLE_ServiceResult cmd_open_save_data(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  Fs_State *s = state_of(self);
  if (!s->pool) return FS_RESULT_TARGET_NOT_FOUND;
  uint8_t in[8 + FS_SAVE_ATTRIBUTE_BYTES];
  if (!error_is_ok(ipc_request_read_bytes(req, 0, in, sizeof(in)))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  Fs_Save *free_slot = NULL;
  for (uint32_t i = 0; i < FS_MAX_SAVES; i++) {
    Fs_Save *save = &s->saves[i];
    if (!save->used) {
      if (!free_slot) free_slot = save;
      continue;
    }
    if (save->space == in[0] && memcmp(save->key, in + 8, FS_SAVE_ATTRIBUTE_BYTES) == 0) {
      (void)ipc_response_push_object(res, &s->filesystem, save->root);
      return HLE_RESULT_SUCCESS;
    }
  }
  if (!free_slot) return FS_RESULT_ALLOCATION_TABLE_FULL;
  const uint32_t rc = ramfs_create_filesystem(s->pool, &free_slot->root);
  if (rc) return rc;
  free_slot->used = true;
  free_slot->space = in[0];
  memcpy(free_slot->key, in + 8, FS_SAVE_ATTRIBUTE_BYTES);
  (void)ipc_response_push_object(res, &s->filesystem, free_slot->root);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_open_data_storage_self(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                    IPC_Response *res) {
  (void)c;
  (void)req;
  Fs_State *s = state_of(self);
  if (!s->romfs) return FS_RESULT_TARGET_NOT_FOUND;
  (void)ipc_response_push_object(res, &s->storage, FS_STORAGE_CONTENT_ROMFS);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_open_data_storage_by_id(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                     IPC_Response *res) {
  (void)c;
  (void)self;
  (void)res;
  uint64_t data_id = 0;
  (void)ipc_request_read_u64(req, 8, &data_id);
  log_warn("[fs] OpenDataStorageByDataId(%016llx): system archives are not shipped (§1.6)",
           (unsigned long long)data_id);
  return FS_RESULT_TARGET_NOT_FOUND;
}

static HLE_ServiceResult cmd_get_access_log_mode(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                 IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, FS_ACCESS_LOG_MODE_NONE);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_program_index(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 0); /* index */
  (void)ipc_response_push_u32(res, 1); /* count */
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* IFileSystem.                                                        */
/* ------------------------------------------------------------------ */

#define FS_ROOT(self) ((uint32_t)(self)->state)

static HLE_ServiceResult cmd_create_file(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)res;
  char path[FS_MAX_PATH_BYTES];
  uint64_t size = 0;
  if (!error_is_ok(ipc_request_read_u64(req, 8, &size))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  const uint32_t rc = read_path(c, req, 0, path);
  return rc ? rc : ramfs_create_file(state_of(self)->pool, FS_ROOT(self), path, size);
}

static HLE_ServiceResult cmd_delete_file(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)res;
  char path[FS_MAX_PATH_BYTES];
  const uint32_t rc = read_path(c, req, 0, path);
  return rc ? rc : ramfs_delete_file(state_of(self)->pool, FS_ROOT(self), path);
}

static HLE_ServiceResult cmd_create_directory(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)res;
  char path[FS_MAX_PATH_BYTES];
  const uint32_t rc = read_path(c, req, 0, path);
  return rc ? rc : ramfs_create_directory(state_of(self)->pool, FS_ROOT(self), path);
}

static HLE_ServiceResult delete_directory(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          bool recursive, bool clean_only) {
  char path[FS_MAX_PATH_BYTES];
  const uint32_t rc = read_path(c, req, 0, path);
  return rc ? rc : ramfs_delete_directory(state_of(self)->pool, FS_ROOT(self), path, recursive, clean_only);
}

static HLE_ServiceResult cmd_delete_directory(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)res;
  return delete_directory(c, self, req, false, false);
}

static HLE_ServiceResult cmd_delete_directory_recursively(HLE_Context *c, Service_Object *self,
                                                          const IPC_Request *req, IPC_Response *res) {
  (void)res;
  return delete_directory(c, self, req, true, false);
}

static HLE_ServiceResult cmd_clean_directory_recursively(HLE_Context *c, Service_Object *self,
                                                         const IPC_Request *req, IPC_Response *res) {
  (void)res;
  return delete_directory(c, self, req, true, true);
}

static HLE_ServiceResult rename_entry(HLE_Context *c, Service_Object *self, const IPC_Request *req, bool directory) {
  char from[FS_MAX_PATH_BYTES], to[FS_MAX_PATH_BYTES];
  uint32_t rc = read_path(c, req, 0, from);
  if (!rc) rc = read_path(c, req, 1, to);
  return rc ? rc : ramfs_rename(state_of(self)->pool, FS_ROOT(self), from, to, directory);
}

static HLE_ServiceResult cmd_rename_file(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)res;
  return rename_entry(c, self, req, false);
}

static HLE_ServiceResult cmd_rename_directory(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)res;
  return rename_entry(c, self, req, true);
}

static HLE_ServiceResult cmd_get_entry_type(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  Fs_State *s = state_of(self);
  char path[FS_MAX_PATH_BYTES];
  uint32_t node = 0;
  uint32_t rc = read_path(c, req, 0, path);
  if (!rc) rc = ramfs_lookup(s->pool, FS_ROOT(self), path, &node);
  if (rc) return rc;
  (void)ipc_response_push_u32(res, s->pool->nodes[node].is_dir ? RAMFS_DIRECTORY : RAMFS_FILE);
  return HLE_RESULT_SUCCESS;
}

static void file_on_close(void *service_state, uint64_t object_state) {
  Fs_State *s = (Fs_State *)service_state;
  if (object_state >= FS_MAX_OPEN_FILES || !s->files[object_state].used) return;
  Ramfs_Node *node = &s->pool->nodes[s->files[object_state].node];
  if (node->open_count) node->open_count--;
  memset(&s->files[object_state], 0, sizeof(s->files[object_state]));
}

static HLE_ServiceResult cmd_open_file(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  Fs_State *s = state_of(self);
  uint32_t mode = 0;
  if (!error_is_ok(ipc_request_read_u32(req, 0, &mode))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  if (!(mode & (FS_OPEN_MODE_READ | FS_OPEN_MODE_WRITE))) return FS_RESULT_INVALID_OPEN_MODE;
  char path[FS_MAX_PATH_BYTES];
  uint32_t node = 0;
  uint32_t rc = read_path(c, req, 0, path);
  if (!rc) rc = ramfs_lookup(s->pool, FS_ROOT(self), path, &node);
  if (rc) return rc;
  if (s->pool->nodes[node].is_dir) return FS_RESULT_PATH_NOT_FOUND;
  for (uint32_t i = 0; i < FS_MAX_OPEN_FILES; i++) {
    if (s->files[i].used) continue;
    s->files[i] = (Fs_Open_File){true, node, mode};
    s->pool->nodes[node].open_count++;
    (void)ipc_response_push_object(res, &s->file, i);
    return HLE_RESULT_SUCCESS;
  }
  return FS_RESULT_ALLOCATION_TABLE_FULL;
}

static void directory_on_close(void *service_state, uint64_t object_state) {
  Fs_State *s = (Fs_State *)service_state;
  if (object_state < FS_MAX_OPEN_DIRECTORIES) memset(&s->directories[object_state], 0, sizeof(s->directories[0]));
}

static HLE_ServiceResult cmd_open_directory(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  Fs_State *s = state_of(self);
  uint32_t mode = 0;
  if (!error_is_ok(ipc_request_read_u32(req, 0, &mode))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  char path[FS_MAX_PATH_BYTES];
  uint32_t node = 0;
  uint32_t rc = read_path(c, req, 0, path);
  if (!rc) rc = ramfs_lookup(s->pool, FS_ROOT(self), path, &node);
  if (rc) return rc;
  if (!s->pool->nodes[node].is_dir) return FS_RESULT_PATH_NOT_FOUND;
  for (uint32_t i = 0; i < FS_MAX_OPEN_DIRECTORIES; i++) {
    if (s->directories[i].used) continue;
    s->directories[i] = (Fs_Open_Directory){true, node, mode, s->pool->nodes[node].first_child};
    (void)ipc_response_push_object(res, &s->directory, i);
    return HLE_RESULT_SUCCESS;
  }
  return FS_RESULT_ALLOCATION_TABLE_FULL;
}

static HLE_ServiceResult cmd_get_free_space(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u64(res, ramfs_free_bytes(state_of(self)->pool));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_total_space(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u64(res, ramfs_total_bytes(state_of(self)->pool));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_timestamp(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  Fs_State *s = state_of(self);
  char path[FS_MAX_PATH_BYTES];
  uint32_t node = 0;
  uint32_t rc = read_path(c, req, 0, path);
  if (!rc) rc = ramfs_lookup(s->pool, FS_ROOT(self), path, &node);
  if (rc) return rc;
  uint8_t stamp[FS_TIMESTAMP_BYTES];
  memset(stamp, 0, sizeof(stamp)); /* created/modified/accessed 0, is_valid 0 */
  (void)ipc_response_push_bytes(res, stamp, sizeof(stamp));
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* IFile.                                                              */
/* ------------------------------------------------------------------ */

static Fs_Open_File *file_of(Service_Object *self) {
  Fs_State *s = state_of(self);
  return self->state < FS_MAX_OPEN_FILES && s->files[self->state].used ? &s->files[self->state] : NULL;
}

/* {u32 option, pad, s64 offset, u64 size} + B buffer -> s64 bytes read. */
static HLE_ServiceResult cmd_file_read(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  Fs_State *s = state_of(self);
  const Fs_Open_File *f = file_of(self);
  if (!f) return HLE_RESULT_INVALID_STATE;
  if (!(f->mode & FS_OPEN_MODE_READ)) return FS_RESULT_INVALID_OPEN_MODE;
  uint64_t offset = 0, size = 0;
  if (!error_is_ok(ipc_request_read_u64(req, 8, &offset)) || !error_is_ok(ipc_request_read_u64(req, 16, &size))) {
    return IPC_RESULT_SF_INVALID_IN_HEADER;
  }
  const IPC_Buffer *buf = service_out_buffer(req, 0);
  if (!buf) return FS_RESULT_OUT_OF_RANGE;
  if (size > buf->size) size = buf->size;
  uint64_t total = 0;
  while (total < size) {
    const uint64_t want = size - total < FS_BOUNCE_BYTES ? size - total : FS_BOUNCE_BYTES;
    uint64_t got = 0;
    const uint32_t rc = ramfs_read(s->pool, f->node, offset + total, s->bounce, want, &got);
    if (rc) return rc;
    if (!got) break;
    if (!error_is_ok(vmm_write_block(c->vmm, buf->gva + total, s->bounce, got))) return HLE_RESULT_INVALID_POINTER;
    total += got;
  }
  (void)ipc_response_push_u64(res, total);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_file_write(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)res;
  Fs_State *s = state_of(self);
  const Fs_Open_File *f = file_of(self);
  if (!f) return HLE_RESULT_INVALID_STATE;
  if (!(f->mode & FS_OPEN_MODE_WRITE)) return FS_RESULT_INVALID_OPEN_MODE;
  uint64_t offset = 0, size = 0;
  if (!error_is_ok(ipc_request_read_u64(req, 8, &offset)) || !error_is_ok(ipc_request_read_u64(req, 16, &size))) {
    return IPC_RESULT_SF_INVALID_IN_HEADER;
  }
  const IPC_Buffer *buf = service_in_buffer(req, 0);
  if (!buf) return size ? FS_RESULT_OUT_OF_RANGE : HLE_RESULT_SUCCESS;
  if (size > buf->size) size = buf->size;
  if (offset + size > s->pool->nodes[f->node].size && !(f->mode & FS_OPEN_MODE_APPEND)) return FS_RESULT_NEED_APPEND;
  for (uint64_t done = 0; done < size;) {
    const uint64_t chunk = size - done < FS_BOUNCE_BYTES ? size - done : FS_BOUNCE_BYTES;
    if (!error_is_ok(vmm_read_block(c->vmm, buf->gva + done, s->bounce, chunk))) return HLE_RESULT_INVALID_POINTER;
    const uint32_t rc = ramfs_write(s->pool, f->node, offset + done, s->bounce, chunk);
    if (rc) return rc;
    done += chunk;
  }
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_file_set_size(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)c;
  (void)res;
  const Fs_Open_File *f = file_of(self);
  if (!f) return HLE_RESULT_INVALID_STATE;
  if (!(f->mode & FS_OPEN_MODE_WRITE)) return FS_RESULT_INVALID_OPEN_MODE;
  uint64_t size = 0;
  if (!error_is_ok(ipc_request_read_u64(req, 0, &size))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  return ramfs_set_size(state_of(self)->pool, f->node, size);
}

static HLE_ServiceResult cmd_file_get_size(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)c;
  (void)req;
  const Fs_Open_File *f = file_of(self);
  if (!f) return HLE_RESULT_INVALID_STATE;
  (void)ipc_response_push_u64(res, state_of(self)->pool->nodes[f->node].size);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_out_range_info(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  uint8_t info[FS_RANGE_INFO_BYTES];
  memset(info, 0, sizeof(info));
  (void)ipc_response_push_bytes(res, info, sizeof(info));
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* IDirectory.                                                         */
/* ------------------------------------------------------------------ */

static bool wanted(const Ramfs_Node *n, uint32_t mode) {
  return n->is_dir ? (mode & FS_DIR_MODE_DIRS) != 0 : (mode & FS_DIR_MODE_FILES) != 0;
}

static HLE_ServiceResult cmd_directory_read(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  Fs_State *s = state_of(self);
  if (self->state >= FS_MAX_OPEN_DIRECTORIES || !s->directories[self->state].used) return HLE_RESULT_INVALID_STATE;
  Fs_Open_Directory *d = &s->directories[self->state];
  const IPC_Buffer *buf = service_out_buffer(req, 0);
  const uint64_t capacity = buf ? buf->size / FS_DIRECTORY_ENTRY_BYTES : 0;
  uint64_t count = 0;
  uint8_t entry[FS_DIRECTORY_ENTRY_BYTES];
  while (count < capacity && d->cursor != RAMFS_NO_NODE) {
    const Ramfs_Node *n = &s->pool->nodes[d->cursor];
    d->cursor = n->next_sibling;
    if (!wanted(n, d->mode)) continue;
    memset(entry, 0, sizeof(entry));
    memcpy(entry, n->name, strlen(n->name));
    entry[FS_ENTRY_TYPE_OFFSET] = n->is_dir ? RAMFS_DIRECTORY : RAMFS_FILE;
    const uint64_t size = n->is_dir ? 0 : n->size;
    memcpy(entry + FS_ENTRY_SIZE_OFFSET, &size, sizeof(size));
    if (!error_is_ok(vmm_write_block(c->vmm, buf->gva + count * FS_DIRECTORY_ENTRY_BYTES, entry, sizeof(entry)))) {
      return HLE_RESULT_INVALID_POINTER;
    }
    count++;
  }
  (void)ipc_response_push_u64(res, count);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_directory_count(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)req;
  Fs_State *s = state_of(self);
  if (self->state >= FS_MAX_OPEN_DIRECTORIES || !s->directories[self->state].used) return HLE_RESULT_INVALID_STATE;
  const Fs_Open_Directory *d = &s->directories[self->state];
  uint64_t count = 0;
  for (uint32_t n = s->pool->nodes[d->node].first_child; n != RAMFS_NO_NODE; n = s->pool->nodes[n].next_sibling) {
    if (wanted(&s->pool->nodes[n], d->mode)) count++;
  }
  (void)ipc_response_push_u64(res, count);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* IStorage (the program's RomFS).                                     */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_storage_read(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)res;
  Fs_State *s = state_of(self);
  if (!s->romfs) return FS_RESULT_TARGET_NOT_FOUND;
  uint64_t offset = 0, size = 0;
  if (!error_is_ok(ipc_request_read_u64(req, 0, &offset)) || !error_is_ok(ipc_request_read_u64(req, 8, &size))) {
    return IPC_RESULT_SF_INVALID_IN_HEADER;
  }
  const IPC_Buffer *buf = service_out_buffer(req, 0);
  if (!buf) return size ? FS_RESULT_OUT_OF_RANGE : HLE_RESULT_SUCCESS;
  if (size > buf->size) size = buf->size;
  if (offset > s->romfs->size || size > s->romfs->size - offset) return FS_RESULT_OUT_OF_RANGE;
  for (uint64_t done = 0; done < size;) {
    const uint64_t chunk = size - done < FS_BOUNCE_BYTES ? size - done : FS_BOUNCE_BYTES;
    if (!error_is_ok(byte_source_read(s->romfs, offset + done, s->bounce, chunk))) return FS_RESULT_OUT_OF_RANGE;
    if (!error_is_ok(vmm_write_block(c->vmm, buf->gva + done, s->bounce, chunk))) return HLE_RESULT_INVALID_POINTER;
    done += chunk;
  }
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_storage_get_size(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)c;
  (void)req;
  const Fs_State *s = state_of(self);
  (void)ipc_response_push_u64(res, s->romfs ? s->romfs->size : 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_unsupported(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return FS_RESULT_UNSUPPORTED_OPERATION;
}

/* ------------------------------------------------------------------ */
/* Tables.                                                             */
/* ------------------------------------------------------------------ */

/* ISaveDataInfoReader: Voland keeps no save-data index yet, so readers
 * list nothing (homebrew save managers then show an empty list). */
static HLE_ServiceResult cmd_read_save_info(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u64(res, 0);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_save_info_reader_commands[] = {
    {0, cmd_read_save_info, "ReadSaveDataInfo"},
};

static Service_Interface g_save_info_reader =
    SERVICE_INTERFACE("ISaveDataInfoReader", k_save_info_reader_commands, 0, NULL);

static HLE_ServiceResult cmd_open_save_info_reader(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                   IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_object(res, &g_save_info_reader, 0);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_proxy_commands[] = {
    {1, service_cmd_ok, "SetCurrentProcess"},
    {18, cmd_open_sd_card, "OpenSdCardFileSystem"},
    {22, service_cmd_ok, "CreateSaveDataFileSystem"},
    {23, service_cmd_ok, "CreateSaveDataFileSystemBySystemSaveDataId"},
    {51, cmd_open_save_data, "OpenSaveDataFileSystem"},
    {52, cmd_open_save_data, "OpenSaveDataFileSystemBySystemSaveDataId"},
    {53, cmd_open_save_data, "OpenReadOnlySaveDataFileSystem"},
    {60, cmd_open_save_info_reader, "OpenSaveDataInfoReader"},
    {61, cmd_open_save_info_reader, "OpenSaveDataInfoReaderBySaveDataSpaceId"},
    {62, cmd_open_save_info_reader, "OpenSaveDataInfoReaderOnlyCacheStorage"},
    {68, cmd_open_save_info_reader, "OpenSaveDataInfoReaderWithFilter"},
    {200, cmd_open_data_storage_self, "OpenDataStorageByCurrentProcess"},
    {202, cmd_open_data_storage_by_id, "OpenDataStorageByDataId"},
    {203, cmd_open_data_storage_self, "OpenPatchDataStorageByCurrentProcess"},
    {1003, service_cmd_ok, "DisableAutoSaveDataCreation_stub"},
    {1004, service_cmd_ok, "SetGlobalAccessLogMode_stub"},
    {1005, cmd_get_access_log_mode, "GetGlobalAccessLogMode"},
    {1006, service_cmd_ok, "OutputAccessLogToSdCard_stub"},
    {1011, cmd_get_program_index, "GetProgramIndexForAccessLog"},
};

static const Service_Command k_filesystem_commands[] = {
    {0, cmd_create_file, "CreateFile"},
    {1, cmd_delete_file, "DeleteFile"},
    {2, cmd_create_directory, "CreateDirectory"},
    {3, cmd_delete_directory, "DeleteDirectory"},
    {4, cmd_delete_directory_recursively, "DeleteDirectoryRecursively"},
    {5, cmd_rename_file, "RenameFile"},
    {6, cmd_rename_directory, "RenameDirectory"},
    {7, cmd_get_entry_type, "GetEntryType"},
    {8, cmd_open_file, "OpenFile"},
    {9, cmd_open_directory, "OpenDirectory"},
    {10, service_cmd_ok, "Commit"},
    {11, cmd_get_free_space, "GetFreeSpaceSize"},
    {12, cmd_get_total_space, "GetTotalSpaceSize"},
    {13, cmd_clean_directory_recursively, "CleanDirectoryRecursively"},
    {14, cmd_get_timestamp, "GetFileTimeStampRaw"},
};

static const Service_Command k_file_commands[] = {
    {0, cmd_file_read, "Read"},
    {1, cmd_file_write, "Write"},
    {2, service_cmd_ok, "Flush"},
    {3, cmd_file_set_size, "SetSize"},
    {4, cmd_file_get_size, "GetSize"},
    {5, cmd_out_range_info, "OperateRange"},
};

static const Service_Command k_directory_commands[] = {
    {0, cmd_directory_read, "Read"},
    {1, cmd_directory_count, "GetEntryCount"},
};

static const Service_Command k_storage_commands[] = {
    {0, cmd_storage_read, "Read"},
    {1, cmd_unsupported, "Write"},
    {2, service_cmd_ok, "Flush"},
    {3, cmd_unsupported, "SetSize"},
    {4, cmd_storage_get_size, "GetSize"},
    {5, cmd_out_range_info, "OperateRange"},
};

void fs_init(Fs_State *s, Ramfs_Pool *pool) {
  memset(s, 0, sizeof(*s));
  s->pool = pool;
  s->proxy = SERVICE_INTERFACE("fsp-srv", k_proxy_commands, 0x800, s);
  s->filesystem = SERVICE_INTERFACE("IFileSystem", k_filesystem_commands, 0, s);
  s->file = SERVICE_INTERFACE("IFile", k_file_commands, 0, s);
  s->file.on_close = file_on_close;
  s->directory = SERVICE_INTERFACE("IDirectory", k_directory_commands, 0, s);
  s->directory.on_close = directory_on_close;
  s->storage = SERVICE_INTERFACE("IStorage", k_storage_commands, 0, s);
  s->sd_root = RAMFS_NO_NODE;
  if (pool && ramfs_create_filesystem(pool, &s->sd_root) != 0) s->sd_root = RAMFS_NO_NODE;
}

void fs_reset_process(Fs_State *s, const Byte_Source *romfs) {
  for (uint32_t i = 0; i < FS_MAX_OPEN_FILES && s->pool; i++) {
    if (s->files[i].used) file_on_close(s, i);
  }
  memset(s->files, 0, sizeof(s->files));
  memset(s->directories, 0, sizeof(s->directories));
  s->romfs = romfs;
}


Error fs_register(Fs_State *s, SM_Registry *registry) { return sm_registry_add(registry, "fsp-srv", &s->proxy); }
