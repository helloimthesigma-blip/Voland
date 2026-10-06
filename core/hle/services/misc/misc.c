#include "hle/services/misc/misc.h"

#include "common/log.h"

#include <stdio.h>

#include "hle/kernel/svc_memory.h"
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

/* usb:ds (§12 stub tier): never attached. */
#define USB_STATE_DETACHED 0u

static HLE_ServiceResult cmd_usb_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  return service_push_event(c, res, &s->usb_event);
}

static HLE_ServiceResult cmd_usb_state(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, USB_STATE_DETACHED);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_usb_interface(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)c;
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  (void)ipc_response_push_object(res, &s->usb_interface, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_usb_endpoint(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)c;
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  (void)ipc_response_push_object(res, &s->usb_endpoint, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_usb_service(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  (void)ipc_response_push_object(res, &s->usb_service, 0);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_usb_ds_commands[] = {
    {0, cmd_usb_service, "OpenDsService"},
};

static const Service_Command k_usb_service_commands[] = {
    {0, service_cmd_ok, "BindDevice"},
    {1, cmd_usb_interface, "RegisterInterface"},
    {2, cmd_usb_event, "GetStateChangeEvent"},
    {3, cmd_usb_state, "GetState"},
    {4, service_cmd_ok, "ClearDeviceData"},
    {5, service_cmd_out_u8_false, "AddUsbStringDescriptor"},
    {6, service_cmd_ok, "DeleteUsbStringDescriptor"},
    {7, service_cmd_ok, "SetUsbDeviceDescriptor"},
    {8, service_cmd_ok, "SetBinaryObjectStore"},
    {9, service_cmd_ok, "Enable"},
    {10, service_cmd_ok, "Disable"},
    {11, service_cmd_out_u8_false, "GetSpeed"},
};

static const Service_Command k_usb_interface_commands[] = {
    {0, cmd_usb_endpoint, "RegisterEndpoint"},
    {1, cmd_usb_event, "GetSetupEvent"},
    {2, service_cmd_out_zero128, "GetSetupPacket"},
    {3, service_cmd_out_u8_false, "CtrlInPostBufferAsync"},
    {4, service_cmd_out_u8_false, "CtrlOutPostBufferAsync"},
    {5, cmd_usb_event, "GetCtrlInCompletionEvent"},
    {6, service_cmd_out_zero128, "GetCtrlInReportData"},
    {7, cmd_usb_event, "GetCtrlOutCompletionEvent"},
    {8, service_cmd_out_zero128, "GetCtrlOutReportData"},
    {9, service_cmd_ok, "StallCtrl"},
    {10, service_cmd_out_u8_false, "AppendConfigurationData"},
};

static const Service_Command k_usb_endpoint_commands[] = {
    {0, service_cmd_out_u8_false, "PostBufferAsync"},
    {1, service_cmd_ok, "Cancel"},
    {2, cmd_usb_event, "GetCompletionEvent"},
    {3, service_cmd_out_zero128, "GetReportData"},
    {4, service_cmd_ok, "Stall"},
    {5, service_cmd_ok, "SetZlt"},
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

/* usb:hs (§12 stub tier): no device is ever attached. */
static HLE_ServiceResult cmd_usb_hs_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                          IPC_Response *res) {
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  return service_push_event(c, res, &s->usb_hs_event);
}

static HLE_ServiceResult cmd_usb_hs_none(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 0); /* total entries */
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_usb_hs_acquire(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)res;
  return USB_RESULT_NOT_FOUND;
}

static const Service_Command k_usb_hs_commands[] = {
    {0, service_cmd_ok, "BindClientProcess"},
    {1, cmd_usb_hs_none, "QueryAllInterfaces"},
    {2, cmd_usb_hs_none, "QueryAvailableInterfaces"},
    {3, cmd_usb_hs_none, "QueryAcquiredInterfaces"},
    {4, cmd_usb_hs_event, "CreateInterfaceAvailableEvent"},
    {5, service_cmd_ok, "DestroyInterfaceAvailableEvent"},
    {6, cmd_usb_hs_event, "GetInterfaceStateChangeEvent"},
    {7, cmd_usb_hs_acquire, "AcquireUsbIf"},
};

/* lm: a title's log sink. */
static HLE_ServiceResult cmd_open_logger(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  (void)ipc_response_push_object(res, &s->logger, 0);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_lm_commands[] = {
    {0, cmd_open_logger, "OpenLogger"},
};

/* lm ILogger::Log: a packet header (u64 process id, u64 thread id, u8
 * flags, u8 reserved, u8 severity, u8 verbosity, u32 payload size) then
 * chunks (u8 key, ULEB128 size, bytes). The title's own text, file, line
 * and function go to Voland's log - when a game gives up, this is usually
 * where it says why. */
#define LM_HEADER_BYTES 0x18u
#define LM_SEVERITY_OFFSET 0x12u
#define LM_PACKET_MAX 0x1000u
#define LM_KEY_TEXT 2u
#define LM_KEY_LINE 3u
#define LM_KEY_FILE 4u
#define LM_KEY_FUNCTION 5u
#define LM_SEVERITY_WARNING 2u
#define LM_FIELD_MAX 160u

static void lm_copy_text(char *out, const uint8_t *bytes, uint64_t size) {
  const uint64_t n = size < LM_FIELD_MAX - 1u ? size : LM_FIELD_MAX - 1u;
  for (uint64_t i = 0; i < n; i++) out[i] = (bytes[i] >= 0x20 && bytes[i] < 0x7f) ? (char)bytes[i] : ' ';
  out[n] = '\0';
}

static HLE_ServiceResult cmd_logger_log(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)self;
  (void)res;
  uint8_t packet[LM_PACKET_MAX];
  const uint64_t got = service_read_in(c, req, 0, packet, sizeof(packet));
  if (got <= LM_HEADER_BYTES) return HLE_RESULT_SUCCESS;
  char text[LM_FIELD_MAX] = "", file[LM_FIELD_MAX] = "", function[LM_FIELD_MAX] = "";
  uint32_t line = 0;
  for (uint64_t at = LM_HEADER_BYTES; at < got;) {
    const uint8_t key = packet[at++];
    uint64_t size = 0;
    for (uint32_t shift = 0; at < got && shift < 35u; shift += 7u) {
      const uint8_t b = packet[at++];
      size |= (uint64_t)(b & 0x7Fu) << shift;
      if (!(b & 0x80u)) break;
    }
    if (size > got - at) break;
    if (key == LM_KEY_TEXT) lm_copy_text(text, packet + at, size);
    else if (key == LM_KEY_FILE) lm_copy_text(file, packet + at, size);
    else if (key == LM_KEY_FUNCTION) lm_copy_text(function, packet + at, size);
    else if (key == LM_KEY_LINE && size >= 4u) memcpy(&line, packet + at, 4);
    at += size;
  }
  if (!text[0]) return HLE_RESULT_SUCCESS;
  char where[LM_FIELD_MAX * 2u + 16u] = "";
  if (file[0] || function[0]) snprintf(where, sizeof(where), " (%s:%u %s)", file, line, function);
  if (packet[LM_SEVERITY_OFFSET] >= LM_SEVERITY_WARNING) log_warn("[guest log] %s%s", text, where);
  else log_info("[guest log] %s%s", text, where);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_logger_commands[] = {
    {0, cmd_logger_log, "Log"},
    {1, service_cmd_ok, "SetDestination"},
};

static HLE_ServiceResult cmd_create_registrar(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  (void)ipc_response_push_object(res, &s->ectx_registrar, 0);
  return HLE_RESULT_SUCCESS;
}

/* ldr:ro 0 MapManualLoadModuleMemory {u64 pid placeholder, u64 nro
 * address, u64 nro size, u64 bss address, u64 bss size} -> u64 address
 * (the module mapped by svc_memory.c; nn::ro links it in the guest). */
#define RO_ARG_NRO 8u
#define RO_ARG_NRO_SIZE 16u
#define RO_ARG_BSS 24u
#define RO_ARG_BSS_SIZE 32u
static HLE_ServiceResult cmd_ro_load_module(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)self;
  uint64_t nro = 0, nro_size = 0, bss = 0, bss_size = 0, base = 0;
  if (!error_is_ok(ipc_request_read_u64(req, RO_ARG_NRO, &nro)) ||
      !error_is_ok(ipc_request_read_u64(req, RO_ARG_NRO_SIZE, &nro_size)) ||
      !error_is_ok(ipc_request_read_u64(req, RO_ARG_BSS, &bss)) ||
      !error_is_ok(ipc_request_read_u64(req, RO_ARG_BSS_SIZE, &bss_size)))
    return IPC_RESULT_SF_INVALID_IN_HEADER;
  const uint32_t rc = hle_ro_map_module(c, nro, nro_size, bss, bss_size, &base);
  if (rc) {
    log_warn("[ro] LoadModule(nro 0x%llx+0x%llx, bss 0x%llx+0x%llx) failed: 0x%x", (unsigned long long)nro,
             (unsigned long long)nro_size, (unsigned long long)bss, (unsigned long long)bss_size, rc);
    return rc;
  }
  (void)ipc_response_push_u64(res, base);
  return HLE_RESULT_SUCCESS;
}

/* 1 UnmapManualLoadModuleMemory {u64 pid placeholder, u64 module address}. */
#define RO_ARG_MODULE 8u
static HLE_ServiceResult cmd_ro_unload_module(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)self;
  (void)res;
  uint64_t base = 0;
  if (!error_is_ok(ipc_request_read_u64(req, RO_ARG_MODULE, &base))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  return hle_ro_unmap_module(c, base);
}

static const Service_Command k_ldr_ro_commands[] = {
    {0, cmd_ro_load_module, "MapManualLoadModuleMemory"},
    {1, cmd_ro_unload_module, "UnmapManualLoadModuleMemory"},
    {2, service_cmd_ok, "RegisterModuleInfo"},
    {3, service_cmd_ok, "UnregisterModuleInfo"},
    {4, service_cmd_ok, "RegisterProcessHandle"},
    {10, service_cmd_ok, "RegisterProcessModuleInfo"},
};

static const Service_Command k_ectx_commands[] = {
    {0, cmd_create_registrar, "CreateContextRegistrar"},
};

static const Service_Command k_ectx_registrar_commands[] = {
    {0, service_cmd_ok, "Complete"},
};

/* mm:u: clock requests are remembered, nothing is clocked. */
static HLE_ServiceResult cmd_mm_initialize(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)c;
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  (void)ipc_response_push_u32(res, ++s->mm_next_id);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_mm_set(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  (void)res;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  uint32_t id = 0, minimum = 0;
  if (error_is_ok(ipc_request_read_u32(req, 0, &id)) && error_is_ok(ipc_request_read_u32(req, 4, &minimum)))
    s->mm_rate[id % MM_REQUESTS] = minimum;
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_mm_get(HLE_Context *c, Service_Object *self, const IPC_Request *req, IPC_Response *res) {
  (void)c;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  uint32_t id = 0;
  (void)ipc_request_read_u32(req, 0, &id);
  (void)ipc_response_push_u32(res, s->mm_rate[id % MM_REQUESTS]);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_mm_commands[] = {
    {0, service_cmd_ok, "InitializeOld"}, {1, service_cmd_ok, "FinalizeOld"}, {2, cmd_mm_set, "SetAndWaitOld"},
    {3, cmd_mm_get, "GetOld"},           {4, cmd_mm_initialize, "Initialize"}, {5, service_cmd_ok, "Finalize"},
    {6, cmd_mm_set, "SetAndWait"},        {7, cmd_mm_get, "Get"},
};

/* Parental controls: nothing is restricted. */
static HLE_ServiceResult cmd_pctl_create(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                         IPC_Response *res) {
  (void)c;
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  (void)ipc_response_push_object(res, &s->pctl_service, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_pctl_event(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                        IPC_Response *res) {
  (void)req;
  Misc_State *s = (Misc_State *)self->interface->service_state;
  return service_push_event(c, res, &s->pctl_event);
}

static HLE_ServiceResult cmd_pctl_zero_u32(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 0);
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_pctl_commands[] = {
    {0, cmd_pctl_create, "CreateService"},
    {1, cmd_pctl_create, "CreateServiceWithoutInitialize"},
};

static const Service_Command k_pctl_service_commands[] = {
    {1, service_cmd_ok, "Initialize"},
    {1001, service_cmd_ok, "CheckFreeCommunicationPermission"},
    {1002, service_cmd_ok, "ConfirmLaunchApplicationPermission"},
    {1003, service_cmd_ok, "ConfirmResumeApplicationPermission"},
    {1004, service_cmd_ok, "ConfirmSnsPostPermission"},
    {1006, service_cmd_out_u8_false, "IsRestrictionTemporaryUnlocked"},
    {1007, service_cmd_ok, "RevertRestrictionTemporaryUnlocked"},
    {1008, service_cmd_ok, "EnterRestrictedSystemSettings"},
    {1009, service_cmd_ok, "LeaveRestrictedSystemSettings"},
    {1010, service_cmd_out_u8_false, "IsRestrictedSystemSettingsEntered"},
    {1011, service_cmd_ok, "RevertRestrictedSystemSettingsEntered"},
    {1012, cmd_pctl_zero_u32, "GetRestrictedFeatures"},
    {1013, service_cmd_ok, "ConfirmStereoVisionPermission"},
    {1017, service_cmd_ok, "EndFreeCommunication"},
    {1018, service_cmd_out_u8_true, "IsFreeCommunicationAvailable"},
    {1031, service_cmd_out_u8_false, "IsRestrictionEnabled"},
    {1032, cmd_pctl_zero_u32, "GetSafetyLevel"},
    {1035, cmd_pctl_zero_u32, "GetCurrentSettings"},
    {1037, cmd_pctl_zero_u32, "GetFreeCommunicationApplicationListCount"},
    {1039, cmd_pctl_zero_u32, "GetFreeCommunicationApplicationListCount2"},
    {1061, service_cmd_ok, "ConfirmStereoVisionRestrictionConfigurable"},
    {1062, service_cmd_out_u8_false, "GetStereoVisionRestriction"},
    {1063, service_cmd_ok, "SetStereoVisionRestriction"},
    {1064, service_cmd_ok, "ResetConfirmedStereoVisionPermission"},
    {1065, service_cmd_out_u8_true, "IsStereoVisionPermitted"},
    {1403, service_cmd_out_u8_false, "IsPairingActive"},
    {1451, service_cmd_ok, "StartPlayTimer"},
    {1452, service_cmd_ok, "StopPlayTimer"},
    {1453, service_cmd_out_u8_false, "IsPlayTimerEnabled"},
    {1454, service_cmd_out_u64_zero, "GetPlayTimerRemainingTime"},
    {1455, service_cmd_out_u8_false, "IsRestrictedByPlayTimer"},
    {1456, service_cmd_out_zero128, "GetPlayTimerSettings"},
    {1457, cmd_pctl_event, "GetPlayTimerEventToRequestSuspension"},
    {1458, service_cmd_out_u8_false, "IsPlayTimerAlarmDisabled"},
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
  s->usb_ds = SERVICE_INTERFACE("usb:ds", k_usb_ds_commands, 0, s);
  s->usb_service = SERVICE_INTERFACE("IDsService", k_usb_service_commands, 0, s);
  s->usb_interface = SERVICE_INTERFACE("IDsInterface", k_usb_interface_commands, 0, s);
  s->usb_endpoint = SERVICE_INTERFACE("IDsEndpoint", k_usb_endpoint_commands, 0, s);
  s->usb_hs = SERVICE_INTERFACE("usb:hs", k_usb_hs_commands, 0, s);
  s->lm = SERVICE_INTERFACE("lm", k_lm_commands, 0, s);
  s->ectx = SERVICE_INTERFACE("ectx:aw", k_ectx_commands, 0, s);
  s->ldr_ro = SERVICE_INTERFACE("ldr:ro", k_ldr_ro_commands, 0, s);
  static const char *const k_pctl_ports[4] = {"pctl", "pctl:a", "pctl:r", "pctl:s"};
  for (uint32_t i = 0; i < 4u; i++) s->pctl[i] = SERVICE_INTERFACE(k_pctl_ports[i], k_pctl_commands, 0, s);
  s->pctl_service = SERVICE_INTERFACE("IParentalControlService", k_pctl_service_commands, 0, s);
  s->ectx_registrar = SERVICE_INTERFACE("IContextRegistrar", k_ectx_registrar_commands, 0, s);
  s->logger = SERVICE_INTERFACE("ILogger", k_logger_commands, 0, s);
  s->mm = SERVICE_INTERFACE("mm:u", k_mm_commands, 0, s);
}

Error misc_register(Misc_State *s, SM_Registry *registry) {
  Error err = sm_registry_add(registry, "psm", &s->psm);
  if (error_is_ok(err)) err = sm_registry_add(registry, "ts", &s->ts);
  if (error_is_ok(err)) err = sm_registry_add(registry, "csrng", &s->csrng);
  if (error_is_ok(err)) err = sm_registry_add(registry, "pdm:qry", &s->pdm_query);
  if (error_is_ok(err)) err = sm_registry_add(registry, "pm:shell", &s->pm_shell);
  if (error_is_ok(err)) err = sm_registry_add(registry, "pm:info", &s->pm_info);
  if (error_is_ok(err)) err = sm_registry_add(registry, "usb:ds", &s->usb_ds);
  if (error_is_ok(err)) err = sm_registry_add(registry, "usb:hs", &s->usb_hs);
  if (error_is_ok(err)) err = sm_registry_add(registry, "lm", &s->lm);
  if (error_is_ok(err)) err = sm_registry_add(registry, "ectx:aw", &s->ectx);
  if (error_is_ok(err)) err = sm_registry_add(registry, "ldr:ro", &s->ldr_ro);
  if (error_is_ok(err)) err = sm_registry_add(registry, "mm:u", &s->mm);
  for (uint32_t i = 0; i < 4u && error_is_ok(err); i++) err = sm_registry_add(registry, s->pctl[i].name, &s->pctl[i]);
  return err;
}
