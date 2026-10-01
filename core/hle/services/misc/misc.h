/**
 * Small system services with fixed answers (§12 stub tier): the console
 * Voland presents is always on mains power with a full battery, at room
 * temperature.
 *
 *   psm  0 GetBatteryChargePercentage (100), 1 GetChargerType (EnoughPower),
 *        5 IsBatteryChargingEnabled, 6 IsEnoughPowerSupplied, 7 OpenSession
 *        -> IPsmSession {0 BindStateChangeEvent, 1 Unbind, 2-4 Set*Enabled},
 *        13 GetBatteryAgePercentage (100), 14 GetBatteryChargeInfoEvent.
 *   ts   1 GetTemperature (35 C), 3 GetTemperatureMilliC.
 */
#ifndef SWITCH_HLE_SERVICES_MISC_MISC_H
#define SWITCH_HLE_SERVICES_MISC_MISC_H

#include "hle/kernel/event.h"
#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define PSM_BATTERY_PERCENT 100u
#define PSM_CHARGER_ENOUGH_POWER 1u
#define TS_TEMPERATURE_C 35u

typedef struct Misc_State {
  Service_Interface psm;
  Service_Interface psm_session;
  Service_Interface ts;
  Kernel_Event *psm_event;
} Misc_State;

void misc_init(Misc_State *state);
Error misc_register(Misc_State *state, SM_Registry *registry);

#endif /* SWITCH_HLE_SERVICES_MISC_MISC_H */
