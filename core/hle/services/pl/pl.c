#include "hle/services/pl/pl.h"

#include "hle/services/service_util.h"

#define PL_LOAD_STATE_LOADED 1u

static Pl_State *state_of(Service_Object *self) { return (Pl_State *)self->interface->service_state; }

static bool has_font(const Pl_State *s) {
  return s->font && s->font_size && s->font_size <= PL_SHARED_MEMORY_BYTES - PL_FONT_HEADER_BYTES;
}

static HLE_ServiceResult cmd_request_load(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)req;
  (void)res;
  return has_font(state_of(self)) ? HLE_RESULT_SUCCESS : HLE_RESULT_NOT_FOUND;
}

static HLE_ServiceResult cmd_get_load_state(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_u32(res, has_font(state_of(self)) ? PL_LOAD_STATE_LOADED : 0u);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_size(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                      IPC_Response *res) {
  (void)c;
  (void)req;
  const Pl_State *s = state_of(self);
  (void)ipc_response_push_u32(res, has_font(s) ? s->font_size : 0u);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_offset(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, PL_FONT_HEADER_BYTES);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_shared_memory(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)req;
  Pl_State *s = state_of(self);
  if (!s->shared_memory) {
    if (!error_is_ok(shared_memory_create(s->pool, c->vmm, PL_SHARED_MEMORY_BYTES, SHARED_MEMORY_PERM_R,
                                          &s->shared_memory))) {
      return HLE_RESULT_OUT_OF_MEMORY;
    }
    if (has_font(s)) {
      const uint32_t header[2] = {PL_FONT_MAGIC ^ PL_FONT_KEY, s->font_size ^ PL_FONT_KEY};
      (void)vmm_write_physical(c->vmm, s->shared_memory->guest_pa, header, sizeof(header));
      (void)vmm_write_physical(c->vmm, s->shared_memory->guest_pa + PL_FONT_HEADER_BYTES, s->font, s->font_size);
    }
  }
  uint32_t handle = 0;
  shared_memory_retain(s->shared_memory);
  if (!error_is_ok(handle_table_add(&c->process->handles, KERNEL_OBJECT_SHARED_MEMORY, s->shared_memory, &handle))) {
    shared_memory_release(s->pool, s->shared_memory);
    return HLE_RESULT_OUT_OF_HANDLES;
  }
  (void)ipc_response_push_copy_handle(res, handle);
  return HLE_RESULT_SUCCESS;
}

/* u64 language -> {u8 loaded, s32 total} + types/offsets/sizes (B). */
static HLE_ServiceResult cmd_fonts_in_priority(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  const Pl_State *s = state_of(self);
  const bool loaded = has_font(s);
  uint32_t types[PL_FONT_TYPE_COUNT], offsets[PL_FONT_TYPE_COUNT], sizes[PL_FONT_TYPE_COUNT];
  for (uint32_t i = 0; i < PL_FONT_TYPE_COUNT; i++) {
    types[i] = i;
    offsets[i] = PL_FONT_HEADER_BYTES;
    sizes[i] = loaded ? s->font_size : 0u;
  }
  (void)service_write_out(c, req, 0, types, sizeof(types));
  (void)service_write_out(c, req, 1, offsets, sizeof(offsets));
  (void)service_write_out(c, req, 2, sizes, sizeof(sizes));
  (void)ipc_response_push_u32(res, loaded ? 1u : 0u);
  (void)ipc_response_push_u32(res, loaded ? PL_FONT_TYPE_COUNT : 0u);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_pl_commands[] = {
    {0, cmd_request_load, "RequestLoad"},
    {1, cmd_get_load_state, "GetLoadState"},
    {2, cmd_get_size, "GetSize"},
    {3, cmd_get_offset, "GetSharedMemoryAddressOffset"},
    {4, cmd_get_shared_memory, "GetSharedMemoryNativeHandle"},
    {5, cmd_fonts_in_priority, "GetSharedFontInOrderOfPriority"},
};

void pl_init(Pl_State *s, Shared_Memory_Pool *pool, const uint8_t *font, uint32_t font_size) {
  memset(s, 0, sizeof(*s));
  s->pool = pool;
  s->font = font;
  s->font_size = font_size;
  s->interface = SERVICE_INTERFACE("pl:u", k_pl_commands, 0, s);
}

Error pl_register(Pl_State *s, SM_Registry *registry) {
  Error err = sm_registry_add(registry, "pl:u", &s->interface);
  if (error_is_ok(err)) err = sm_registry_add(registry, "pl:s", &s->interface);
  return err;
}
