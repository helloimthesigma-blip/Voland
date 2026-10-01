/**
 * apm - performance management (§12). Registered as "apm", "apm:am" and
 * "apm:sys". OpenSession (0) -> ISession {SetPerformanceConfiguration 0,
 * GetPerformanceConfiguration 1}; GetPerformanceMode (1). The console is
 * always in Normal (handheld) performance mode; configurations are
 * remembered per mode and have no effect on emulation speed.
 */
#ifndef SWITCH_HLE_SERVICES_APM_APM_H
#define SWITCH_HLE_SERVICES_APM_APM_H

#include <stdint.h>

#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define APM_PERFORMANCE_MODES 2u
#define APM_DEFAULT_CONFIGURATION 0x00010000u /* CPU 1020MHz, GPU 384MHz, EMC 1600MHz */

typedef struct Apm_State {
  Service_Interface interface;
  Service_Interface session;
  uint32_t configuration[APM_PERFORMANCE_MODES];
} Apm_State;

void apm_init(Apm_State *state);
Error apm_register(Apm_State *state, SM_Registry *registry);

#endif /* SWITCH_HLE_SERVICES_APM_APM_H */
