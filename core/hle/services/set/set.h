/**
 * set / set:sys - system settings (§12). The console Voland presents:
 * firmware 17.0.0 (what titles' hosversion checks see), US region, US
 * English, the dark color set, nickname "Voland".
 *
 * set: 0 GetLanguageCode, 1/5 GetAvailableLanguageCodes(2), 2
 * MakeLanguageCode, 3/6 GetAvailableLanguageCodeCount(2), 4 GetRegionCode,
 * 8 GetQuestFlag, 11 GetDeviceNickName.
 * set:sys: 3/4 GetFirmwareVersion(2) (0x100-byte SetSysFirmwareVersion),
 * 5 GetFirmwareVersionDigest, 7 GetLockScreenFlag, 23 GetColorSetId,
 * 37/38 GetSettingsItemValue(Size) (a small table; others NotFound),
 * 62 GetDebugModeFlag, 68 GetSerialNumber, 77 GetDeviceNickName.
 */
#ifndef SWITCH_HLE_SERVICES_SET_SET_H
#define SWITCH_HLE_SERVICES_SET_SET_H

#include <stdint.h>

#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define SET_FIRMWARE_MAJOR 17u
#define SET_FIRMWARE_MINOR 0u
#define SET_FIRMWARE_MICRO 0u
#define SET_LANGUAGE_COUNT 18u     /* SetLanguage_Total */
#define SET_LANGUAGE_COUNT_V1 15u  /* GetAvailableLanguageCodeCount (pre-4.0 list) */
#define SET_LANGUAGE_EN_US 1u
#define SET_REGION_USA 1u
#define SET_COLOR_SET_DARK 1u
#define SET_MODULE 105u
#define SET_RESULT_ITEM_NOT_FOUND ((11u << 9) | SET_MODULE)
#define SET_RESULT_INVALID_LANGUAGE ((625u << 9) | SET_MODULE)

typedef struct Set_State {
  Service_Interface set;
  Service_Interface set_sys;
} Set_State;

void set_init(Set_State *state);
Error set_register(Set_State *state, SM_Registry *registry);

/* The language code (packed ASCII, e.g. "en-US") for a SetLanguage. */
uint64_t set_language_code(uint32_t language);

#endif /* SWITCH_HLE_SERVICES_SET_SET_H */
