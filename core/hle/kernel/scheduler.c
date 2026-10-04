/**
 * Guest thread scheduler. See scheduler.h.
 */
#include "hle/kernel/scheduler.h"

#include "common/log.h"
#include "hle/hle.h"
#include "hle/kernel/parallel.h"

#include <string.h>

#define NS_PER_SECOND 1000000000ull

/* Cycles -> ticks: 19.2 MHz / 1.02 GHz = 192 / 10200. */
#define TICKS_PER_CYCLE_NUMERATOR (SCHEDULER_TIMER_HZ / 100000u)
#define TICKS_PER_CYCLE_DENOMINATOR (SCHEDULER_CPU_HZ / 100000u)

void scheduler_init(Scheduler *sched, const CPU_Backend *backend) {
  memset(sched, 0, sizeof(*sched));
  sched->backend = backend;
  sched->current = -1;
  sched->next_thread_id = 1;
  sched->device_wake_at = SCHEDULER_WAIT_FOREVER;
}

uint64_t scheduler_ns_to_ticks(uint64_t ns) {
  /* ticks = ns * 19.2e6 / 1e9 = ns * 12 / 625, without overflowing. */
  return (ns / 625u) * 12u + ((ns % 625u) * 12u) / 625u;
}

Sched_Thread *scheduler_new_thread(Scheduler *sched) {
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    Sched_Thread *t = &sched->threads[i];
    if (t->state != THREAD_STATE_FREE) continue;
    memset(t, 0, sizeof(*t));
    t->thread_id = sched->next_thread_id++;
    t->wake_at = SCHEDULER_WAIT_FOREVER;
    return t;
  }
  return NULL;
}

void scheduler_free_thread(Scheduler *sched, Sched_Thread *thread) {
  (void)sched;
  memset(thread, 0, sizeof(*thread));
}

Sched_Thread *scheduler_current(Scheduler *sched) {
  return sched->current < 0 ? NULL : &sched->threads[sched->current];
}

Sched_Thread *scheduler_thread_by_state(Scheduler *sched, const CPU_State *state) {
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    if (sched->threads[i].state != THREAD_STATE_FREE && sched->threads[i].thread.cpu_state == state) {
      return &sched->threads[i];
    }
  }
  return NULL;
}

void scheduler_block(Scheduler *sched, Sched_Thread *thread, Wait_Kind kind, uint64_t timeout_ns) {
  thread->state = THREAD_STATE_WAITING;
  thread->wait = kind;
  thread->wait_sequence = ++sched->wait_counter;
  thread->wake_at = timeout_ns == SCHEDULER_WAIT_FOREVER ? SCHEDULER_WAIT_FOREVER
                                                         : sched->ticks + scheduler_ns_to_ticks(timeout_ns);
}

static CPU_Register_File *regs_of(Sched_Thread *thread, const CPU_Backend *backend) {
  return backend->get_register_file(thread->thread.cpu_state);
}

void scheduler_wake(Scheduler *sched, Sched_Thread *thread, uint32_t result) {
  thread->state = THREAD_STATE_RUNNABLE;
  thread->wait = WAIT_NONE;
  thread->wake_at = SCHEDULER_WAIT_FOREVER;
  thread->wait_handle_count = 0;
  regs_of(thread, sched->backend)->x[0] = result;
}

void scheduler_exit_thread(Scheduler *sched, Sched_Thread *thread, const CPU_Backend *backend) {
  thread->state = THREAD_STATE_DEAD;
  thread->wait = WAIT_NONE;
  /* Wake every thread waiting on this one (a dead thread is signaled). */
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    Sched_Thread *t = &sched->threads[i];
    if (t->state != THREAD_STATE_WAITING || t->wait != WAIT_SYNCHRONIZATION) continue;
    for (uint32_t h = 0; h < t->wait_handle_count; h++) {
      if (t->wait_handles[h] == thread->handle) {
        scheduler_wake(sched, t, HLE_RESULT_SUCCESS);
        regs_of(t, backend)->x[1] = h;
        break;
      }
    }
  }
}

bool scheduler_alone(const Scheduler *sched, const Sched_Thread *self, uint64_t *wake_at) {
  uint64_t earliest = sched->device_wake_at;
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    const Sched_Thread *t = &sched->threads[i];
    if (t == self) continue;
    if (!t->spinning && (t->on_core || (t->state == THREAD_STATE_RUNNABLE && !t->paused))) return false;
    if (t->state == THREAD_STATE_WAITING && t->wake_at < earliest) earliest = t->wake_at;
  }
  *wake_at = earliest;
  return true;
}

void scheduler_wake_off_core_waiters(Scheduler *sched, const Sched_Thread *thread) {
  const uint64_t index = (uint64_t)(thread - sched->threads);
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    Sched_Thread *t = &sched->threads[i];
    if (t->state == THREAD_STATE_WAITING && t->wait == WAIT_OFF_CORE && t->wait_address == index) {
      scheduler_wake(sched, t, HLE_RESULT_SUCCESS);
    }
  }
}

/* Wakes every thread whose timeout has passed. */
void scheduler_expire_timeouts(Scheduler *sched) {
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    Sched_Thread *t = &sched->threads[i];
    if (t->state != THREAD_STATE_WAITING || t->wake_at > sched->ticks) continue;
    scheduler_wake(sched, t, t->wait == WAIT_SLEEP ? HLE_RESULT_SUCCESS : HLE_RESULT_TIMED_OUT);
  }
}

/* Is `t` a better pick than `b`? With an affine core, home threads win
 * ties of priority before round-robin order does. */
static bool better(const Sched_Thread *t, const Sched_Thread *b, bool affine, uint32_t core) {
  /* A thread that is only polling for work (Sched_Thread.spinning) runs
   * when nothing that does work can: on the Switch it would spin on a core
   * of its own instead of taking turns with the threads that feed it. */
  if (t->spinning != b->spinning) return !t->spinning;
  if (t->thread.priority != b->thread.priority) return t->thread.priority < b->thread.priority;
  if (affine) {
    const bool t_home = t->last_core == core, b_home = b->last_core == core;
    if (t_home != b_home) return t_home;
  }
  return t->last_run < b->last_run;
}

static int32_t pick(Scheduler *sched, bool affine, uint32_t core) {
  int32_t best = -1;
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    const Sched_Thread *t = &sched->threads[i];
    if (t->state != THREAD_STATE_RUNNABLE || t->paused || t->on_core) continue;
    if (best < 0 || better(t, &sched->threads[best], affine, core)) best = (int32_t)i;
  }
  return best;
}

int32_t scheduler_pick(Scheduler *sched) { return pick(sched, false, 0); }
int32_t scheduler_pick_for_core(Scheduler *sched, uint32_t core) { return pick(sched, true, core); }

Scheduler_Status scheduler_idle(Scheduler *sched) {
  uint64_t earliest = SCHEDULER_WAIT_FOREVER;
  bool alive = false;
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    const Sched_Thread *t = &sched->threads[i];
    if (t->state == THREAD_STATE_WAITING) {
      alive = true;
      if (t->wake_at < earliest) earliest = t->wake_at;
    } else if (t->state == THREAD_STATE_CREATED) {
      alive = true;
    }
  }
  if (!alive) return SCHEDULER_EXITED;
  if (sched->device_wake_at < earliest) {
    /* A device signals first: jump there; its update runs next slice. */
    if (sched->device_wake_at > sched->ticks) sched->ticks = sched->device_wake_at;
    return SCHEDULER_IDLE;
  }
  if (earliest == SCHEDULER_WAIT_FOREVER) return SCHEDULER_DEADLOCK;
  sched->ticks = earliest;
  scheduler_expire_timeouts(sched);
  return SCHEDULER_IDLE;
}

Scheduler_Status scheduler_finish_run(Scheduler *sched, const CPU_Backend *backend, Sched_Thread *thread,
                                      uint64_t start_ticks, CPU_ExitReason exit_reason) {
  const uint64_t cycles = backend->get_cycles_consumed(thread->thread.cpu_state);
  if (start_ticks == sched->ticks) {
    /* Time did not move during the run (always so in serial mode): exact,
     * with the sub-tick remainder carried. */
    const uint64_t scaled = cycles * TICKS_PER_CYCLE_NUMERATOR + sched->cycle_remainder;
    sched->ticks += scaled / TICKS_PER_CYCLE_DENOMINATOR;
    sched->cycle_remainder = scaled % TICKS_PER_CYCLE_DENOMINATOR;
  } else {
    /* Another core moved time meanwhile: this run ends at its own start
     * plus its cycles, and time is the latest any core has reached
     * (docs/PARALLEL.md "Virtual time"). */
    const uint64_t end = start_ticks + cycles * TICKS_PER_CYCLE_NUMERATOR / TICKS_PER_CYCLE_DENOMINATOR;
    if (end > sched->ticks) sched->ticks = end;
  }
  thread->last_run = ++sched->run_counter;
  if (exit_reason != CPU_EXIT_SVC) thread->spinning = false; /* it ran its budget: real work */

  if (exit_reason == CPU_EXIT_FAULT || exit_reason == CPU_EXIT_BREAKPOINT) {
    if (!sched->process_crashed) {
      sched->process_crashed = true;
      sched->crash_pc = backend->get_pc(thread->thread.cpu_state);
      sched->crash_address = backend->get_fault_address(thread->thread.cpu_state);
    }
    log_error("[scheduler] thread %llu stopped: %s at pc=0x%010llx (address 0x%010llx)",
              (unsigned long long)thread->thread_id, exit_reason == CPU_EXIT_FAULT ? "fault" : "breakpoint",
              (unsigned long long)backend->get_pc(thread->thread.cpu_state),
              (unsigned long long)backend->get_fault_address(thread->thread.cpu_state));
    return SCHEDULER_CRASHED;
  }
  if (sched->process_crashed) return SCHEDULER_CRASHED;
  if (sched->process_exited) return SCHEDULER_EXITED;
  return SCHEDULER_RAN;
}

void scheduler_kernel_enter(Scheduler *sched, const CPU_State *state) {
  if (sched && sched->parallel) parallel_kernel_enter(sched->parallel, state);
}

void scheduler_kernel_exit(Scheduler *sched) {
  if (sched && sched->parallel) parallel_kernel_exit(sched->parallel);
}

Scheduler_Status scheduler_tick(Scheduler *sched, const CPU_Backend *backend, uint64_t budget,
                                CPU_ExitReason *reason) {
  if (sched->process_crashed) return SCHEDULER_CRASHED;
  if (sched->process_exited) return SCHEDULER_EXITED;
  scheduler_expire_timeouts(sched);

  const int32_t index = scheduler_pick(sched);
  if (index < 0) return scheduler_idle(sched);

  Sched_Thread *thread = &sched->threads[index];
  sched->current = index;
  const uint64_t start_ticks = sched->ticks;
  backend->set_sys_reg(thread->thread.cpu_state, CPU_SYSREG_CNTVCT_EL0, start_ticks);
  const CPU_ExitReason exit_reason = backend->run(thread->thread.cpu_state, budget);
  sched->current = -1;
  if (reason) *reason = exit_reason;
  return scheduler_finish_run(sched, backend, thread, start_ticks, exit_reason);
}
