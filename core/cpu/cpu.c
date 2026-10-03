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
