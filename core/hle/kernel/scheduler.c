/**
 * Guest thread scheduler. See scheduler.h.
 */
#include "hle/kernel/scheduler.h"

#include "common/log.h"
#include "hle/hle.h"
#include "hle/kernel/parallel.h"

#include <stdlib.h>
#include <string.h>

#define NS_PER_SECOND 1000000000ull

/* Cycles -> ticks: 19.2 MHz / 1.02 GHz = 192 / 10200. */
#define TICKS_PER_CYCLE_NUMERATOR (SCHEDULER_TIMER_HZ / 100000u)
#define TICKS_PER_CYCLE_DENOMINATOR(sched) (SCHEDULER_CPU_HZ / 100000u * (sched)->cpu_percent / 100u)
#define SCHEDULER_CPU_PERCENT_DEFAULT 100u

void scheduler_init(Scheduler *sched, const CPU_Backend *backend) {
  memset(sched, 0, sizeof(*sched));
  sched->poll_coalescing = true;
  sched->time_limit = SCHEDULER_WAIT_FOREVER;
  sched->backend = backend;
  sched->current = -1;
  sched->next_thread_id = 1;
  sched->device_wake_at = SCHEDULER_WAIT_FOREVER;
  sched->cpu_percent = SCHEDULER_CPU_PERCENT_DEFAULT;
}

void scheduler_set_cpu_percent(Scheduler *sched, uint32_t percent) {
  if (percent < SCHEDULER_CPU_PERCENT_MIN) percent = SCHEDULER_CPU_PERCENT_MIN;
  if (percent > SCHEDULER_CPU_PERCENT_MAX) percent = SCHEDULER_CPU_PERCENT_MAX;
  sched->cpu_percent = percent;
  sched->cycle_remainder = 0;
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

static void forget_coalescing(Scheduler *sched, Sched_Thread *thread) {
  if (!thread->poll_wake_at) return;
  thread->poll_wake_at = 0;
  sched->coalesced_waiting--;
}

void scheduler_free_thread(Scheduler *sched, Sched_Thread *thread) {
  forget_coalescing(sched, thread);
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
  forget_coalescing(sched, thread);
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

/* A thread that does work - not polling - is running or can run. */
static bool worker_active(const Sched_Thread *t) {
  return !t->spinning && (t->on_core || (t->state == THREAD_STATE_RUNNABLE && !t->paused));
}

static bool polling_sleep(const Sched_Thread *t) {
  return t->spinning && t->state == THREAD_STATE_WAITING && t->wait == WAIT_SLEEP;
}

bool scheduler_alone(const Scheduler *sched, const Sched_Thread *self, uint64_t *wake_at) {
  uint64_t earliest = sched->device_wake_at;
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    const Sched_Thread *t = &sched->threads[i];
    if (t == self) continue;
    if (worker_active(t)) return false;
    /* Another poller's next poll is not an event: it can't change anything
     * either. Its own real deadline, if coalesced, is the event. */
    if (t->state != THREAD_STATE_WAITING) continue;
    const uint64_t at = polling_sleep(t) ? SCHEDULER_WAIT_FOREVER : t->wake_at;
    if (at < earliest) earliest = at;
  }
  *wake_at = earliest;
  return true;
}

void scheduler_poll_sleep(Scheduler *sched, Sched_Thread *self, int64_t ns, uint64_t run_cycles) {
  if (!sched->poll_coalescing) { /* the plain scheduler */
    if (ns > 0) scheduler_block(sched, self, WAIT_SLEEP, (uint64_t)ns);
    return;
  }
  const bool yield = ns <= 0;
  self->spinning = run_cycles <= SCHEDULER_SPIN_RUN_CYCLES && (yield || (uint64_t)ns <= SCHEDULER_POLL_SLEEP_MAX_NS);
  if (self->spinning && yield) sched->polling_yields++;
  if (!yield) scheduler_block(sched, self, WAIT_SLEEP, (uint64_t)ns);
  uint64_t event = SCHEDULER_WAIT_FOREVER;
  if (!scheduler_alone(sched, self, &event) || event == SCHEDULER_WAIT_FOREVER || event <= sched->ticks) return;
  if (yield && self->spinning) { /* a lone polling yield: sleep to the next event (nothing else to rotate to) */
    scheduler_block(sched, self, WAIT_SLEEP, SCHEDULER_WAIT_FOREVER);
    self->wake_at = event;
    sched->lone_yields++;
    return;
  }
  if (yield || !self->spinning || event <= self->wake_at) return; /* a working yield just rotates */
  if (self->work_epoch_seen != sched->work_epoch) return; /* work finished while it polled */
  self->poll_wake_at = self->wake_at; /* what it asked for, restored if a worker wakes */
  self->wake_at = event;
  sched->coalesced_polls++;
  sched->coalesced_waiting++;
}

/* A worker can run again: every coalesced poller polls on its own schedule
 * from here on (its requested wake, maybe already due). */
static void uncoalesce(Scheduler *sched) {
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS && sched->coalesced_waiting; i++) {
    Sched_Thread *t = &sched->threads[i];
    if (!t->poll_wake_at) continue;
    if (t->state == THREAD_STATE_WAITING && t->poll_wake_at < t->wake_at) t->wake_at = t->poll_wake_at;
    t->poll_wake_at = 0;
    sched->coalesced_waiting--;
  }
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
  if (sched->coalesced_waiting) {
    for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
      if (worker_active(&sched->threads[i])) {
        uncoalesce(sched);
        break;
      }
    }
  }
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

/* The guest cores in a thread's affinity (all of them if it names none). */
static uint64_t guest_cores_of(const Sched_Thread *t) {
  const uint64_t all = ((uint64_t)1 << THREAD_CORE_COUNT) - 1u;
  const uint64_t mask = t->core_mask & all;
  return mask ? mask : all;
}

bool scheduler_runs_on(const Scheduler *sched, const Sched_Thread *t, uint32_t core) {
  if (sched->host_cores <= 1u || sched->free_placement) return true;
  const uint64_t mask = guest_cores_of(t);
  if (sched->pin_ideal && t->thread.preferred_core < THREAD_CORE_COUNT && ((mask >> t->thread.preferred_core) & 1u))
    return t->thread.preferred_core % sched->host_cores == core;
  for (uint32_t g = 0; g < THREAD_CORE_COUNT; g++)
    if (((mask >> g) & 1u) && g % sched->host_cores == core) return true;
  return false;
}

uint32_t scheduler_guest_core(const Scheduler *sched, const Sched_Thread *t) {
  if (sched->host_cores <= 1u || !t->on_core) return t->thread.preferred_core;
  const uint64_t mask = guest_cores_of(t);
  if (((mask >> t->thread.preferred_core) & 1u) && t->thread.preferred_core % sched->host_cores == t->last_core)
    return t->thread.preferred_core;
  for (uint32_t g = 0; g < THREAD_CORE_COUNT; g++)
    if (((mask >> g) & 1u) && g % sched->host_cores == t->last_core) return g;
  return t->thread.preferred_core;
}

/* Diagnostics: VOLAND_SOLO_THREADS="0x18802c,..." (thread handles) or "all" - with
 * several cores, those threads run alone: nothing else is picked while
 * one runs, and one is picked only when no core is busy. Bisects which
 * threads race. */
#define SOLO_MAX 16u
static uint32_t g_solo[SOLO_MAX], g_solo_count;
static bool g_solo_all;
static uint32_t solo_count(void) {
  static int parsed;
  if (!parsed) {
    parsed = 1;
    const char *spec = getenv("VOLAND_SOLO_THREADS");
    if (spec && strcmp(spec, "all") == 0) {
      g_solo_all = true;
      return g_solo_count = 1u;
    }
    while (spec && *spec && g_solo_count < SOLO_MAX) {
      char *end = NULL;
      g_solo[g_solo_count++] = (uint32_t)strtoul(spec, &end, 0);
      spec = end && *end == ',' ? end + 1 : NULL;
    }
  }
  return g_solo_count;
}
static bool solo(const Sched_Thread *t) {
  if (g_solo_all) return true;
  for (uint32_t i = 0; i < g_solo_count; i++)
    if (g_solo[i] == t->handle) return true;
  return false;
}

/* With parallel cores, the pick follows the Switch's per-core rule:
 * 1. Each guest core's leader: its best runnable thread by priority (a
 *    lower priority never runs on a core while a better thread there can),
 *    then working before polling (a yield goes to the back of its
 *    priority), then a thread already on this host core, then round-robin.
 * 2. Among the leaders this host core may run: working before polling,
 *    then the same order.
 * The serial pick (better(), below) instead lets a poller's own-core
 * lower priorities run - on one host thread a Switch core's polling would
 * otherwise eat the other cores' time (docs/PARALLEL.md "Pollers and
 * priority"). On parallel cores that cost is gone, and Tears of the
 * Kingdom's loader, which counts on the per-core rule, mostly stopped
 * crashing: on three cores 7 of 9 runs clean, 1 crash, 1 stall at boot,
 * vs about 3 of 4 crashing before. Silksong, Celeste, Mario Kart 8 and
 * Animal Crossing run the same (docs/PARALLEL.md). */
static bool leads_core(const Sched_Thread *t, const Sched_Thread *b, bool affine, uint32_t core) {
  if (t->thread.priority != b->thread.priority) return t->thread.priority < b->thread.priority;
  if (t->spinning != b->spinning) return !t->spinning;
  if (affine) {
    const bool t_home = t->last_core == core, b_home = b->last_core == core;
    if (t_home != b_home) return t_home;
  }
  return t->last_run < b->last_run;
}

static bool better_leader(const Sched_Thread *t, const Sched_Thread *b, bool affine, uint32_t core) {
  if (t->spinning != b->spinning) return !t->spinning;
  return leads_core(t, b, affine, core);
}

static int32_t pick_leaders(const Scheduler *sched, bool affine, uint32_t core, bool busy) {
  int32_t leader[THREAD_CORE_COUNT];
  for (uint32_t g = 0; g < THREAD_CORE_COUNT; g++) leader[g] = -1;
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    const Sched_Thread *t = &sched->threads[i];
    if (t->state != THREAD_STATE_RUNNABLE || t->paused || t->on_core) continue;
    if (busy && solo(t)) continue;
    const uint64_t mask = guest_cores_of(t);
    for (uint32_t g = 0; g < THREAD_CORE_COUNT; g++)
      if (((mask >> g) & 1u) && (leader[g] < 0 || leads_core(t, &sched->threads[leader[g]], affine, core)))
        leader[g] = (int32_t)i;
  }
  int32_t best = -1;
  for (uint32_t g = 0; g < THREAD_CORE_COUNT; g++) {
    const int32_t i = leader[g];
    if (i < 0 || i == best) continue;
    const Sched_Thread *t = &sched->threads[i];
    if (affine && !scheduler_runs_on(sched, t, core)) continue;
    if (best < 0 || better_leader(t, &sched->threads[best], affine, core)) best = i;
  }
  return best;
}

static int32_t pick(Scheduler *sched, bool affine, uint32_t core) {
  int32_t best = -1;
  bool busy = false;
  if (sched->host_cores > 1u && solo_count()) {
    for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
      const Sched_Thread *t = &sched->threads[i];
      if (!t->on_core) continue;
      if (solo(t)) return -1;
      busy = true;
    }
  }
  if (sched->host_cores > 1u) return pick_leaders(sched, affine, core, busy);
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    const Sched_Thread *t = &sched->threads[i];
    if (t->state != THREAD_STATE_RUNNABLE || t->paused || t->on_core) continue;
    if (busy && solo(t)) continue;
    if (affine && !scheduler_runs_on(sched, t, core)) continue;
    if (best < 0 || better(t, &sched->threads[best], affine, core)) best = (int32_t)i;
  }
  return best;
}

int32_t scheduler_pick(Scheduler *sched) { return pick(sched, false, 0); }
int32_t scheduler_pick_for_core(Scheduler *sched, uint32_t core) { return pick(sched, true, core); }

/* Jumps time to `target`, or only up to the pacing limit: true if the
 * target was reached (its event is due). */
static bool jump_to(Scheduler *sched, uint64_t target) {
  if (target > sched->time_limit) {
    if (sched->time_limit > sched->ticks) sched->ticks = sched->time_limit;
    return false;
  }
  if (target > sched->ticks) sched->ticks = target;
  return true;
}

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
    (void)jump_to(sched, sched->device_wake_at);
    return SCHEDULER_IDLE;
  }
  if (earliest == SCHEDULER_WAIT_FOREVER) {
    /* Nothing will wake on its own - unless a host-side event (a GPU
     * fence) does, which pacing waits for in wall time. */
    if (sched->time_limit != SCHEDULER_WAIT_FOREVER) {
      (void)jump_to(sched, sched->time_limit);
      return SCHEDULER_IDLE;
    }
    return SCHEDULER_DEADLOCK;
  }
  if (jump_to(sched, earliest)) scheduler_expire_timeouts(sched);
  return SCHEDULER_IDLE;
}

Scheduler_Status scheduler_finish_run(Scheduler *sched, const CPU_Backend *backend, Sched_Thread *thread,
                                      uint64_t start_ticks, CPU_ExitReason exit_reason) {
  const uint64_t cycles = backend->get_cycles_consumed(thread->thread.cpu_state);
  if (start_ticks == sched->ticks) {
    /* Time did not move during the run (always so in serial mode): exact,
     * with the sub-tick remainder carried. */
    const uint64_t scaled = cycles * TICKS_PER_CYCLE_NUMERATOR + sched->cycle_remainder;
    sched->ticks += scaled / TICKS_PER_CYCLE_DENOMINATOR(sched);
    sched->cycle_remainder = scaled % TICKS_PER_CYCLE_DENOMINATOR(sched);
  } else {
    /* Another core moved time meanwhile: this run ends at its own start
     * plus its cycles, and time is the latest any core has reached
     * (docs/PARALLEL.md "Virtual time"). */
    const uint64_t end = start_ticks + cycles * TICKS_PER_CYCLE_NUMERATOR / TICKS_PER_CYCLE_DENOMINATOR(sched);
    if (end > sched->ticks) sched->ticks = end;
  }
  thread->last_run = ++sched->run_counter;
  thread->cycles_run += cycles;
  if (exit_reason != CPU_EXIT_SVC) thread->spinning = false; /* it ran its budget: real work */
  if (!thread->spinning) sched->work_epoch++;

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

void scheduler_gpu_lock(Scheduler *sched) {
  if (sched && sched->parallel) parallel_gpu_lock(sched->parallel);
}

void scheduler_gpu_unlock(Scheduler *sched) {
  if (sched && sched->parallel) parallel_gpu_unlock(sched->parallel);
}

void scheduler_gpu_begin(Scheduler *sched, Kernel_Suspend *saved) {
  if (sched && sched->parallel) parallel_gpu_begin(sched->parallel, saved);
}

void scheduler_gpu_end(Scheduler *sched, const Kernel_Suspend *saved) {
  if (sched && sched->parallel) parallel_gpu_end(sched->parallel, saved);
}

/* Diagnostics: VOLAND_RUN_TRACE=FROM:TO (virtual ticks) logs every serial
 * run in that window - thread, start, cycles, how it ended, the pc. */
static void run_trace(const Scheduler *sched, const CPU_Backend *backend, Sched_Thread *thread, uint64_t start,
                      CPU_ExitReason exit_reason) {
  static int parsed;
  static uint64_t from, to;
  if (!parsed) {
    const char *spec = getenv("VOLAND_RUN_TRACE");
    char *end = NULL;
    parsed = spec ? 2 : 1;
    if (spec) {
      from = strtoull(spec, &end, 0);
      to = end && *end == ':' ? strtoull(end + 1, NULL, 0) : UINT64_MAX;
    }
  }
  if (parsed != 2 || start < from || start > to) return;
  (void)sched;
  log_info("[run] h%x %llu +%llu exit %d pc %llx", thread->handle, (unsigned long long)start,
           (unsigned long long)backend->get_cycles_consumed(thread->thread.cpu_state), (int)exit_reason,
           (unsigned long long)backend->get_pc(thread->thread.cpu_state));
}

/* Diagnostics: VOLAND_SERIAL_ROTATE=N - the serial scheduler takes guest
 * cores 0..N-1 in turn, one slice each, and runs that core's best thread:
 * the order several host cores give (a lower-priority thread of one core
 * runs while another core's better thread waits), deterministically. */
static uint32_t rotate_cores(void) {
  static int parsed;
  static uint32_t cores;
  if (!parsed) {
    parsed = 1;
    const char *spec = getenv("VOLAND_SERIAL_ROTATE");
    cores = spec ? (uint32_t)strtoul(spec, NULL, 0) : 0u;
    if (cores > THREAD_CORE_COUNT) cores = THREAD_CORE_COUNT;
  }
  return cores;
}
static int32_t pick_rotating(Scheduler *sched) {
  static uint32_t turn;
  const uint32_t cores = rotate_cores();
  for (uint32_t k = 0; k < cores; k++) {
    const uint32_t g = (turn + k) % cores;
    int32_t best = -1;
    for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
      const Sched_Thread *t = &sched->threads[i];
      if (t->state != THREAD_STATE_RUNNABLE || t->paused) continue;
      /* Guest cores past the rotation share its last one. */
      uint64_t mask = guest_cores_of(t), own = 0;
      for (uint32_t c = 0; c < THREAD_CORE_COUNT; c++)
        if ((mask >> c) & 1u) own |= (uint64_t)1 << (c < cores ? c : cores - 1u);
      if (!((own >> g) & 1u)) continue;
      if (best < 0 || better(t, &sched->threads[best], false, 0)) best = (int32_t)i;
    }
    if (best >= 0) {
      turn = g + 1u;
      return best;
    }
  }
  return -1;
}

Scheduler_Status scheduler_tick(Scheduler *sched, const CPU_Backend *backend, uint64_t budget,
                                CPU_ExitReason *reason) {
  if (sched->process_crashed) return SCHEDULER_CRASHED;
  if (sched->process_exited) return SCHEDULER_EXITED;
  scheduler_expire_timeouts(sched);

  const int32_t index = rotate_cores() ? pick_rotating(sched) : scheduler_pick(sched);
  if (index < 0) return scheduler_idle(sched);

  Sched_Thread *thread = &sched->threads[index];
  sched->current = index;
  const uint64_t start_ticks = sched->ticks;
  thread->work_epoch_seen = sched->work_epoch;
  backend->set_sys_reg(thread->thread.cpu_state, CPU_SYSREG_CNTVCT_EL0, start_ticks);
  const CPU_ExitReason exit_reason = backend->run(thread->thread.cpu_state, budget);
  sched->current = -1;
  if (reason) *reason = exit_reason;
  run_trace(sched, backend, thread, start_ticks, exit_reason);
  return scheduler_finish_run(sched, backend, thread, start_ticks, exit_reason);
}
