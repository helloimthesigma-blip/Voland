#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
/**
 * The GPU thread: see gpu_thread.h.
 */
#include "gpu/gpu_thread.h"

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define GPU_THREAD_SERIAL 1
#elif defined(_WIN32)
#define GPU_THREAD_SERIAL 1
#else
#define GPU_THREAD_SERIAL 0
#endif

#if GPU_THREAD_SERIAL

bool gpu_thread_start(Gpu_Thread *t) {
  t->impl = NULL;
  return false;
}
void gpu_thread_stop(Gpu_Thread *t) { t->impl = NULL; }
bool gpu_thread_async(const Gpu_Thread *t) { (void)t; return false; }
void gpu_thread_call(Gpu_Thread *t, Gpu_Thread_Fn fn, void *user, const void *payload, uint32_t bytes) {
  (void)t;
  fn(user, payload, bytes);
}
void gpu_thread_drain(Gpu_Thread *t) { (void)t; }
bool gpu_thread_busy(const Gpu_Thread *t) { (void)t; return false; }
uint32_t gpu_thread_progress(const Gpu_Thread *t) { (void)t; return 0; }
void gpu_thread_wait(Gpu_Thread *t, uint32_t seen, uint64_t timeout_ns) {
  (void)t;
  (void)seen;
  (void)timeout_ns;
}
void gpu_thread_lock(Gpu_Thread *t) { (void)t; }
void gpu_thread_unlock(Gpu_Thread *t) { (void)t; }

#else

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Several frames of submissions (a fight frame queues ~60 KB of GPFIFO
 * entries and display calls). */
#define QUEUE_BYTES (8u * 1024u * 1024u)
#define RECORD_ALIGN 16u
#define NS_PER_SECOND 1000000000ull

typedef struct Record {
  Gpu_Thread_Fn fn;
  void *user;
  uint32_t bytes; /* payload bytes */
  uint32_t size;  /* the record's bytes in the ring, header and padding included; 0 = wrap to the start */
} Record;

#define RECORD_HEADER ((uint32_t)((sizeof(Record) + RECORD_ALIGN - 1u) & ~(RECORD_ALIGN - 1u)))

struct Gpu_Thread_Impl {
  pthread_t thread;
  pthread_mutex_t mutex;   /* guards the indices and flags below */
  pthread_cond_t work;     /* a record was queued (or quit) */
  pthread_cond_t progress; /* a record finished */
  pthread_mutex_t state;   /* gpu_thread_lock */
  uint64_t head, tail;     /* byte positions: tail = next write, head = next read */
  uint32_t finished;       /* records run */
  bool running;            /* a record is being run */
  bool quit;
  uint8_t ring[QUEUE_BYTES + RECORD_HEADER]; /* the slack holds a wrap marker at the very end */
};

static uint32_t record_size(uint32_t bytes) {
  return RECORD_HEADER + ((bytes + RECORD_ALIGN - 1u) & ~(RECORD_ALIGN - 1u));
}

static void *thread_main(void *arg) {
  Gpu_Thread_Impl *p = (Gpu_Thread_Impl *)arg;
  pthread_mutex_lock(&p->mutex);
  for (;;) {
    while (p->head == p->tail && !p->quit) pthread_cond_wait(&p->work, &p->mutex);
    if (p->head == p->tail) break; /* quit, and drained */
    Record r;
    memcpy(&r, p->ring + p->head % QUEUE_BYTES, sizeof(r));
    if (r.size == 0) { /* wrap marker */
      p->head += QUEUE_BYTES - p->head % QUEUE_BYTES;
      continue;
    }
    p->running = true;
    const uint8_t *payload = p->ring + p->head % QUEUE_BYTES + RECORD_HEADER;
    pthread_mutex_unlock(&p->mutex);
    /* The record stays in place (the producer never overwrites unread
     * bytes), so the payload is read where it lies. */
    pthread_mutex_lock(&p->state);
    r.fn(r.user, payload, r.bytes);
    pthread_mutex_unlock(&p->state);
    pthread_mutex_lock(&p->mutex);
    p->head += r.size;
    p->running = false;
    p->finished++;
    pthread_cond_broadcast(&p->progress);
  }
  pthread_mutex_unlock(&p->mutex);
  return NULL;
}

bool gpu_thread_start(Gpu_Thread *t) {
  if (t->impl) return true;
  Gpu_Thread_Impl *p = (Gpu_Thread_Impl *)calloc(1, sizeof(*p)); /* once, at mode change */
  if (!p) return false;
  pthread_mutex_init(&p->mutex, NULL);
  pthread_mutex_init(&p->state, NULL);
  pthread_cond_init(&p->work, NULL);
  pthread_cond_init(&p->progress, NULL);
  if (pthread_create(&p->thread, NULL, thread_main, p) != 0) {
    pthread_mutex_destroy(&p->mutex);
    pthread_mutex_destroy(&p->state);
    pthread_cond_destroy(&p->work);
    pthread_cond_destroy(&p->progress);
    free(p);
    return false;
  }
  t->impl = p;
  return true;
}

void gpu_thread_stop(Gpu_Thread *t) {
  Gpu_Thread_Impl *p = t->impl;
  if (!p) return;
  pthread_mutex_lock(&p->mutex);
  p->quit = true;
  pthread_cond_signal(&p->work);
  pthread_mutex_unlock(&p->mutex);
  pthread_join(p->thread, NULL);
  pthread_mutex_destroy(&p->mutex);
  pthread_mutex_destroy(&p->state);
  pthread_cond_destroy(&p->work);
  pthread_cond_destroy(&p->progress);
  free(p);
  t->impl = NULL;
}

bool gpu_thread_async(const Gpu_Thread *t) { return t && t->impl != NULL; }

void gpu_thread_call(Gpu_Thread *t, Gpu_Thread_Fn fn, void *user, const void *payload, uint32_t bytes) {
  Gpu_Thread_Impl *p = t ? t->impl : NULL;
  if (!p || bytes > GPU_THREAD_MAX_PAYLOAD) {
    if (p) gpu_thread_drain(t); /* too large to queue: in order, here */
    fn(user, payload, bytes);
    return;
  }
  const uint32_t size = record_size(bytes);
  pthread_mutex_lock(&p->mutex);
  for (;;) {
    const uint64_t at = p->tail % QUEUE_BYTES;
    const uint64_t pad = at + size > QUEUE_BYTES ? QUEUE_BYTES - at : 0u;
    if (p->tail + pad + size - p->head <= QUEUE_BYTES) {
      if (pad) {
        const Record wrap = {NULL, NULL, 0, 0};
        memcpy(p->ring + at, &wrap, sizeof(wrap));
        p->tail += pad;
      }
      break;
    }
    pthread_cond_wait(&p->progress, &p->mutex); /* full: the GPU thread is behind */
  }
  const Record r = {fn, user, bytes, size};
  uint8_t *dst = p->ring + p->tail % QUEUE_BYTES;
  memcpy(dst, &r, sizeof(r));
  if (bytes) memcpy(dst + RECORD_HEADER, payload, bytes);
  p->tail += size;
  pthread_cond_signal(&p->work);
  pthread_mutex_unlock(&p->mutex);
}

void gpu_thread_drain(Gpu_Thread *t) {
  Gpu_Thread_Impl *p = t ? t->impl : NULL;
  if (!p) return;
  pthread_mutex_lock(&p->mutex);
  while (p->head != p->tail || p->running) pthread_cond_wait(&p->progress, &p->mutex);
  pthread_mutex_unlock(&p->mutex);
}

bool gpu_thread_busy(const Gpu_Thread *t) {
  Gpu_Thread_Impl *p = t ? t->impl : NULL;
  if (!p) return false;
  pthread_mutex_lock(&p->mutex);
  const bool busy = p->head != p->tail || p->running;
  pthread_mutex_unlock(&p->mutex);
  return busy;
}

uint32_t gpu_thread_progress(const Gpu_Thread *t) {
  Gpu_Thread_Impl *p = t ? t->impl : NULL;
  if (!p) return 0;
  pthread_mutex_lock(&p->mutex);
  const uint32_t finished = p->finished;
  pthread_mutex_unlock(&p->mutex);
  return finished;
}

void gpu_thread_wait(Gpu_Thread *t, uint32_t seen, uint64_t timeout_ns) {
  Gpu_Thread_Impl *p = t ? t->impl : NULL;
  if (!p) return;
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  const uint64_t ns = (uint64_t)deadline.tv_nsec + timeout_ns;
  deadline.tv_sec += (time_t)(ns / NS_PER_SECOND);
  deadline.tv_nsec = (long)(ns % NS_PER_SECOND);
  pthread_mutex_lock(&p->mutex);
  while (p->finished == seen && (p->head != p->tail || p->running))
    if (pthread_cond_timedwait(&p->progress, &p->mutex, &deadline) != 0) break;
  pthread_mutex_unlock(&p->mutex);
}

void gpu_thread_lock(Gpu_Thread *t) {
  if (t && t->impl) pthread_mutex_lock(&t->impl->state);
}

void gpu_thread_unlock(Gpu_Thread *t) {
  if (t && t->impl) pthread_mutex_unlock(&t->impl->state);
}

#endif
