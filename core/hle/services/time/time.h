/**
 * time - clocks and time zones (§12), backed by virtual time (§7).
 * Registered as time:u, time:a, time:s, time:r, time:su.
 *
 * The steady clock counts seconds of virtual time since the process
 * started; every system clock (user, network, local, ephemeral) is that
 * plus `rtc_at_boot`, the Unix time the platform sets with
 * emulator_set_rtc() (default TIME_DEFAULT_RTC, a fixed date so headless
 * runs are deterministic). The 0x1000-byte clock shared memory (libnx
 * reads the clocks straight from it on 6.0.0+) holds three seqlock'd
 * objects - steady base @0x00, user/local context @0x38, network context
 * @0x80 - plus the automatic-correction flag @0xC8; each is a u32 counter
 * then two copies, the reader taking copy (counter & 1).
 *
 * Time zones: one location, "UTC". LoadTimeZoneRule hands out an opaque
 * all-zero rule that every conversion treats as UTC.
 */
#ifndef SWITCH_HLE_SERVICES_TIME_TIME_H
#define SWITCH_HLE_SERVICES_TIME_TIME_H

#include <stdbool.h>
#include <stdint.h>

#include "hle/kernel/ipc.h"
#include "hle/kernel/shared_memory.h"
#include "hle/services/sm/sm.h"

#define TIME_DEFAULT_RTC 1767225600ll /* 2026-01-01T00:00:00Z */
#define TIME_SHARED_MEMORY_BYTES 0x1000u
#define TIME_SHMEM_STEADY 0x00u
#define TIME_SHMEM_LOCAL_CONTEXT 0x38u
#define TIME_SHMEM_NETWORK_CONTEXT 0x80u
#define TIME_SHMEM_AUTOMATIC_CORRECTION 0xC8u
/* Newer SDKs' steady clock reads its continuous adjustment time point
 * here: {s64 clock offset (ns), s64 multiplier, s64 divisor log2, steady
 * clock time point}, a rate of multiplier / 2^divisor_log2. Left zero,
 * its clock source id matched no context and CLOCK_REALTIME failed
 * (Silksong aborted on every save: std::system_error from clock_gettime). */
#define TIME_SHMEM_CONTINUOUS_ADJUSTMENT 0xD0u
#define TIME_MODULE 116u
#define TIME_RESULT_OUT_OF_RANGE ((902u << 9) | TIME_MODULE)

typedef struct Time_State {
  Service_Interface interface;
  Service_Interface system_clock;   /* object state: clock kind */
  Service_Interface steady_clock;
  Service_Interface time_zone;
  Shared_Memory_Pool *pool;
  Kernel_Shared_Memory *shared_memory;
  int64_t rtc_at_boot;              /* Unix seconds at virtual time 0 */
} Time_State;

void time_init(Time_State *state, Shared_Memory_Pool *pool, int64_t rtc_at_boot);
Error time_register(Time_State *state, SM_Registry *registry);

/* Proleptic-Gregorian conversions (UTC), exposed for tests. */
typedef struct Time_Calendar {
  int64_t year;
  uint32_t month, day, hour, minute, second; /* month/day 1-based */
  uint32_t weekday;                          /* 0 = Sunday */
  uint32_t yearday;                          /* 0-based */
} Time_Calendar;

Time_Calendar time_to_calendar(int64_t posix_seconds);
int64_t time_from_calendar(int64_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute,
                           uint32_t second);

#endif /* SWITCH_HLE_SERVICES_TIME_TIME_H */
