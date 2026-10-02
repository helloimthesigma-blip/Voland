#include "hle/services/misc/misc.h"

#include "hle/services/service_util.h"

#define MILLI 1000u

static Misc_State *state_of(Service_Object *self) { return (Misc_State *)self->interface->service_state; }

#define MISC_VALUE_COMMAND(fn, value)                                                                  \
  static HLE_ServiceResult fn(HLE_Context *c, Service_Object *self, const IPC_Request *req,          \
                              IPC_Response *res) {                                                   \
    (void)c;                                                                                         \
    (void)self;                                                                                      \
    (void)req;                                                                                       \
    (void)ipc_response_push_u32(res, value);                                                         \
    return HLE_RESULT_SUCCESS;                                                                       \
  }

MISC_VALUE_COMMAND(cmd_battery_percent, PSM_BATTERY_PERCENT)
MISC_VALUE_COMMAND(cmd_charger_type, PSM_CHARGER_ENOUGH_POWER)
MISC_VALUE_COMMAND(cmd_temperature, TS_TEMPERATURE_C)
MISC_VALUE_COMMAND(cmd_temperature_milli, TS_TEMPERATURE_C * MILLI)

static HLE_ServiceResult cmd_open_session(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->psm_session, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_state_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)req;
  return service_push_event(c, res, &state_of(self)->psm_event);
}

static uint64_t splitmix64(uint64_t *state) {
  uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

static HLE_ServiceResult cmd_random_bytes(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)res;
  Misc_State *s = state_of(self);
  const IPC_Buffer *buf = service_out_buffer(req, 0);
  if (!buf) return HLE_RESULT_SUCCESS;
  uint8_t chunk[CSRNG_CHUNK_BYTES];
  for (uint64_t at = 0; at < buf->size; at += sizeof(chunk)) {
    for (uint32_t i = 0; i < sizeof(chunk); i += 8) {
      const uint64_t v = splitmix64(&s->random_state);
      memcpy(chunk + i, &v, 8);
    }
    const uint64_t n = buf->size - at < sizeof(chunk) ? buf->size - at : sizeof(chunk);
    if (!error_is_ok(vmm_write_block(c->vmm, buf->gva + at, chunk, n))) return HLE_RESULT_INVALID_POINTER;
  }
  return HLE_RESULT_SUCCESS;
}

/* pdm:qry: counts and statistics are all zero (no play history). */
static HLE_ServiceResult cmd_pdm_nothing(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  for (uint32_t i = 0; i < PDM_ZERO_WORDS; i++) (void)ipc_response_push_u32(res, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_pdm_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  return service_push_event(c, res, &s->pdm_event);
}

static const Service_Command k_pdm_commands[] = {
    {0, cmd_pdm_nothing, "QueryAppletEvent"},
    {1, cmd_pdm_nothing, "QueryPlayStatistics"},
    {2, cmd_pdm_nothing, "QueryPlayStatisticsByUserAccountId"},
    {3, cmd_pdm_nothing, "QueryPlayStatisticsByNetworkServiceAccountId"},
    {4, cmd_pdm_nothing, "QueryPlayStatisticsByApplicationId"},
    {5, cmd_pdm_nothing, "QueryPlayStatisticsByApplicationIdAndUserAccountId"},
    {6, cmd_pdm_nothing, "QueryPlayStatisticsByApplicationIdAndNetworkServiceAccountId"},
    {7, cmd_pdm_nothing, "QueryLastPlayTimeV0"},
    {8, cmd_pdm_nothing, "QueryPlayEvent"},
    {9, cmd_pdm_nothing, "GetAvailablePlayEventRange"},
    {10, cmd_pdm_nothing, "QueryAccountEvent"},
    {11, cmd_pdm_nothing, "QueryAccountPlayEvent"},
    {12, cmd_pdm_nothing, "GetAvailableAccountPlayEventRange"},
    {13, cmd_pdm_nothing, "QueryApplicationPlayStatisticsForSystem"},
    {14, cmd_pdm_nothing, "QueryRecentlyPlayedApplication"},
    {15, cmd_pdm_event, "GetRecentlyPlayedApplicationUpdateEvent"},
    {16, cmd_pdm_nothing, "QueryApplicationPlayStatisticsByUserAccountIdForSystem"},
    {17, cmd_pdm_nothing, "QueryLastPlayTime"},
    {18, cmd_pdm_nothing, "QueryApplicationPlayStatisticsByUid"},
};

/* pm:shell / pm:info. */
static HLE_ServiceResult cmd_pm_not_found(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return PM_RESULT_PROCESS_NOT_FOUND;
}

static HLE_ServiceResult cmd_pm_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                      IPC_Response *res) {
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  return service_push_event(c, res, &s->pm_event);
}

static const Service_Command k_pm_shell_commands[] = {
    {0, cmd_pm_not_found, "LaunchProgram"},
    {1, cmd_pm_not_found, "TerminateProcess"},
    {2, cmd_pm_not_found, "TerminateProgram"},
    {3, cmd_pm_event, "GetProcessEventHandle"},
    {4, service_cmd_out_zero128, "GetProcessEventInfo"},
    {5, service_cmd_ok, "NotifyBootFinished"},
    {6, cmd_pm_not_found, "GetApplicationProcessIdForShell"},
    {7, service_cmd_ok, "BoostSystemMemoryResourceLimit"},
    {8, service_cmd_ok, "BoostApplicationThreadResourceLimit"},
    {9, cmd_pm_event, "GetBootFinishedEventHandle"},
};

static const Service_Command k_pm_info_commands[] = {
    {0, cmd_pm_not_found, "GetProgramId"},
    {65000, cmd_pm_not_found, "AtmosphereGetProcessId"},
};

static const Service_Command k_csrng_commands[] = {
    {0, cmd_random_bytes, "GetRandomBytes"},
};

static const Service_Command k_psm_commands[] = {
    {0, cmd_battery_percent, "GetBatteryChargePercentage"},
    {1, cmd_charger_type, "GetChargerType"},
    {2, service_cmd_ok, "EnableBatteryCharging_stub"},
    {3, service_cmd_ok, "DisableBatteryCharging_stub"},
    {4, service_cmd_out_u8_true, "IsBatteryChargingEnabled"},
    {5, service_cmd_ok, "AcquireControllerPowerSupply_stub"},
    {6, service_cmd_ok, "ReleaseControllerPowerSupply_stub"},
    {7, cmd_open_session, "OpenSession"},
    {13, cmd_battery_percent, "GetBatteryAgePercentage"},
    {14, cmd_state_event, "GetBatteryChargeInfoEvent"},
    {18, service_cmd_out_u8_true, "IsEnoughPowerSupplied"},
};

static const Service_Command k_psm_session_commands[] = {
    {0, cmd_state_event, "BindStateChangeEvent"},
    {1, service_cmd_ok, "UnbindStateChangeEvent"},
    {2, service_cmd_ok, "SetChargerTypeChangeEventEnabled"},
    {3, service_cmd_ok, "SetPowerSupplyChangeEventEnabled"},
    {4, service_cmd_ok, "SetBatteryVoltageStateChangeEventEnabled"},
};

static const Service_Command k_ts_commands[] = {
    {1, cmd_temperature, "GetTemperature"},
    {3, cmd_temperature_milli, "GetTemperatureMilliC"},
};

void misc_init(Misc_State *s) {
  memset(s, 0, sizeof(*s));
  s->psm = SERVICE_INTERFACE("psm", k_psm_commands, 0, s);
  s->psm_session = SERVICE_INTERFACE("IPsmSession", k_psm_session_commands, 0, s);
  s->ts = SERVICE_INTERFACE("ts", k_ts_commands, 0, s);
  s->csrng = SERVICE_INTERFACE("csrng", k_csrng_commands, 0, s);
  s->random_state = CSRNG_SEED;
  s->pdm_query = SERVICE_INTERFACE("pdm:qry", k_pdm_commands, 0, s);
  s->pm_shell = SERVICE_INTERFACE("pm:shell", k_pm_shell_commands, 0, s);
  s->pm_info = SERVICE_INTERFACE("pm:info", k_pm_info_commands, 0, s);
}

Error misc_register(Misc_State *s, SM_Registry *registry) {
  Error err = sm_registry_add(registry, "psm", &s->psm);
  if (error_is_ok(err)) err = sm_registry_add(registry, "ts", &s->ts);
  if (error_is_ok(err)) err = sm_registry_add(registry, "csrng", &s->csrng);
  if (error_is_ok(err)) err = sm_registry_add(registry, "pdm:qry", &s->pdm_query);
  if (error_is_ok(err)) err = sm_registry_add(registry, "pm:shell", &s->pm_shell);
  if (error_is_ok(err)) err = sm_registry_add(registry, "pm:info", &s->pm_info);
  return err;
}
