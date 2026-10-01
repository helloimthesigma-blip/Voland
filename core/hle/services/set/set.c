#include "hle/services/set/set.h"

#include "hle/services/service_util.h"

#define SET_FIRMWARE_VERSION_BYTES 0x100u
#define SET_FW_PLATFORM 0x08u
#define SET_FW_HASH 0x28u
#define SET_FW_DISPLAY_VERSION 0x68u
#define SET_FW_DISPLAY_TITLE 0x80u
#define SET_FW_DIGEST_BYTES 0x40u
#define SET_NICKNAME_BYTES 0x80u
#define SET_SERIAL_BYTES 0x18u
#define SET_ITEM_NAME_BYTES 0x48u

static const char *const k_language_codes[SET_LANGUAGE_COUNT] = {
    "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419",
    "zh-Hans", "zh-Hant", "pt-BR"};

uint64_t set_language_code(uint32_t language) {
  uint64_t code = 0;
  if (language < SET_LANGUAGE_COUNT) memcpy(&code, k_language_codes[language], strlen(k_language_codes[language]));
  return code;
}

static HLE_ServiceResult cmd_get_language_code(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u64(res, set_language_code(SET_LANGUAGE_EN_US));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult write_language_codes(HLE_Context *c, const IPC_Request *req, IPC_Response *res,
                                              uint32_t total) {
  const IPC_Buffer *buf = service_out_buffer(req, 0);
  uint32_t n = 0;
  if (buf) {
    const uint64_t fit = buf->size / sizeof(uint64_t);
    n = fit < total ? (uint32_t)fit : total;
    for (uint32_t i = 0; i < n; i++) {
      if (!error_is_ok(vmm_write64(c->vmm, buf->gva + i * sizeof(uint64_t), set_language_code(i)))) {
        return HLE_RESULT_INVALID_POINTER;
      }
    }
  }
  (void)ipc_response_push_u32(res, n);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_available_language_codes(HLE_Context *c, Service_Object *self,
                                                          const IPC_Request *req, IPC_Response *res) {
  (void)self;
  return write_language_codes(c, req, res, SET_LANGUAGE_COUNT_V1);
}

static HLE_ServiceResult cmd_get_available_language_codes2(HLE_Context *c, Service_Object *self,
                                                           const IPC_Request *req, IPC_Response *res) {
  (void)self;
  return write_language_codes(c, req, res, SET_LANGUAGE_COUNT);
}

static HLE_ServiceResult cmd_make_language_code(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                IPC_Response *res) {
  (void)c;
  (void)self;
  uint32_t language = 0;
  if (!error_is_ok(ipc_request_read_u32(req, 0, &language))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  if (language >= SET_LANGUAGE_COUNT) return SET_RESULT_INVALID_LANGUAGE;
  (void)ipc_response_push_u64(res, set_language_code(language));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_language_count(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, SET_LANGUAGE_COUNT_V1);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_language_count2(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, SET_LANGUAGE_COUNT);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_region_code(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, SET_REGION_USA);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_device_nickname(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                 IPC_Response *res) {
  (void)self;
  (void)res;
  char name[SET_NICKNAME_BYTES];
  memset(name, 0, sizeof(name));
  memcpy(name, "Voland", sizeof("Voland"));
  (void)service_write_out(c, req, 0, name, sizeof(name));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_firmware_version(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                  IPC_Response *res) {
  (void)self;
  (void)res;
  uint8_t fw[SET_FIRMWARE_VERSION_BYTES];
  memset(fw, 0, sizeof(fw));
  fw[0] = SET_FIRMWARE_MAJOR;
  fw[1] = SET_FIRMWARE_MINOR;
  fw[2] = SET_FIRMWARE_MICRO;
  memcpy(fw + SET_FW_PLATFORM, "NX", sizeof("NX"));
  memcpy(fw + SET_FW_HASH, "voland", sizeof("voland"));
  memcpy(fw + SET_FW_DISPLAY_VERSION, "17.0.0", sizeof("17.0.0"));
  memcpy(fw + SET_FW_DISPLAY_TITLE, "NintendoSDK Firmware for NX 17.0.0", sizeof("NintendoSDK Firmware for NX 17.0.0"));
  if (!service_write_out(c, req, 0, fw, sizeof(fw))) return HLE_RESULT_INVALID_POINTER;
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_firmware_digest(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                 IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  uint8_t digest[SET_FW_DIGEST_BYTES];
  memset(digest, 0, sizeof(digest));
  (void)ipc_response_push_bytes(res, digest, sizeof(digest));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_color_set_id(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, SET_COLOR_SET_DARK);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_serial_number(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  char serial[SET_SERIAL_BYTES];
  memset(serial, 0, sizeof(serial));
  memcpy(serial, "XAW00000000000", sizeof("XAW00000000000"));
  (void)ipc_response_push_bytes(res, serial, sizeof(serial));
  return HLE_RESULT_SUCCESS;
}

/* Settings items: (class, name) in two X buffers. A small table of the
 * items titles ask for at boot; everything else is NotFound. */
typedef struct Set_Item {
  const char *klass;
  const char *name;
  uint32_t value;
  uint32_t size;
} Set_Item;

static const Set_Item k_items[] = {
    {"time", "standard_steady_clock_test_offset_minutes", 0, 4},
    {"time", "standard_steady_clock_rtc_update_interval_minutes", 5, 4},
    {"time", "standard_network_clock_sufficient_accuracy_minutes", 43200, 4},
    {"time", "standard_user_clock_initial_year", 2026, 4},
    {"settings_debug", "is_debug_mode_enabled", 0, 1},
    {"hbloader", "applet_heap_size", 0, 8},
    {"hbloader", "applet_heap_reservation_size", 0x8600000, 8},
};

static const Set_Item *find_item(HLE_Context *c, const IPC_Request *req) {
  char klass[SET_ITEM_NAME_BYTES + 1], name[SET_ITEM_NAME_BYTES + 1];
  memset(klass, 0, sizeof(klass));
  memset(name, 0, sizeof(name));
  (void)service_read_in(c, req, 0, klass, SET_ITEM_NAME_BYTES);
  (void)service_read_in(c, req, 1, name, SET_ITEM_NAME_BYTES);
  for (size_t i = 0; i < SERVICE_COMMAND_COUNT(k_items); i++) {
    if (strcmp(klass, k_items[i].klass) == 0 && strcmp(name, k_items[i].name) == 0) return &k_items[i];
  }
  return NULL;
}

static HLE_ServiceResult cmd_get_item_size(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)self;
  const Set_Item *item = find_item(c, req);
  if (!item) return SET_RESULT_ITEM_NOT_FOUND;
  (void)ipc_response_push_u64(res, item->size);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_item_value(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                            IPC_Response *res) {
  (void)self;
  const Set_Item *item = find_item(c, req);
  if (!item) return SET_RESULT_ITEM_NOT_FOUND;
  uint64_t value = item->value;
  (void)ipc_response_push_u64(res, service_write_out(c, req, 0, &value, item->size));
  return HLE_RESULT_SUCCESS;
}

static const Service_Command k_set_commands[] = {
    {0, cmd_get_language_code, "GetLanguageCode"},
    {1, cmd_get_available_language_codes, "GetAvailableLanguageCodes"},
    {2, cmd_make_language_code, "MakeLanguageCode"},
    {3, cmd_language_count, "GetAvailableLanguageCodeCount"},
    {4, cmd_get_region_code, "GetRegionCode"},
    {5, cmd_get_available_language_codes2, "GetAvailableLanguageCodes2"},
    {6, cmd_language_count2, "GetAvailableLanguageCodeCount2"},
    {8, service_cmd_out_u8_false, "GetQuestFlag"},
    {11, cmd_get_device_nickname, "GetDeviceNickName"},
};

static const Service_Command k_set_sys_commands[] = {
    {3, cmd_get_firmware_version, "GetFirmwareVersion"},
    {4, cmd_get_firmware_version, "GetFirmwareVersion2"},
    {5, cmd_get_firmware_digest, "GetFirmwareVersionDigest"},
    {7, service_cmd_out_u8_false, "GetLockScreenFlag"},
    {23, cmd_get_color_set_id, "GetColorSetId"},
    {37, cmd_get_item_size, "GetSettingsItemValueSize"},
    {38, cmd_get_item_value, "GetSettingsItemValue"},
    {62, service_cmd_out_u8_false, "GetDebugModeFlag"},
    {68, cmd_get_serial_number, "GetSerialNumber"},
    {77, cmd_get_device_nickname, "GetDeviceNickName"},
};

void set_init(Set_State *state) {
  memset(state, 0, sizeof(*state));
  state->set = SERVICE_INTERFACE("set", k_set_commands, 0x800, state);
  state->set_sys = SERVICE_INTERFACE("set:sys", k_set_sys_commands, 0x800, state);
}

Error set_register(Set_State *state, SM_Registry *registry) {
  Error err = sm_registry_add(registry, "set", &state->set);
  if (error_is_ok(err)) err = sm_registry_add(registry, "set:sys", &state->set_sys);
  return err;
}
