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
 *   usb:ds USB device mode (the 11.0.0+ layout, matching the firmware
 *        version set: reports): 0 OpenDsService -> IDsService; descriptors,
 *        interfaces and endpoints are accepted, the state is always
 *        Detached (no cable), events never fire, transfers never complete.
 *   usb:hs USB host mode (the 2.0.0+ layout): 0 BindClientProcess, 1-3
 *        Query{All,Available,Acquired}Interfaces (always none), 4/5
 *        Create/DestroyInterfaceAvailableEvent, 6 GetInterfaceStateChange-
 *        Event; nothing is ever plugged in, so the events never fire and
 *        7 AcquireUsbIf fails.
 *   ectx:aw error-context recording: 0 CreateContextRegistrar ->
 *        IContextRegistrar {0 Complete} - contexts accepted and dropped.
 *   pctl, pctl:a, pctl:r, pctl:s parental controls, never restricted:
 *        0/1 CreateService(WithoutInitialize) -> IParentalControlService
 *        (permission checks pass, restriction off, safety level 0, the
 *        play timer off and its suspension event never fires).
 *   ldr:ro runtime module loading (nn::ro): 4 RegisterProcessHandle, 2/3
 *        (Un)RegisterModuleInfo (NRR; not verified) and 10 Register-
 *        ProcessModuleInfo are accepted; 0 MapManualLoadModuleMemory maps
 *        the NRO and its .bss buffer at a free 2MB-aligned address past the
 *        TLS/IO region (.text RX, .rodata R, .data/.bss RW, the buffers
 *        inaccessible meanwhile; hle_ro_map_module) - the guest's nn::ro
 *        relocates and links it; 1 UnmapManualLoadModuleMemory undoes it.
 *   lm     the system log: 0 OpenLogger -> ILogger {0 Log (packets
 *        accepted and dropped - titles' own logging, not Voland's), 1 Set-
 *        Destination}.
 *   mm:u   the multimedia clock manager (nn::mm, used by the video
 *        decoders): 4 Initialize -> a request id, 5 Finalize, 6 SetAndWait
 *        {id, min, max}, 7 Get -> the last minimum set; 0-3 are the
 *        id-less forms (the module stands in for the id). Nothing is
 *        clocked; requests are accepted and remembered.
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
#define MM_REQUESTS 16u
#define PM_MODULE 15u
#define USB_MODULE 140u
#define USB_RESULT_NOT_FOUND ((2u << 9) | USB_MODULE) /* no such interface */
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
  Service_Interface usb_ds;
  Service_Interface usb_service;
  Service_Interface usb_interface;
  Service_Interface usb_endpoint;
  Kernel_Event *usb_event;
  Service_Interface usb_hs;
  Service_Interface lm;
  Service_Interface ectx;
  Service_Interface ldr_ro;
  Service_Interface pctl[4];
  Service_Interface pctl_service;
  Kernel_Event *pctl_event;
  Service_Interface ectx_registrar;
  Service_Interface logger;
  Kernel_Event *usb_hs_event;
  Service_Interface mm;
  uint32_t mm_next_id;
  uint32_t mm_rate[MM_REQUESTS]; /* by request id (or module) % MM_REQUESTS */
} Misc_State;

void misc_init(Misc_State *state);
Error misc_register(Misc_State *state, SM_Registry *registry);

#endif /* SWITCH_HLE_SERVICES_MISC_MISC_H */
