/**
 * Save states (emulator_savestate_*) across sessions: what a state
 * carries that only meant something in the session that made it.
 *
 *   1. The GPU latency ring (nvdrv_bound_gpu_latency) names GPU-thread
 *      calls of the saving session; restored into a session whose GPU
 *      thread has made fewer, the next device update must not wait for
 *      them (it would never return).
 */
#define CHECK_NAME "savestate_test"
#include "check.h"

#include "guest_fixture.h"
#include "guest/threads.inc"

#include <stdio.h>
#include <string.h>

static Guest_Run g_run; /* Emulator is large; keep it off the stack */
static Savestate_Plan g_plan;

#define BUDGET 997u
#define MAX_SLICES 2000000ull
#define FAR_AHEAD_CALLS 0x7fffffffu /* a longer session's GPU thread call count */

static void test_latency_ring_from_another_session(void) {
  guest_boot(&g_run, &CPU_BACKEND_INTERPRETER, k_guest_threads, sizeof(k_guest_threads));
  CHECK(emulator_set_gpu_async(&g_run.emu, true));
  CHECK(error_is_ok(emulator_savestate_begin_restore(&g_run.emu, &g_plan)));
  /* The restored bytes: one submission of the saving session, long overdue. */
  Nvdrv_State *nv = &g_run.emu.nvdrv;
  nv->submit_head = 0;
  nv->submit_tail = 1;
  nv->submit_calls[0] = FAR_AHEAD_CALLS;
  nv->submit_ticks[0] = 0;
  CHECK(error_is_ok(emulator_savestate_finish_restore(&g_run.emu, true)));
  CHECK(nv->submit_head == nv->submit_tail);
  CHECK(guest_run(&g_run, BUDGET, MAX_SLICES) == EMULATOR_EXITED);
  CHECK(strstr(g_run.output, "counter 2000") != NULL);
  (void)emulator_set_gpu_async(&g_run.emu, false);
  guest_shutdown(&g_run);
  printf("[savestate_test] a restored GPU latency ring does not wait on another session's calls\n");
}

int main(void) {
  test_latency_ring_from_another_session();
  printf("[savestate_test] passed\n");
  return 0;
}
