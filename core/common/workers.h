/**
 * Fork-join worker pool for data-parallel work inside one emulator step
 * (the reference renderer's pixel shading, §13). The caller is worker 0
 * and takes part; workers_run returns when every index has finished.
 *
 * This is not guest-visible blocking (§7): the pool parallelises one
 * host-side computation the CPU worker would otherwise do alone, and the
 * join is the end of that computation. On the web the helpers are
 * Emscripten pthreads (-pthread, PTHREAD_POOL_SIZE in §24) sharing the one
 * linear memory; where threads are unavailable (no pthreads, MSVC) the
 * pool runs everything serially on the caller - same results, one core.
 *
 * Threads are created once, at workers_start (never on a hot path).
 */
#ifndef SWITCH_COMMON_WORKERS_H
#define SWITCH_COMMON_WORKERS_H

#include <stdbool.h>
#include <stdint.h>

#define WORKERS_MAX 8u /* web: must not exceed PTHREAD_POOL_SIZE (§24) */

/* Runs index `index` of `count` for `user`. */
typedef void (*Workers_Task)(void *user, uint32_t index, uint32_t count);

typedef struct Workers_Impl Workers_Impl;

typedef struct Workers {
  Workers_Impl *impl; /* NULL: serial */
  uint32_t count;     /* workers including the caller; 1 when serial */
} Workers;

/* Host cores worth using (1..WORKERS_MAX). */
uint32_t workers_default_count(void);

/* Starts `count` - 1 helper threads (count is clamped to 1..WORKERS_MAX).
 * On failure, or without thread support, the pool is serial (count 1)
 * and this returns false. */
bool workers_start(Workers *w, uint32_t count);

/* Runs task(user, i, n) for every i in [0, n), n = min(count, w->count):
 * index 0 on the caller, the rest on helpers. Returns when all are done.
 * Not reentrant: one run at a time, from one thread. */
void workers_run(Workers *w, uint32_t count, Workers_Task task, void *user);

/* A mutex for tasks to guard shared state (no-op when serial). */
void workers_lock(Workers *w);
void workers_unlock(Workers *w);

/* Joins the helpers; the pool becomes serial. Safe on a serial pool. */
void workers_stop(Workers *w);

#endif /* SWITCH_COMMON_WORKERS_H */
