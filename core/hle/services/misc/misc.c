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
}

Error misc_register(Misc_State *s, SM_Registry *registry) {
  Error err = sm_registry_add(registry, "psm", &s->psm);
  if (error_is_ok(err)) err = sm_registry_add(registry, "ts", &s->ts);
  if (error_is_ok(err)) err = sm_registry_add(registry, "csrng", &s->csrng);
  return err;
}
