#include "cpu/cpu.h"
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

void cpu_set_multicore(bool on) { g_multicore = on; }
bool cpu_multicore(void) { return g_multicore; }
