/**
 * time service: clocks over virtual time, the clock shared memory, and a
 * UTC-only time zone service. See time.h.
 */
#include "hle/services/time/time.h"

#include "hle/kernel/scheduler.h"
#include "hle/services/service_util.h"

#define TIME_TICKS_PER_SECOND 19200000ull /* §7: CNTFRQ */
#define TIME_NS_PER_SECOND 1000000000ull
#define TIME_UUID_BYTES 16u
#define TIME_STEADY_POINT_BYTES 0x18u     /* s64 time_point + source id */
#define TIME_CONTEXT_BYTES 0x20u          /* s64 offset + steady point */
#define TIME_CALENDAR_BYTES 0x8u
#define TIME_ADDITIONAL_INFO_BYTES 0x18u
#define TIME_LOCATION_NAME_BYTES 0x24u
#define TIME_RULE_BYTES 0x4000u
#define TIME_RULE_VERSION_BYTES 0x10u
#define TIME_SNAPSHOT_BYTES 0xD0u
#define TIME_SHMEM_COPY_OFFSET 0x8u        /* the two copies after the u32 counter, 8-aligned */
#define TIME_SHMEM_BOOL_COPY_OFFSET 0x4u   /* a bool's copies right after the counter */
#define TIME_ADJUSTMENT_BYTES 0x38u
#define TIME_ADJUSTMENT_SOURCE_ID 0x28u
#define TIME_INITIAL_YEAR 2026u

/* Kinds of ISystemClock (object state). */
enum { CLOCK_USER = 0, CLOCK_NETWORK = 1, CLOCK_LOCAL = 2, CLOCK_EPHEMERAL = 3 };

/* The steady clock's source id; libnx checks the contexts carry it. */
static const uint8_t k_source_id[TIME_UUID_BYTES] = {0x56, 0x6F, 0x6C, 0x61, 0x6E, 0x64, 0x2D, 0x73,
                                                     0x74, 0x65, 0x61, 0x64, 0x79, 0x00, 0x00, 0x01};

static Time_State *state_of(Service_Object *self) { return (Time_State *)self->interface->service_state; }

static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, sizeof(v)); }
static int64_t rd64s(const uint8_t *p) {
  int64_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

/* ------------------------------------------------------------------ */
/* Calendar math (UTC; days-from-civil over 400-year eras).            */
/* ------------------------------------------------------------------ */

#define DAYS_PER_ERA 146097
#define YEARS_PER_ERA 400
#define SECONDS_PER_DAY 86400
#define EPOCH_SHIFT_DAYS 719468 /* 0000-03-01 to 1970-01-01 */

static int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

int64_t time_from_calendar(int64_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute,
                           uint32_t second) {
  const int64_t y = year - (month <= 2u ? 1 : 0);
  const int64_t era = floor_div(y, YEARS_PER_ERA);
  const int64_t yoe = y - era * YEARS_PER_ERA;
  const int64_t mp = (int64_t)month + (month > 2u ? -3 : 9);
  const int64_t doy = (153 * mp + 2) / 5 + (int64_t)day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * DAYS_PER_ERA + doe - EPOCH_SHIFT_DAYS;
  return days * SECONDS_PER_DAY + (int64_t)hour * 3600 + (int64_t)minute * 60 + (int64_t)second;
}

Time_Calendar time_to_calendar(int64_t posix_seconds) {
  Time_Calendar out;
  const int64_t days = floor_div(posix_seconds, SECONDS_PER_DAY);
  const int64_t secs = posix_seconds - days * SECONDS_PER_DAY;
  const int64_t z = days + EPOCH_SHIFT_DAYS;
  const int64_t era = floor_div(z, DAYS_PER_ERA);
  const int64_t doe = z - era * DAYS_PER_ERA;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;
  out.year = yoe + era * YEARS_PER_ERA + (m <= 2 ? 1 : 0);
  out.month = (uint32_t)m;
  out.day = (uint32_t)d;
  out.hour = (uint32_t)(secs / 3600);
  out.minute = (uint32_t)(secs % 3600 / 60);
  out.second = (uint32_t)(secs % 60);
  out.weekday = (uint32_t)(floor_div(days + 4, 7) * -7 + days + 4); /* 1970-01-01 was a Thursday */
  out.yearday = (uint32_t)(days - floor_div(time_from_calendar(out.year, 1, 1, 0, 0, 0), SECONDS_PER_DAY));
  return out;
}

/* CalendarTime (8 bytes) + CalendarAdditionalInfo (0x18) for UTC. */
static void encode_calendar(uint8_t out[TIME_CALENDAR_BYTES + TIME_ADDITIONAL_INFO_BYTES], int64_t posix) {
  const Time_Calendar cal = time_to_calendar(posix);
  memset(out, 0, TIME_CALENDAR_BYTES + TIME_ADDITIONAL_INFO_BYTES);
  const uint16_t year = (uint16_t)cal.year;
  memcpy(out, &year, sizeof(year));
  out[2] = (uint8_t)cal.month;
  out[3] = (uint8_t)cal.day;
  out[4] = (uint8_t)cal.hour;
  out[5] = (uint8_t)cal.minute;
  out[6] = (uint8_t)cal.second;
  uint8_t *info = out + TIME_CALENDAR_BYTES;
  wr32(info, cal.weekday);
  wr32(info + 4, cal.yearday);
  memcpy(info + 8, "UTC", sizeof("UTC"));
  /* DST 0, offset 0 */
}

/* ------------------------------------------------------------------ */
/* Clock values.                                                       */
/* ------------------------------------------------------------------ */

static uint64_t now_ticks(const HLE_Context *c) { return c->scheduler ? c->scheduler->ticks : 0; }
static int64_t steady_seconds(const HLE_Context *c) { return (int64_t)(now_ticks(c) / TIME_TICKS_PER_SECOND); }
static int64_t current_time(const Time_State *s, const HLE_Context *c) { return s->rtc_at_boot + steady_seconds(c); }

static void encode_steady_point(uint8_t out[TIME_STEADY_POINT_BYTES], int64_t seconds) {
  wr64(out, (uint64_t)seconds);
  memcpy(out + 8, k_source_id, TIME_UUID_BYTES);
}

/* SystemClockContext: offset such that offset + steady seconds = now. */
static void encode_context(uint8_t out[TIME_CONTEXT_BYTES], const Time_State *s) {
  wr64(out, (uint64_t)s->rtc_at_boot);
  encode_steady_point(out + 8, 0);
}

/* One seqlock'd shared-memory object: bump the counter, write the copy
 * it now selects. Writes are host-side and atomic w.r.t. the guest
 * (green threads), so both copies end up current. */
static void write_shmem_object(HLE_Context *c, const Time_State *s, uint32_t offset, const void *data, uint32_t size) {
  const uint64_t base = s->shared_memory->guest_pa + offset;
  const uint64_t copies = size == 1u ? TIME_SHMEM_BOOL_COPY_OFFSET : TIME_SHMEM_COPY_OFFSET;
  uint32_t counter = 0;
  (void)vmm_read_physical(c->vmm, base, &counter, sizeof(counter));
  counter++;
  (void)vmm_write_physical(c->vmm, base + copies + (uint64_t)(counter & 1u) * size, data, size);
  (void)vmm_write_physical(c->vmm, base, &counter, sizeof(counter));
}

static void refresh_shared_memory(HLE_Context *c, const Time_State *s) {
  if (!s->shared_memory) return;
  uint8_t steady[TIME_STEADY_POINT_BYTES];
  wr64(steady, 0); /* base_time ns: the steady clock starts with the process */
  memcpy(steady + 8, k_source_id, TIME_UUID_BYTES);
  uint8_t context[TIME_CONTEXT_BYTES];
  encode_context(context, s);
  const uint8_t automatic = 0;
  /* The steady clock unadjusted, from the one clock source. */
  uint8_t adjustment[TIME_ADJUSTMENT_BYTES];
  memset(adjustment, 0, sizeof(adjustment));
  memcpy(adjustment + TIME_ADJUSTMENT_SOURCE_ID, k_source_id, TIME_UUID_BYTES);
  for (int copy = 0; copy < 2; copy++) { /* write both copies */
    write_shmem_object(c, s, TIME_SHMEM_STEADY, steady, sizeof(steady));
    write_shmem_object(c, s, TIME_SHMEM_LOCAL_CONTEXT, context, sizeof(context));
    write_shmem_object(c, s, TIME_SHMEM_NETWORK_CONTEXT, context, sizeof(context));
    write_shmem_object(c, s, TIME_SHMEM_AUTOMATIC_CORRECTION, &automatic, sizeof(automatic));
    write_shmem_object(c, s, TIME_SHMEM_CONTINUOUS_ADJUSTMENT, adjustment, sizeof(adjustment));
  }
}

/* ------------------------------------------------------------------ */
/* time:* commands.                                                    */
/* ------------------------------------------------------------------ */

#define TIME_CLOCK_GETTER(fn, kind)                                                                    \
  static HLE_ServiceResult fn(HLE_Context *c, Service_Object *self, const IPC_Request *req,          \
                              IPC_Response *res) {                                                   \
    (void)c;                                                                                         \
    (void)req;                                                                                       \
    (void)ipc_response_push_object(res, &state_of(self)->system_clock, kind);                        \
    return HLE_RESULT_SUCCESS;                                                                       \
  }

TIME_CLOCK_GETTER(cmd_get_user_clock, CLOCK_USER)
TIME_CLOCK_GETTER(cmd_get_network_clock, CLOCK_NETWORK)
TIME_CLOCK_GETTER(cmd_get_local_clock, CLOCK_LOCAL)
TIME_CLOCK_GETTER(cmd_get_ephemeral_clock, CLOCK_EPHEMERAL)

static HLE_ServiceResult cmd_get_steady_clock(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->steady_clock, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_time_zone_service(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                   IPC_Response *res) {
  (void)c;
  (void)req;
  (void)ipc_response_push_object(res, &state_of(self)->time_zone, 0);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_shared_memory(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)req;
  Time_State *s = state_of(self);
  if (!s->shared_memory) {
    if (!error_is_ok(shared_memory_create(s->pool, c->vmm, TIME_SHARED_MEMORY_BYTES, SHARED_MEMORY_PERM_R,
                                          &s->shared_memory))) {
      return HLE_RESULT_OUT_OF_MEMORY;
    }
    refresh_shared_memory(c, s);
  }
  uint32_t handle = 0;
  shared_memory_retain(s->shared_memory);
  if (!error_is_ok(handle_table_add(&c->process->handles, KERNEL_OBJECT_SHARED_MEMORY, s->shared_memory, &handle))) {
    shared_memory_release(s->pool, s->shared_memory);
    return HLE_RESULT_OUT_OF_HANDLES;
  }
  (void)ipc_response_push_copy_handle(res, handle);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_initial_year(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, TIME_INITIAL_YEAR);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_steady_point_now(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)self;
  (void)req;
  uint8_t point[TIME_STEADY_POINT_BYTES];
  encode_steady_point(point, steady_seconds(c));
  (void)ipc_response_push_bytes(res, point, sizeof(point));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_calculate_base_time_point(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                       IPC_Response *res) {
  (void)c;
  uint8_t context[TIME_CONTEXT_BYTES];
  if (!error_is_ok(ipc_request_read_bytes(req, 0, context, sizeof(context)))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  (void)self;
  (void)ipc_response_push_u64(res, (uint64_t)rd64s(context)); /* offset: the time at steady zero */
  return HLE_RESULT_SUCCESS;
}

static void build_snapshot(uint8_t out[TIME_SNAPSHOT_BYTES], const Time_State *s, HLE_Context *c, uint8_t type) {
  memset(out, 0, TIME_SNAPSHOT_BYTES);
  encode_context(out, s);
  encode_context(out + 0x20, s);
  const int64_t now = current_time(s, c);
  wr64(out + 0x40, (uint64_t)now);
  wr64(out + 0x48, (uint64_t)now);
  uint8_t cal[TIME_CALENDAR_BYTES + TIME_ADDITIONAL_INFO_BYTES];
  encode_calendar(cal, now);
  memcpy(out + 0x50, cal, TIME_CALENDAR_BYTES);
  memcpy(out + 0x58, cal, TIME_CALENDAR_BYTES);
  memcpy(out + 0x60, cal + TIME_CALENDAR_BYTES, TIME_ADDITIONAL_INFO_BYTES);
  memcpy(out + 0x78, cal + TIME_CALENDAR_BYTES, TIME_ADDITIONAL_INFO_BYTES);
  encode_steady_point(out + 0x90, steady_seconds(c));
  memcpy(out + 0xA8, "UTC", sizeof("UTC"));
  out[0xCD] = type;
}

static HLE_ServiceResult cmd_get_clock_snapshot(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                IPC_Response *res) {
  (void)res;
  uint32_t type = 0;
  (void)ipc_request_read_u32(req, 0, &type);
  uint8_t snapshot[TIME_SNAPSHOT_BYTES];
  build_snapshot(snapshot, state_of(self), c, (uint8_t)type);
  if (!service_write_out(c, req, 0, snapshot, sizeof(snapshot))) return HLE_RESULT_INVALID_POINTER;
  return HLE_RESULT_SUCCESS;
}

/* Two snapshots in A buffers -> the difference of their user times. */
static HLE_ServiceResult cmd_snapshot_difference(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                 IPC_Response *res) {
  (void)self;
  uint8_t a[TIME_SNAPSHOT_BYTES], b[TIME_SNAPSHOT_BYTES];
  memset(a, 0, sizeof(a));
  memset(b, 0, sizeof(b));
  (void)service_read_in(c, req, 0, a, sizeof(a));
  (void)service_read_in(c, req, 1, b, sizeof(b));
  (void)ipc_response_push_u64(res, (uint64_t)(rd64s(b + 0x40) - rd64s(a + 0x40)));
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* ISystemClock / ISteadyClock.                                        */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_clock_get_current_time(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                    IPC_Response *res) {
  (void)req;
  (void)ipc_response_push_u64(res, (uint64_t)current_time(state_of(self), c));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_clock_set_current_time(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                    IPC_Response *res) {
  (void)res;
  Time_State *s = state_of(self);
  uint64_t value = 0;
  if (!error_is_ok(ipc_request_read_u64(req, 0, &value))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  s->rtc_at_boot = (int64_t)value - steady_seconds(c);
  refresh_shared_memory(c, s);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_clock_get_context(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)c;
  (void)req;
  uint8_t context[TIME_CONTEXT_BYTES];
  encode_context(context, state_of(self));
  (void)ipc_response_push_bytes(res, context, sizeof(context));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_clock_set_context(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)res;
  Time_State *s = state_of(self);
  uint8_t context[TIME_CONTEXT_BYTES];
  if (!error_is_ok(ipc_request_read_bytes(req, 0, context, sizeof(context)))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  s->rtc_at_boot = rd64s(context) + rd64s(context + 8); /* offset + the context's steady point */
  refresh_shared_memory(c, s);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_steady_rtc_value(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)req;
  (void)ipc_response_push_u64(res, (uint64_t)current_time(state_of(self), c));
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* ITimeZoneService (UTC only).                                        */
/* ------------------------------------------------------------------ */

static HLE_ServiceResult cmd_get_location_name(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                               IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  char name[TIME_LOCATION_NAME_BYTES];
  memset(name, 0, sizeof(name));
  memcpy(name, "UTC", sizeof("UTC"));
  (void)ipc_response_push_bytes(res, name, sizeof(name));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_location_count(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  (void)ipc_response_push_u32(res, 1);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_load_location_names(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                                 IPC_Response *res) {
  (void)self;
  uint32_t index = 0;
  (void)ipc_request_read_u32(req, 0, &index);
  char name[TIME_LOCATION_NAME_BYTES];
  memset(name, 0, sizeof(name));
  memcpy(name, "UTC", sizeof("UTC"));
  const uint32_t count = index == 0 && service_write_out(c, req, 0, name, sizeof(name)) ? 1u : 0u;
  (void)ipc_response_push_u32(res, count);
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_load_rule(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                       IPC_Response *res) {
  (void)self;
  (void)res;
  const IPC_Buffer *buf = service_out_buffer(req, 0);
  if (!buf) return HLE_RESULT_SUCCESS;
  static const uint8_t k_zero[0x400];
  for (uint64_t at = 0; at < buf->size && at < TIME_RULE_BYTES; at += sizeof(k_zero)) {
    const uint64_t n = buf->size - at < sizeof(k_zero) ? buf->size - at : sizeof(k_zero);
    if (!error_is_ok(vmm_write_block(c->vmm, buf->gva + at, k_zero, n))) return HLE_RESULT_INVALID_POINTER;
  }
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_get_rule_version(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)c;
  (void)self;
  (void)req;
  char version[TIME_RULE_VERSION_BYTES];
  memset(version, 0, sizeof(version));
  memcpy(version, "2024a", sizeof("2024a"));
  (void)ipc_response_push_bytes(res, version, sizeof(version));
  return HLE_RESULT_SUCCESS;
}

static HLE_ServiceResult cmd_to_calendar_time(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                              IPC_Response *res) {
  (void)c;
  (void)self;
  uint64_t posix = 0;
  if (!error_is_ok(ipc_request_read_u64(req, 0, &posix))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  uint8_t out[TIME_CALENDAR_BYTES + TIME_ADDITIONAL_INFO_BYTES];
  encode_calendar(out, (int64_t)posix);
  (void)ipc_response_push_bytes(res, out, sizeof(out));
  return HLE_RESULT_SUCCESS;
}

/* CalendarTime in the raw data -> one posix time in the output buffer. */
static HLE_ServiceResult cmd_to_posix_time(HLE_Context *c, Service_Object *self, const IPC_Request *req,
                                           IPC_Response *res) {
  (void)self;
  uint8_t cal[TIME_CALENDAR_BYTES];
  if (!error_is_ok(ipc_request_read_bytes(req, 0, cal, sizeof(cal)))) return IPC_RESULT_SF_INVALID_IN_HEADER;
  uint16_t year = 0;
  memcpy(&year, cal, sizeof(year));
  if (cal[2] < 1 || cal[2] > 12 || cal[3] < 1 || cal[3] > 31 || cal[4] > 23 || cal[5] > 59 || cal[6] > 60) {
    return TIME_RESULT_OUT_OF_RANGE;
  }
  const int64_t posix = time_from_calendar(year, cal[2], cal[3], cal[4], cal[5], cal[6]);
  const uint32_t count = service_write_out(c, req, 0, &posix, sizeof(posix)) ? 1u : 0u;
  (void)ipc_response_push_u32(res, count);
  return HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Tables.                                                             */
/* ------------------------------------------------------------------ */

static const Service_Command k_time_commands[] = {
    {0, cmd_get_user_clock, "GetStandardUserSystemClock"},
    {1, cmd_get_network_clock, "GetStandardNetworkSystemClock"},
    {2, cmd_get_steady_clock, "GetStandardSteadyClock"},
    {3, cmd_get_time_zone_service, "GetTimeZoneService"},
    {4, cmd_get_local_clock, "GetStandardLocalSystemClock"},
    {5, cmd_get_ephemeral_clock, "GetEphemeralNetworkSystemClock"},
    {20, cmd_get_shared_memory, "GetSharedMemoryNativeHandle"},
    {100, service_cmd_out_u8_false, "IsStandardUserSystemClockAutomaticCorrectionEnabled"},
    {101, service_cmd_ok, "SetStandardUserSystemClockAutomaticCorrectionEnabled_stub"},
    {102, cmd_get_initial_year, "GetStandardUserSystemClockInitialYear"},
    {200, service_cmd_out_u8_true, "IsStandardNetworkSystemClockAccuracySufficient"},
    {201, cmd_steady_point_now, "GetStandardUserSystemClockAutomaticCorrectionUpdatedTime"},
    {300, cmd_calculate_base_time_point, "CalculateMonotonicSystemClockBaseTimePoint"},
    {400, cmd_get_clock_snapshot, "GetClockSnapshot"},
    {401, cmd_get_clock_snapshot, "GetClockSnapshotFromSystemClockContext"},
    {500, cmd_snapshot_difference, "CalculateStandardUserSystemClockDifferenceByUser"},
    {501, cmd_snapshot_difference, "CalculateSpanBetween"},
};

static const Service_Command k_system_clock_commands[] = {
    {0, cmd_clock_get_current_time, "GetCurrentTime"},
    {1, cmd_clock_set_current_time, "SetCurrentTime"},
    {2, cmd_clock_get_context, "GetSystemClockContext"},
    {3, cmd_clock_set_context, "SetSystemClockContext"},
};

static const Service_Command k_steady_clock_commands[] = {
    {0, cmd_steady_point_now, "GetCurrentTimePoint"},
    {2, service_cmd_out_u64_zero, "GetTestOffset"},
    {3, service_cmd_ok, "SetTestOffset_stub"},
    {100, cmd_steady_rtc_value, "GetRtcValue"},
    {101, service_cmd_out_u8_false, "IsRtcResetDetected"},
    {102, service_cmd_out_u8_false, "GetSetupResultValue"},
    {200, service_cmd_out_u64_zero, "GetInternalOffset"},
    {201, service_cmd_ok, "SetInternalOffset_stub"},
};

static const Service_Command k_time_zone_commands[] = {
    {0, cmd_get_location_name, "GetDeviceLocationName"},
    {1, service_cmd_ok, "SetDeviceLocationName_stub"},
    {2, cmd_get_location_count, "GetTotalLocationNameCount"},
    {3, cmd_load_location_names, "LoadLocationNameList"},
    {4, cmd_load_rule, "LoadTimeZoneRule"},
    {5, cmd_get_rule_version, "GetTimeZoneRuleVersion"},
    {100, cmd_to_calendar_time, "ToCalendarTime"},
    {101, cmd_to_calendar_time, "ToCalendarTimeWithMyRule"},
    {201, cmd_to_posix_time, "ToPosixTime"},
    {202, cmd_to_posix_time, "ToPosixTimeWithMyRule"},
};

void time_init(Time_State *state, Shared_Memory_Pool *pool, int64_t rtc_at_boot) {
  memset(state, 0, sizeof(*state));
  state->pool = pool;
  state->rtc_at_boot = rtc_at_boot;
  state->interface = SERVICE_INTERFACE("time:u", k_time_commands, 0x1000, state);
  state->system_clock = SERVICE_INTERFACE("ISystemClock", k_system_clock_commands, 0, state);
  state->steady_clock = SERVICE_INTERFACE("ISteadyClock", k_steady_clock_commands, 0, state);
  state->time_zone = SERVICE_INTERFACE("ITimeZoneService", k_time_zone_commands, 0, state);
}

Error time_register(Time_State *state, SM_Registry *registry) {
  static const char *const k_names[] = {"time:u", "time:a", "time:s", "time:r", "time:su"};
  for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
    const Error err = sm_registry_add(registry, k_names[i], &state->interface);
    if (!error_is_ok(err)) return err;
  }
  return OK;
}
