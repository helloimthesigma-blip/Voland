#include "hle/services/apm/apm.h"

#include "hle/services/service_util.h"

static Apm_State *state_of(Service_Object *self) { return (Apm_State *)self->interface->service_state; }

static HLE_ServiceResult cmd_open_session(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->session, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_performance_mode(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                  IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 0); /* Normal */
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_set_configuration(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)c;
  (void)res;
  uint32_t mode = 0, config = 0;
  if (!error_is_ok(ipc_request_read_u32(req, 0, &mode)) || !error_is_ok(ipc_request_read_u32(req, 4, &config))) {
    return IPC_RESULT_SF_INVALID_IN_HEADER;
  }
  if (mode >= APM_PERFORMANCE_MODES) return HLE_RESULT_INVALID_ENUM_VALUE;
  state_of(self)->configuration[mode] = config;
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_configuration(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)c;
  uint32_t mode = 0;
  if (!error_is_ok(ipc_request_read_u32(req, 0, &mode))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  if (mode >= APM_PERFORMANCE_MODES) return HLE_RESULT_INVALID_ENUM_VALUE;
  (void)ipc_response_push_u32(res, state_of(self)->configuration[mode]);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_apm_commands[] = {
    {0, cmd_open_session, "OpenSession"},
    {1, cmd_get_performance_mode, "GetPerformanceMode"},
    {6, service_cmd_out_u8_false, "IsCpuOverclockEnabled_stub"},
};

static const Service_Command k_session_commands[] = {
    {0, cmd_set_configuration, "SetPerformanceConfiguration"},
    {1, cmd_get_configuration, "GetPerformanceConfiguration"},
    {2, service_cmd_ok, "SetCpuOverclockEnabled_stub"},
};

void apm_init(Apm_State *state) {
  memset(state, 0, sizeof(*state));
  for (uint32_t i = 0; i < APM_PERFORMANCE_MODES; i++) state->configuration[i] = APM_DEFAULT_CONFIGURATION;
  state->interface = SERVICE_INTERFACE("apm", k_apm_commands, 0, state);
  state->session = SERVICE_INTERFACE("apm:ISession", k_session_commands, 0, state);
}

Error apm_register(Apm_State *state, SM_Registry *registry) {
  static const char *const k_names[] = {"apm", "apm:am", "apm:sys"};
  for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
    const Error err = sm_registry_add(registry, k_names[i], &state->interface);
    if (!error_is_ok(err)) return err;
  }
  return OK;
}
