/**
 * Parallel guest threads (docs/PARALLEL.md): an opt-in scheduler mode in
 * which guest threads run on several host threads ("cores") at once.
 *
 * The calling thread (the CPU worker on the web, the CLI's main thread)
 * becomes the driver: per slice it opens the scheduler to the cores,
 * then waits - serving host calls the cores cannot make themselves
 * (parallel_on_driver) - until every core is done with the slice. No
 * guest code runs between slices, so everything outside
 * emulator_run_slice keeps its serial meaning.
 *
 * One kernel lock guards the scheduler and all of HLE: cores take it to
 * pick and account runs, and every SVC runs under it
 * (scheduler_kernel_enter/exit). backend->run itself runs unlocked.
 *
 * With one core the result is bit-identical to serial mode: the core
 * makes exactly the serial scheduler's single run per slice.
 */
#ifndef SWITCH_HLE_KERNEL_PARALLEL_H
#define SWITCH_HLE_KERNEL_PARALLEL_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu/cpu.h"
#include "hle/kernel/scheduler.h"

/* The Switch gives an application three cores; a fourth is allowed for
 * experiments. */
#define PARALLEL_MAX_CORES 4u

/* Host stack of each core thread: SVC handlers run on it. */
#define PARALLEL_CORE_STACK_BYTES (4u * 1024u * 1024u)

/* Whether this build can run cores on host threads at all. */
bool parallel_supported(void);

/* Starts `cores` (1..PARALLEL_MAX_CORES) core threads for `sched` and
 * attaches them (sched->parallel). NULL if threads are unavailable. */
Parallel *parallel_create(Scheduler *sched, const CPU_Backend *backend, uint32_t cores);

/* Joins the core threads and detaches from the scheduler. Must not be
 * called from inside a slice. */
void parallel_destroy(Parallel *p);

uint32_t parallel_core_count(const Parallel *p);

/* What the cores did, for measuring (docs/PARALLEL.md "Measuring"). */
typedef struct Parallel_Stats {
  uint64_t slices;         /* slices that opened (something was runnable) */
  uint64_t cycles;         /* guest cycles, summed over cores */
  uint64_t span;           /* per slice the busiest core's cycles, summed */
  uint64_t shared_slices;  /* slices in which two or more cores ran */
} Parallel_Stats;
Parallel_Stats parallel_stats(const Parallel *p);

/* One slice: every core runs guest threads for up to `budget` cycles.
 * Called by the driver; returns like scheduler_tick (RAN, IDLE, ...). */
Scheduler_Status parallel_tick(Parallel *p, uint64_t budget);

/* scheduler_kernel_enter/exit in parallel mode. */
void parallel_kernel_enter(Parallel *p, const CPU_State *state);
void parallel_kernel_exit(Parallel *p);

/* Runs fn(ctx) on the driver thread and returns when it is done: for host
 * hooks that only work there (the web CPU worker's JS objects). From the
 * driver itself, or with no parallel scheduler active, calls fn directly. */
void parallel_on_driver(void (*fn)(void *ctx), void *ctx);

/* True on a core thread. */
bool parallel_on_core_thread(void);

#endif /* SWITCH_HLE_KERNEL_PARALLEL_H */
