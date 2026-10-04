/**
 * Parallel guest threads. See parallel.h and docs/PARALLEL.md.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#include "hle/kernel/parallel.h"

#include "common/log.h"

#if (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)) || defined(_WIN32)
#define PARALLEL_THREADS 0
#else
#define PARALLEL_THREADS 1
#endif

#if !PARALLEL_THREADS

bool parallel_supported(void) { return false; }
Parallel *parallel_create(Scheduler *sched, const CPU_Backend *backend, uint32_t cores) {
  (void)sched;
  (void)backend;
  (void)cores;
  return NULL;
}
void parallel_destroy(Parallel *p) { (void)p; }
uint32_t parallel_core_count(const Parallel *p) {
  (void)p;
  return 0;
}
Parallel_Stats parallel_stats(const Parallel *p) {
  (void)p;
  const Parallel_Stats none = {0, 0, 0, 0};
  return none;
}
#ifdef __EMSCRIPTEN__
/* The driver is Emscripten's main runtime thread (the CPU worker), and
 * every syscall a core makes - a log line's write to stderr - is proxied
 * to it synchronously. Its queue only runs when asked: wait in short
 * steps and drain it between them, or the first log line deadlocks. */
#define PARALLEL_DRIVER_POLL_NS 1000000l
#define NS_PER_SECOND 1000000000l
static void wait_on_channel(Parallel *p) {
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_nsec += PARALLEL_DRIVER_POLL_NS;
  if (until.tv_nsec >= NS_PER_SECOND) {
    until.tv_sec++;
    until.tv_nsec -= NS_PER_SECOND;
  }
  (void)pthread_cond_timedwait(&p->channel_cv, &p->channel, &until);
  pthread_mutex_unlock(&p->channel);
  emscripten_current_thread_process_queued_calls();
  pthread_mutex_lock(&p->channel);
}
#else
static void wait_on_channel(Parallel *p) { pthread_cond_wait(&p->channel_cv, &p->channel); }
#endif

Scheduler_Status parallel_tick(Parallel *p, uint64_t budget) {
  (void)p;
  (void)budget;
  return SCHEDULER_DEADLOCK;
}
void parallel_kernel_enter(Parallel *p, const CPU_State *state) {
  (void)p;
  (void)state;
}
void parallel_kernel_exit(Parallel *p) { (void)p; }
void parallel_on_driver(void (*fn)(void *ctx), void *ctx) { fn(ctx); }
bool parallel_on_core_thread(void) { return false; }

#else

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#endif

typedef struct Core {
  Parallel *parallel;
  uint32_t index;
  pthread_t thread;
  int32_t running;          /* scheduler index of the thread in run(), -1 */
} Core;

/* A host call waiting for the driver. */
typedef struct Host_Call {
  void (*fn)(void *ctx);
  void *ctx;
  bool done;
} Host_Call;

struct Parallel {
  Scheduler *sched;
  const CPU_Backend *backend;

  pthread_mutex_t kernel;   /* the kernel lock: scheduler, HLE, and the fields below */
  pthread_cond_t core_cv;   /* to cores: a slice opened, a run ended, a thread may have woken, quit */
  uint64_t slice;           /* generation; a new value opens a slice */
  uint64_t budget;          /* cycles per core in this slice */
  uint32_t busy;            /* cores inside backend->run */
  uint32_t working;         /* cores not yet done with this slice */
  bool quit;
  int32_t saved_current;    /* `current` around the SVC holding the lock */

  pthread_mutex_t channel;  /* driver <-> cores: slice end and host calls */
  pthread_cond_t channel_cv;
  bool slice_done;
  Host_Call *call;          /* pending host call, or NULL */

  uint64_t slice_max;       /* this slice: the busiest core's cycles */
  uint32_t slice_cores;     /* this slice: cores that ran something */
  Parallel_Stats stats;

  uint32_t core_count;
  Core cores[PARALLEL_MAX_CORES];
};

static _Thread_local Core *t_core;
static Parallel *g_parallel; /* the attached scheduler, for parallel_on_driver */

bool parallel_supported(void) { return true; }

uint32_t parallel_core_count(const Parallel *p) { return p ? p->core_count : 0; }

bool parallel_on_core_thread(void) { return t_core != NULL; }

Parallel_Stats parallel_stats(const Parallel *p) { return p->stats; }

/* A core is done with the slice, having run `used` cycles. */
static void finish_slice(Parallel *p, uint64_t used) {
  p->stats.cycles += used;
  if (used > p->slice_max) p->slice_max = used;
  if (used) p->slice_cores++;
  if (--p->working != 0) return;
  p->stats.slices++;
  p->stats.span += p->slice_max;
  if (p->slice_cores >= 2u) p->stats.shared_slices++;
  pthread_mutex_lock(&p->channel);
  p->slice_done = true;
  pthread_cond_broadcast(&p->channel_cv);
  pthread_mutex_unlock(&p->channel);
}

/* One core's share of a slice; returns its cycles. Called and returns
 * with the kernel lock.
 * A core keeps picking threads while another core is still running (it
 * would only wait otherwise) and its cycles last; alone, it makes exactly
 * one run, which is what makes one core equal serial mode. */
static uint64_t run_slice_on_core(Parallel *p, Core *core) {
  Scheduler *sched = p->sched;
  const CPU_Backend *backend = p->backend;
  uint64_t used = 0;
  bool ran = false;
  for (;;) {
    if (sched->process_crashed || sched->process_exited) return used;
    if (ran && (p->busy == 0 || used >= p->budget)) return used;
    scheduler_expire_timeouts(sched);
    const int32_t index = scheduler_pick_for_core(sched, core->index);
    if (index < 0) {
      if (p->busy == 0) return used; /* nothing can change until the next slice */
      pthread_cond_wait(&p->core_cv, &p->kernel);
      continue;
    }
    Sched_Thread *thread = &sched->threads[index];
    thread->on_core = true;
    thread->last_core = core->index;
    thread->work_epoch_seen = sched->work_epoch;
    core->running = index;
    p->busy++;
    const uint64_t start_ticks = sched->ticks;
    backend->set_sys_reg(thread->thread.cpu_state, CPU_SYSREG_CNTVCT_EL0, start_ticks);
    pthread_mutex_unlock(&p->kernel);

    const CPU_ExitReason exit_reason = backend->run(thread->thread.cpu_state, p->budget - used);

    pthread_mutex_lock(&p->kernel);
    p->busy--;
    core->running = -1;
    thread->on_core = false;
    scheduler_wake_off_core_waiters(sched, thread);
    used += backend->get_cycles_consumed(thread->thread.cpu_state);
    ran = true;
    (void)scheduler_finish_run(sched, backend, thread, start_ticks, exit_reason);
    pthread_cond_broadcast(&p->core_cv); /* its thread is free again; idle cores re-check */
  }
}

static void *core_main(void *arg) {
  Core *core = (Core *)arg;
  Parallel *p = core->parallel;
  t_core = core;
  uint64_t seen = 0;
  pthread_mutex_lock(&p->kernel);
  for (;;) {
    while (p->slice == seen && !p->quit) pthread_cond_wait(&p->core_cv, &p->kernel);
    if (p->quit) break;
    seen = p->slice;
    finish_slice(p, run_slice_on_core(p, core));
  }
  pthread_mutex_unlock(&p->kernel);
  t_core = NULL;
  return NULL;
}

Parallel *parallel_create(Scheduler *sched, const CPU_Backend *backend, uint32_t cores) {
  if (!sched || !backend || cores < 1u || g_parallel) return NULL;
  if (cores > PARALLEL_MAX_CORES) cores = PARALLEL_MAX_CORES;
  Parallel *p = (Parallel *)calloc(1, sizeof(*p)); /* once, when the mode is switched on */
  if (!p) return NULL;
  p->sched = sched;
  p->backend = backend;
  pthread_mutex_init(&p->kernel, NULL);
  pthread_cond_init(&p->core_cv, NULL);
  pthread_mutex_init(&p->channel, NULL);
  pthread_cond_init(&p->channel_cv, NULL);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, PARALLEL_CORE_STACK_BYTES);
  for (uint32_t i = 0; i < cores; i++) {
    Core *core = &p->cores[i];
    core->parallel = p;
    core->index = i;
    core->running = -1;
    if (pthread_create(&core->thread, &attr, core_main, core) != 0) break;
    p->core_count++;
  }
  pthread_attr_destroy(&attr);
  if (p->core_count == 0) {
    parallel_destroy(p);
    return NULL;
  }
  sched->parallel = p;
  g_parallel = p;
  log_info("[parallel] %u guest core thread(s)", p->core_count);
  return p;
}

void parallel_destroy(Parallel *p) {
  if (!p) return;
  pthread_mutex_lock(&p->kernel);
  p->quit = true;
  pthread_cond_broadcast(&p->core_cv);
  pthread_mutex_unlock(&p->kernel);
  for (uint32_t i = 0; i < p->core_count; i++) pthread_join(p->cores[i].thread, NULL);
  if (p->sched && p->sched->parallel == p) p->sched->parallel = NULL;
  if (g_parallel == p) g_parallel = NULL;
  pthread_cond_destroy(&p->channel_cv);
  pthread_mutex_destroy(&p->channel);
  pthread_cond_destroy(&p->core_cv);
  pthread_mutex_destroy(&p->kernel);
  free(p);
}

#ifdef __EMSCRIPTEN__
/* The driver is Emscripten's main runtime thread (the CPU worker), and
 * every syscall a core makes - a log line's write to stderr - is proxied
 * to it synchronously. Its queue only runs when asked: wait in short
 * steps and drain it between them, or the first log line deadlocks. */
#define PARALLEL_DRIVER_POLL_NS 1000000l
#define NS_PER_SECOND 1000000000l
static void wait_on_channel(Parallel *p) {
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_nsec += PARALLEL_DRIVER_POLL_NS;
  if (until.tv_nsec >= NS_PER_SECOND) {
    until.tv_sec++;
    until.tv_nsec -= NS_PER_SECOND;
  }
  (void)pthread_cond_timedwait(&p->channel_cv, &p->channel, &until);
  pthread_mutex_unlock(&p->channel);
  emscripten_current_thread_process_queued_calls();
  pthread_mutex_lock(&p->channel);
}
#else
static void wait_on_channel(Parallel *p) { pthread_cond_wait(&p->channel_cv, &p->channel); }
#endif

Scheduler_Status parallel_tick(Parallel *p, uint64_t budget) {
  Scheduler *sched = p->sched;
  pthread_mutex_lock(&p->kernel);
  /* The serial scheduler's preamble, unchanged: with nothing runnable no
   * core is woken and time jumps exactly as in serial mode. */
  Scheduler_Status status = SCHEDULER_RAN;
  if (sched->process_crashed) status = SCHEDULER_CRASHED;
  else if (sched->process_exited) status = SCHEDULER_EXITED;
  else {
    scheduler_expire_timeouts(sched);
    if (scheduler_pick(sched) < 0) status = scheduler_idle(sched);
  }
  if (status != SCHEDULER_RAN) {
    pthread_mutex_unlock(&p->kernel);
    return status;
  }
  pthread_mutex_lock(&p->channel);
  p->slice_done = false;
  pthread_mutex_unlock(&p->channel);
  p->budget = budget;
  p->working = p->core_count;
  p->slice_max = 0;
  p->slice_cores = 0;
  p->slice++;
  pthread_cond_broadcast(&p->core_cv);
  pthread_mutex_unlock(&p->kernel);

  /* Wait for the slice, serving host calls meanwhile. */
  pthread_mutex_lock(&p->channel);
  while (!p->slice_done) {
    if (p->call) {
      Host_Call *call = p->call;
      call->fn(call->ctx);
      call->done = true;
      p->call = NULL;
      pthread_cond_broadcast(&p->channel_cv);
      continue;
    }
    wait_on_channel(p);
  }
  pthread_mutex_unlock(&p->channel);

  pthread_mutex_lock(&p->kernel);
  if (sched->process_crashed) status = SCHEDULER_CRASHED;
  else if (sched->process_exited) status = SCHEDULER_EXITED;
  pthread_mutex_unlock(&p->kernel);
  return status;
}

void parallel_kernel_enter(Parallel *p, const CPU_State *state) {
  pthread_mutex_lock(&p->kernel);
  p->saved_current = p->sched->current;
  if (t_core && t_core->parallel == p) {
    p->sched->current = t_core->running;
  } else {
    /* The driver running a thread itself (emulator_run, emulator_step). */
    const Sched_Thread *t = scheduler_thread_by_state(p->sched, state);
    p->sched->current = t ? (int32_t)(t - p->sched->threads) : -1;
  }
}

void parallel_kernel_exit(Parallel *p) {
  p->sched->current = p->saved_current;
  /* The SVC may have woken threads (or made its own runnable again):
   * waiting cores re-check. */
  pthread_cond_broadcast(&p->core_cv);
  pthread_mutex_unlock(&p->kernel);
}

void parallel_on_driver(void (*fn)(void *ctx), void *ctx) {
  Parallel *p = g_parallel;
  if (!p || !t_core || t_core->parallel != p) {
    fn(ctx);
    return;
  }
  Host_Call call = {fn, ctx, false};
  pthread_mutex_lock(&p->channel);
  while (p->call) pthread_cond_wait(&p->channel_cv, &p->channel);
  p->call = &call;
  pthread_cond_broadcast(&p->channel_cv);
  while (!call.done) pthread_cond_wait(&p->channel_cv, &p->channel);
  pthread_mutex_unlock(&p->channel);
}

#endif
