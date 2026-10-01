/**
 * sm: service manager. See sm.h for commands, results and the stated
 * deviations.
 */
#include "hle/services/sm/sm.h"

#include "common/log.h"
#include "hle/hle.h"

#include <string.h>

#define SM_NAME_ARG_OFFSET 0u

static uint64_t name_to_wire(const char *name) {
  uint64_t wire = 0;
  for (uint32_t i = 0; i < SM_SERVICE_NAME_BYTES && name[i] != '\0'; i++) {
    wire |= (uint64_t)(uint8_t)name[i] << (8u * i);
  }
  return wire;
}

/* For log lines: the wire name as a C string. */
static void wire_to_text(uint64_t wire, char out[SM_SERVICE_NAME_BYTES + 1]) {
  for (uint32_t i = 0; i < SM_SERVICE_NAME_BYTES; i++) out[i] = (char)(uint8_t)(wire >> (8u * i));
  out[SM_SERVICE_NAME_BYTES] = '\0';
}

bool sm_service_name_is_valid(uint64_t name) {
  /* Non-empty, and nothing but NULs after the first NUL. */
  if ((name & 0xFFu) == 0) return false;
  bool terminated = false;
  for (uint32_t i = 0; i < SM_SERVICE_NAME_BYTES; i++) {
    const uint8_t byte = (uint8_t)(name >> (8u * i));
    if (byte == 0) terminated = true;
    else if (terminated) return false;
  }
  return true;
}

static HLE_ServiceResult sm_register_client(HLE_Context *context, Service_Object *self,
                                            const IPC_Request *request, IPC_Response *response) {
  (void)context;
  (void)request;
  (void)response;
  self->state |= SM_OBJECT_STATE_CLIENT_REGISTERED;
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult sm_get_service_handle(HLE_Context *context, Service_Object *self,
                                               const IPC_Request *request, IPC_Response *response) {
  if (!(self->state & SM_OBJECT_STATE_CLIENT_REGISTERED)) return SM_RESULT_INVALID_CLIENT;
  uint64_t name = 0;
  if (!error_is_ok(ipc_request_read_u64(request, SM_NAME_ARG_OFFSET, &name))) {
    return IPC_RESULT_SF_INVALID_HEADER_SIZE;
  }
  if (!sm_service_name_is_valid(name)) return SM_RESULT_INVALID_SERVICE_NAME;

  const SM_Registry *registry = (const SM_Registry *)self->interface->service_state;
  const Service_Interface *service = sm_registry_find(registry, name);
  char text[SM_SERVICE_NAME_BYTES + 1];
  wire_to_text(name, text);
  if (!service) {
    log_warn("[sm] GetServiceHandle(\"%s\"): no HLE service registered", text);
    return SM_RESULT_NOT_REGISTERED;
  }
  uint32_t handle = 0;
  const uint32_t result = ipc_open_session_handle(context, service, 0, &handle);
  if (result != HLE_RESULT_SUCCESS) return result;
  (void)ipc_response_push_move_handle(response, handle);
  log_info("[sm] GetServiceHandle(\"%s\") -> 0x%x", text, (unsigned)handle);
  return HLE_RESULT_SUCCESS;
}

/* Sorted by command_id (ipc.h bsearch). 2/3 (RegisterService /
 * UnregisterService) are sysmodule-only and absent on purpose: the
 * unknown-command policy logs them. */
static const Service_Command k_sm_commands[] = {
    {SM_COMMAND_REGISTER_CLIENT, sm_register_client, "RegisterClient"},
    {SM_COMMAND_GET_SERVICE_HANDLE, sm_get_service_handle, "GetServiceHandle"},
};

void sm_registry_init(SM_Registry *registry) {
  if (!registry) return;
  memset(registry, 0, sizeof(*registry));
  registry->interface.name = SM_PORT_NAME;
  registry->interface.commands = k_sm_commands;
  registry->interface.command_count = sizeof(k_sm_commands) / sizeof(k_sm_commands[0]);
  registry->interface.pointer_buffer_size = 0;
  registry->interface.service_state = registry;
}

Error sm_registry_add(SM_Registry *registry, const char *name, const Service_Interface *interface) {
  if (!registry || !name || !interface || strlen(name) > SM_SERVICE_NAME_BYTES) {
    return ERR(RESULT_INVALID_ARGUMENT, "sm_registry_add: bad argument");
  }
  const uint64_t wire = name_to_wire(name);
  if (!sm_service_name_is_valid(wire)) return ERR(RESULT_INVALID_ARGUMENT, "sm_registry_add: bad name");
  if (sm_registry_find(registry, wire)) return ERR(RESULT_INVALID_ARGUMENT, "sm_registry_add: duplicate");
  if (registry->count >= SM_REGISTRY_CAPACITY) return ERR(RESULT_OUT_OF_MEMORY, "sm_registry_add: full");
  registry->entries[registry->count].name = wire;
  registry->entries[registry->count].interface = interface;
  registry->count++;
  return OK;
}

const Service_Interface *sm_registry_find(const SM_Registry *registry, uint64_t name) {
  if (!registry) return NULL;
  for (uint32_t i = 0; i < registry->count; i++) {
    if (registry->entries[i].name == name) return registry->entries[i].interface;
  }
  return NULL;
}
