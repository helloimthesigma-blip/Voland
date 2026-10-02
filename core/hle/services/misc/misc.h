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
 *   csrng 0 GetRandomBytes: a splitmix64 stream, seeded identically every
 *        boot so headless runs are reproducible (titles use it for seeds
 *        and nonces, not for anything Voland must keep secret).
 *   pm:shell / pm:info - the process manager as a shell sees it: no other
 *        application process exists (GetApplicationProcessIdForShell fails
 *        ProcessNotFound), boot is finished, events never fire.
 *   pdm:qry play-history queries: Voland records no play history, so every
 *        query reports nothing (zero counts, zeroed statistics) and the
 *        update event never fires.
 */
#ifndef SWITCH_HLE_SERVICES_MISC_MISC_H
#define SWITCH_HLE_SERVICES_MISC_MISC_H

#include "hle/kernel/event.h"
#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define PSM_BATTERY_PERCENT 100u
#define PSM_CHARGER_ENOUGH_POWER 1u
#define TS_TEMPERATURE_C 35u
#define CSRNG_SEED 0x566F6C616E645247ull
#define CSRNG_CHUNK_BYTES 256u
#define PDM_ZERO_WORDS 12u
#define PM_MODULE 15u
#define PM_RESULT_PROCESS_NOT_FOUND ((1u << 9) | PM_MODULE) /* the largest fixed pdm:qry reply (PlayStatistics, 0x28 bytes) */

typedef struct Misc_State {
  Service_Interface psm;
  Service_Interface psm_session;
  Service_Interface ts;
  Kernel_Event *psm_event;
  Service_Interface csrng;
  uint64_t random_state;
  Service_Interface pdm_query;
  Kernel_Event *pdm_event;
  Service_Interface pm_shell;
  Service_Interface pm_info;
  Kernel_Event *pm_event;
} Misc_State;

void misc_init(Misc_State *state);
Error misc_register(Misc_State *state, SM_Registry *registry);

#endif /* SWITCH_HLE_SERVICES_MISC_MISC_H */
