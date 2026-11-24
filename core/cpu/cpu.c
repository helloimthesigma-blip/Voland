#include "cpu/cpu.h"
#include "cpu/backends/interpreter/interp_internal.h"
#include "common/log.h"

const CPU_Backend* cpu_get_active_backend(void) {
#if defined(SWITCH_CPU_BACKEND_NOOP)
  return &CPU_BACKEND_NOOP;
#elif defined(SWITCH_CPU_BACKEND_INTERPRETER)
  return &CPU_BACKEND_INTERPRETER;
#elif defined(SWITCH_CPU_BACKEND_JIT)
  return &CPU_BACKEND_JIT;
#else
# error "No CPU_BACKEND_* define set. Configure the build with -DCPU_BACKEND=noop (or a real backend)."
#endif
}

static bool g_multicore;

/* Code compiled for one mode must not run in the other (the JIT's
 * exclusives and fences): a mode change retires decoded/compiled code. */
void cpu_set_multicore(bool on) {
  if (on != g_multicore) interp_predecode_flush();
  g_multicore = on;
}
bool cpu_multicore(void) { return g_multicore; }

/* The exclusive-access watch (cpu.h). Static: one process at a time. */
#define CPU_WATCH_RING 2048u
typedef struct CPU_Watch_Record {
  uint64_t sequence; /* 0: empty */
  uint64_t tls;
  uint32_t value, desired, now;
  uint8_t kind, flags;
} CPU_Watch_Record;
uint64_t g_cpu_watch_gva;
uintptr_t g_cpu_watch_host;
static CPU_Watch_Record g_watch_ring[CPU_WATCH_RING];
static uint64_t g_watch_next = 1;

void cpu_watch_set(uint64_t gva) {
  g_cpu_watch_gva = gva;
  if (gva) log_warn("[watch-x] recording every exclusive access to 0x%llx", (unsigned long long)gva);
}

void cpu_watch_set_host(uintptr_t host) { g_cpu_watch_host = host; }

void cpu_watch_note(CPU_Watch_Kind kind, uint64_t tls, uint32_t value, uint32_t desired, uint32_t flags, uint32_t now) {
  const uint64_t sequence = __atomic_fetch_add(&g_watch_next, 1u, __ATOMIC_SEQ_CST);
  CPU_Watch_Record *r = &g_watch_ring[sequence % CPU_WATCH_RING];
  r->tls = tls;
  r->value = value;
  r->desired = desired;
  r->now = now;
  r->kind = (uint8_t)kind;
  r->flags = (uint8_t)flags;
  __atomic_store_n(&r->sequence, sequence, __ATOMIC_RELEASE);
}

uint64_t cpu_watch_latest(CPU_Watch_Kind *kind, uint32_t *value, uint32_t *desired, uint32_t *now) {
  const uint64_t last = __atomic_load_n(&g_watch_next, __ATOMIC_ACQUIRE) - 1u;
  if (!last) return 0;
  const CPU_Watch_Record *r = &g_watch_ring[last % CPU_WATCH_RING];
  if (__atomic_load_n(&r->sequence, __ATOMIC_ACQUIRE) != last) return 0;
  *kind = (CPU_Watch_Kind)r->kind;
  *value = r->value;
  *desired = r->desired;
  *now = r->now;
  return last;
}

void cpu_watch_log(void) {
  static const char *const names[] = {"load-ex", "store-ex", "k-read", "k-write", "store"};
  if (!g_cpu_watch_gva) return;
  const uint64_t next = __atomic_load_n(&g_watch_next, __ATOMIC_ACQUIRE);
  const uint64_t first = next > CPU_WATCH_RING ? next - CPU_WATCH_RING : 1u;
  log_warn("[watch-x] the latest %llu accesses to 0x%llx:", (unsigned long long)(next - first),
           (unsigned long long)g_cpu_watch_gva);
  for (uint64_t k = first; k < next; k++) {
    const CPU_Watch_Record *r = &g_watch_ring[k % CPU_WATCH_RING];
    if (__atomic_load_n(&r->sequence, __ATOMIC_ACQUIRE) != k) continue;
    log_warn("[watch-x]   #%llu tls 0x%llx %s 0x%x -> 0x%x flags %u, now 0x%x", (unsigned long long)k,
             (unsigned long long)r->tls, r->kind < sizeof(names) / sizeof(names[0]) ? names[r->kind] : "?", r->value,
             r->desired, r->flags, r->now);
  }
}
