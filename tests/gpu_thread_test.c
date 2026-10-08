/**
 * gpu/gpu_thread: calls run once each, in order, with their own payload
 * copies (the caller's buffer is reused at once); the queue wraps many
 * times over with mixed payload sizes and a full queue waits instead of
 * losing calls; drain returns only when everything has run; the state
 * lock keeps the thread between calls; payloads over the limit run in
 * order on the caller; a NULL or stopped thread runs calls at once; stop
 * drains first.
 */
#define CHECK_NAME "gpu_thread_test"
#include "check.h"

#include "gpu/gpu_thread.h"

#include <stdio.h>
#include <string.h>

#define CALLS 20000u
#define BIG_PAYLOAD (48u * 1024u)
#define BIG_EVERY 97u

typedef struct Seen {
  uint32_t next;     /* the sequence number the next call must carry */
  uint32_t bad;      /* calls out of order or with a damaged payload */
  uint32_t in_call;  /* set while a call runs (lock check) */
} Seen;

static uint8_t g_payload[GPU_THREAD_MAX_PAYLOAD + 4096u];

static void fill(uint8_t *p, uint32_t bytes, uint32_t seq) {
  memcpy(p, &seq, 4);
  for (uint32_t i = 4; i < bytes; i++) p[i] = (uint8_t)(seq * 31u + i);
}

static void call(void *user, const void *payload, uint32_t bytes) {
  Seen *seen = (Seen *)user;
  __atomic_store_n(&seen->in_call, 1u, __ATOMIC_SEQ_CST);
  const uint8_t *p = (const uint8_t *)payload;
  uint32_t seq = 0;
  memcpy(&seq, p, 4);
  bool ok = seq == seen->next;
  for (uint32_t i = 4; i < bytes && ok; i++) ok = p[i] == (uint8_t)(seq * 31u + i);
  if (!ok) seen->bad++;
  seen->next = seq + 1u;
  __atomic_store_n(&seen->in_call, 0u, __ATOMIC_SEQ_CST);
}

static uint32_t size_of(uint32_t i) {
  if (i % BIG_EVERY == 0) return BIG_PAYLOAD;
  return 4u + (i * 7919u) % 3000u;
}

static void run_calls(Gpu_Thread *t, Seen *seen, uint32_t from, uint32_t count) {
  for (uint32_t i = from; i < from + count; i++) {
    const uint32_t bytes = size_of(i);
    fill(g_payload, bytes, i);
    gpu_thread_call(t, call, seen, g_payload, bytes);
    memset(g_payload, 0xEE, bytes); /* the queued copy must not see this */
  }
}

int main(void) {
  /* Synchronous: NULL and an unstarted thread run each call at once. */
  Seen seen = {0};
  run_calls(NULL, &seen, 0, 50);
  CHECK(seen.next == 50u && seen.bad == 0u);
  Gpu_Thread t = {0};
  CHECK(!gpu_thread_async(&t));
  run_calls(&t, &seen, 50, 50);
  CHECK(seen.next == 100u && seen.bad == 0u);

  if (!gpu_thread_start(&t)) {
    printf("[gpu_thread_test] passed (no threads: synchronous only)\n");
    return 0;
  }
  CHECK(gpu_thread_async(&t));
  CHECK(gpu_thread_start(&t)); /* already started */

  /* Many calls: the 8 MiB queue wraps many times. */
  run_calls(&t, &seen, 100, CALLS);
  gpu_thread_drain(&t);
  CHECK(!gpu_thread_busy(&t));
  CHECK(seen.next == 100u + CALLS && seen.bad == 0u);
  CHECK(gpu_thread_progress(&t) == CALLS);

  /* The lock: held, the thread does not run a call. */
  gpu_thread_lock(&t);
  run_calls(&t, &seen, 100u + CALLS, 10);
  for (volatile uint32_t spin = 0; spin < 1000000u; spin++) {
  }
  CHECK(seen.next == 100u + CALLS && __atomic_load_n(&seen.in_call, __ATOMIC_SEQ_CST) == 0u);
  CHECK(gpu_thread_busy(&t));
  gpu_thread_unlock(&t);
  const uint32_t before = gpu_thread_progress(&t);
  gpu_thread_wait(&t, before, 1000000000ull);
  gpu_thread_drain(&t);
  CHECK(seen.next == 110u + CALLS && seen.bad == 0u);

  /* Over the payload limit: drained, then run on the caller - in order. */
  run_calls(&t, &seen, 110u + CALLS, 5);
  fill(g_payload, GPU_THREAD_MAX_PAYLOAD + 16u, 115u + CALLS);
  gpu_thread_call(&t, call, &seen, g_payload, GPU_THREAD_MAX_PAYLOAD + 16u);
  CHECK(seen.next == 116u + CALLS && seen.bad == 0u);

  /* Stop drains what is queued, then the thread is synchronous again. */
  run_calls(&t, &seen, 116u + CALLS, 200);
  gpu_thread_stop(&t);
  CHECK(seen.next == 316u + CALLS && seen.bad == 0u);
  CHECK(!gpu_thread_async(&t));
  run_calls(&t, &seen, 316u + CALLS, 3);
  CHECK(seen.next == 319u + CALLS && seen.bad == 0u);
  printf("[gpu_thread_test] passed (%u calls)\n", seen.next);
  return 0;
}
