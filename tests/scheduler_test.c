/**
 * Guest threads end to end (§7, §12): a real ARM64 program
 * (tests/guest/threads.s) runs on the interpreter under the scheduler -
 * CreateThread/StartThread, two workers contending a libnx-style mutex
 * (exclusive monitor fast path, ArbitrateLock/Unlock slow path),
 * WaitSynchronization on thread handles, SleepThread and virtual time,
 * OutputDebugString and ExitProcess. Run at several slice budgets so
 * preemption lands in different places, including inside critical
 * sections.
 */
#define CHECK_NAME "scheduler_test"
#include "check.h"

#include "guest_fixture.h"
#include "guest/condvar.inc"
#include "guest/events.inc"
#include "guest/threads.inc"

#include <stdio.h>
#include <string.h>

static Guest_Run g_run; /* Emulator is large; keep it off the stack */

typedef struct Program {
  const char *name;
  const uint8_t *code;
  size_t size;
  const char *expected;
} Program;

int main(void) {
  static const Program programs[] = {
      {"threads", k_guest_threads, sizeof(k_guest_threads), "counter 2000, sleep ok"},
      {"condvar", k_guest_condvar, sizeof(k_guest_condvar), "sum 125250, timeouts ok"},
      {"events", k_guest_events, sizeof(k_guest_events), "signal, reset, timeout, close ok"},
  };
  static const uint64_t budgets[] = {7, 31, 100, 997, 100000};
  for (size_t p = 0; p < sizeof(programs) / sizeof(programs[0]); p++)
  for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
    guest_boot(&g_run, &CPU_BACKEND_INTERPRETER, programs[p].code, programs[p].size);
    const Emulator_Status status = guest_run(&g_run, budgets[i], 2000000);
    if (status != EMULATOR_EXITED || strstr(g_run.output, programs[p].expected) == NULL) {
      fprintf(stderr, "budget %llu: status %d after %llu slices, output: %s\n", (unsigned long long)budgets[i],
              (int)status, (unsigned long long)g_run.slices, g_run.output);
      CHECK(false);
    }
    printf("[scheduler_test] %-8s budget %6llu: %llu slices, %llu SVCs, virtual time %llu ticks\n",
           programs[p].name, (unsigned long long)budgets[i], (unsigned long long)g_run.slices,
           (unsigned long long)g_run.emu.hle.svc_call_count, (unsigned long long)g_run.emu.scheduler.ticks);
    guest_shutdown(&g_run);
  }
  printf("[scheduler_test] passed\n");
  return 0;
}
