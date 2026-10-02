/**
 * Fork-join worker pool: see workers.h.
 */
#include "common/workers.h"

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define WORKERS_SERIAL 1
#elif defined(_WIN32)
#define WORKERS_SERIAL 1 /* no pthreads; serial until a Win32 pool is needed */
#else
#define WORKERS_SERIAL 0
#endif

static uint32_t clamp_count(uint32_t count) {
  if (count < 1u) return 1u;
  return count > WORKERS_MAX ? WORKERS_MAX : count;
}

#if WORKERS_SERIAL

uint32_t workers_default_count(void) { return 1u; }

bool workers_start(Workers *w, uint32_t count) {
  (void)count;
  w->impl = NULL;
  w->count = 1u;
  return false;
}

void workers_run(Workers *w, uint32_t count, Workers_Task task, void *user) {
  (void)w;
  (void)count;
  task(user, 0, 1u);
}

void workers_lock(Workers *w) { (void)w; }
void workers_unlock(Workers *w) { (void)w; }

void workers_stop(Workers *w) {
  w->impl = NULL;
  w->count = 1u;
}

#else

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#endif

typedef struct Helper {
  Workers_Impl *pool;
  uint32_t index;
  pthread_t thread;
} Helper;

struct Workers_Impl {
  pthread_mutex_t mutex;  /* guards everything below */
  pthread_cond_t go;      /* a new generation (or quit) */
  pthread_cond_t done;    /* pending reached 0 */
  uint64_t generation;
  uint32_t active;        /* workers taking part in this generation */
  uint32_t pending;       /* helpers of this generation still running */
  bool quit;
  Workers_Task task;
  void *user;
  pthread_mutex_t user_lock; /* workers_lock */
  uint32_t helper_count;
  Helper helpers[WORKERS_MAX];
};

uint32_t workers_default_count(void) {
#ifdef __EMSCRIPTEN__
  const int cores = emscripten_num_logical_cores();
#else
  const long cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
  return clamp_count(cores > 0 ? (uint32_t)cores : 1u);
}

static void *helper_main(void *arg) {
  const Helper *h = (const Helper *)arg;
  Workers_Impl *p = h->pool;
  uint64_t seen = 0;
  pthread_mutex_lock(&p->mutex);
  for (;;) {
    while (p->generation == seen && !p->quit) pthread_cond_wait(&p->go, &p->mutex);
    if (p->quit) break;
    seen = p->generation;
    if (h->index >= p->active) continue;
    const Workers_Task task = p->task;
    void *user = p->user;
    const uint32_t active = p->active;
    pthread_mutex_unlock(&p->mutex);
    task(user, h->index, active);
    pthread_mutex_lock(&p->mutex);
    if (--p->pending == 0) pthread_cond_signal(&p->done);
  }
  pthread_mutex_unlock(&p->mutex);
  return NULL;
}

bool workers_start(Workers *w, uint32_t count) {
  w->impl = NULL;
  w->count = 1u;
  count = clamp_count(count);
  if (count == 1u) return false;
  Workers_Impl *p = (Workers_Impl *)calloc(1, sizeof(*p)); /* once, at start-up */
  if (!p) return false;
  pthread_mutex_init(&p->mutex, NULL);
  pthread_mutex_init(&p->user_lock, NULL);
  pthread_cond_init(&p->go, NULL);
  pthread_cond_init(&p->done, NULL);
  for (uint32_t i = 1; i < count; i++) {
    Helper *h = &p->helpers[p->helper_count];
    h->pool = p;
    h->index = i;
    if (pthread_create(&h->thread, NULL, helper_main, h) != 0) break;
    p->helper_count++;
  }
  w->impl = p;
  w->count = p->helper_count + 1u;
  return w->count > 1u;
}

void workers_run(Workers *w, uint32_t count, Workers_Task task, void *user) {
  Workers_Impl *p = w->impl;
  uint32_t n = count < w->count ? count : w->count;
  if (n < 1u) n = 1u;
  if (!p || n == 1u) {
    task(user, 0, 1u);
    return;
  }
  pthread_mutex_lock(&p->mutex);
  p->task = task;
  p->user = user;
  p->active = n;
  p->pending = n - 1u;
  p->generation++;
  pthread_cond_broadcast(&p->go);
  pthread_mutex_unlock(&p->mutex);
  task(user, 0, n);
  pthread_mutex_lock(&p->mutex);
  while (p->pending != 0) pthread_cond_wait(&p->done, &p->mutex);
  pthread_mutex_unlock(&p->mutex);
}

void workers_lock(Workers *w) {
  if (w->impl) pthread_mutex_lock(&w->impl->user_lock);
}

void workers_unlock(Workers *w) {
  if (w->impl) pthread_mutex_unlock(&w->impl->user_lock);
}

void workers_stop(Workers *w) {
  Workers_Impl *p = w->impl;
  w->impl = NULL;
  w->count = 1u;
  if (!p) return;
  pthread_mutex_lock(&p->mutex);
  p->quit = true;
  pthread_cond_broadcast(&p->go);
  pthread_mutex_unlock(&p->mutex);
  for (uint32_t i = 0; i < p->helper_count; i++) pthread_join(p->helpers[i].thread, NULL);
  pthread_mutex_destroy(&p->mutex);
  pthread_mutex_destroy(&p->user_lock);
  pthread_cond_destroy(&p->go);
  pthread_cond_destroy(&p->done);
  free(p);
}

#endif
