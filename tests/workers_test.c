/**
 * common/workers: every index runs exactly once per run, the caller is
 * index 0, runs with fewer workers than the pool skip the rest, the lock
 * serialises a shared counter, many back-to-back runs (the renderer's
 * pattern: one per draw) neither lose nor repeat work, and a stopped or
 * serial pool runs everything on the caller.
 */
#define CHECK_NAME "workers_test"
#include "check.h"

#include "common/workers.h"

#include <string.h>

#define SLOTS 64u
#define RUNS 2000u
#define LOCKED_ADDS 1000u

typedef struct Job {
  Workers *pool;
  uint32_t hits[WORKERS_MAX];
  uint32_t seen_count[WORKERS_MAX];
  uint32_t shared;            /* incremented under workers_lock */
  uint64_t sums[WORKERS_MAX]; /* per-index partial sums over SLOTS */
} Job;

static void task(void *user, uint32_t index, uint32_t count) {
  Job *job = (Job *)user;
  job->hits[index]++;
  job->seen_count[index] = count;
  uint64_t sum = 0;
  for (uint32_t i = index; i < SLOTS; i += count) sum += i;
  job->sums[index] = sum;
}

static void locked_task(void *user, uint32_t index, uint32_t count) {
  (void)index;
  (void)count;
  Job *job = (Job *)user;
  for (uint32_t i = 0; i < LOCKED_ADDS; i++) {
    workers_lock(job->pool);
    job->shared++;
    workers_unlock(job->pool);
  }
}

/* workers_take: every item claimed exactly once across workers. */
#define ITEMS 5000u
typedef struct Take_Job {
  Workers *pool;
  uint32_t next;
  uint8_t claimed[ITEMS];
} Take_Job;

static void take_task(void *user, uint32_t index, uint32_t count) {
  (void)index;
  (void)count;
  Take_Job *job = (Take_Job *)user;
  for (;;) {
    const uint32_t item = workers_take(job->pool, &job->next);
    if (item >= ITEMS) break;
    job->claimed[item]++;
  }
}

static uint64_t total(const Job *job, uint32_t n) {
  uint64_t t = 0;
  for (uint32_t i = 0; i < n; i++) t += job->sums[i];
  return t;
}

int main(void) {
  const uint64_t expect = (uint64_t)SLOTS * (SLOTS - 1u) / 2u;
  Workers pool;
  workers_start(&pool, 4u);
  CHECK(pool.count >= 1u && pool.count <= 4u);
  const uint32_t n = pool.count;

  static Job job;
  memset(&job, 0, sizeof(job));
  job.pool = &pool;
  for (uint32_t r = 0; r < RUNS; r++) {
    workers_run(&pool, n, task, &job);
    CHECK(total(&job, n) == expect);
  }
  for (uint32_t i = 0; i < n; i++) CHECK(job.hits[i] == RUNS && job.seen_count[i] == n);

  /* Fewer workers than the pool: only indices below 2 run. */
  memset(&job, 0, sizeof(job));
  job.pool = &pool;
  workers_run(&pool, 2u, task, &job);
  const uint32_t two = n < 2u ? n : 2u;
  CHECK(total(&job, two) == expect);
  for (uint32_t i = two; i < WORKERS_MAX; i++) CHECK(job.hits[i] == 0);

  /* The lock makes concurrent increments exact. */
  memset(&job, 0, sizeof(job));
  job.pool = &pool;
  workers_run(&pool, n, locked_task, &job);
  CHECK(job.shared == n * LOCKED_ADDS);

  static Take_Job take;
  memset(&take, 0, sizeof(take));
  take.pool = &pool;
  workers_run(&pool, n, take_task, &take);
  for (uint32_t i = 0; i < ITEMS; i++) CHECK(take.claimed[i] == 1u);

  /* Stopped: serial, index 0 on the caller. */
  workers_stop(&pool);
  CHECK(pool.count == 1u && pool.impl == NULL);
  memset(&job, 0, sizeof(job));
  workers_run(&pool, 4u, task, &job);
  CHECK(job.hits[0] == 1u && job.seen_count[0] == 1u && total(&job, 1u) == expect);

  /* A pool of one is serial from the start. */
  Workers one;
  CHECK(!workers_start(&one, 1u));
  CHECK(one.count == 1u);
  workers_stop(&one);
  CHECK(workers_default_count() >= 1u && workers_default_count() <= WORKERS_MAX);
  printf("[workers_test] passed (%u workers)\n", n);
  return 0;
}
