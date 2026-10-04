/**
 * Thread, synchronization, info and debug SVCs. See svc_thread.h for the
 * register ABI and the semantics chosen.
 */
#include "hle/kernel/svc_thread.h"

#include "common/log.h"
#include "hle/kernel/event.h"
#include "hle/kernel/handle_table.h"
#include "hle/kernel/scheduler.h"
#include "hle/kernel/thread.h"

#include <string.h>

#define CORE_USE_PROCESS_DEFAULT ((int32_t)-2)
#define HLE_APPLICATION_MEMORY_BYTES ((uint64_t)0xCD500000) /* the application pool, ~3.2GB */
#define HLE_ALL_CORES_MASK 0xFull
#define HLE_ALL_PRIORITIES_MASK 0xFFFFFFFFFFFFFFFFull
#define SLEEP_YIELD_LIMIT 0 /* SleepThread(<= 0): 0, -1, -2 are yield variants */
#define RANDOM_ENTROPY_WORDS 4u

static CPU_Register_File *regs(HLE_Context *c, CPU_State *s) { return c->cpu_backend->get_register_file(s); }

static Sched_Thread *current(HLE_Context *c, CPU_State *s) {
  Sched_Thread *t = scheduler_current(c->scheduler);
  return t ? t : scheduler_thread_by_state(c->scheduler, s);
}

static Sched_Thread *thread_from_handle(HLE_Context *c, CPU_State *s, uint32_t handle) {
  if (handle == HANDLE_PSEUDO_CURRENT_THREAD) return current(c, s);
  return (Sched_Thread *)handle_table_get(&c->process->handles, handle, KERNEL_OBJECT_THREAD);
}

static bool is_alive(const Sched_Thread *t) { return t->state != THREAD_STATE_FREE && t->state != THREAD_STATE_DEAD; }

/* ------------------------------------------------------------------ */
/* Process and thread lifecycle.                                       */
/* ------------------------------------------------------------------ */

void hle_svc_exit_process(HLE_Context *c, CPU_State *s) {
  (void)s;
  log_info("[hle] ExitProcess");
  c->scheduler->process_exited = true;
}

/* Frees the slots (CPU state, TLS) of threads whose handle is closed and
 * that can never run again: exited, or created and never started. Titles
 * that create and join short-lived workers would otherwise exhaust the
 * thread table. */
static void reclaim_threads(HLE_Context *c, const Thread_Env *env, const CPU_State *running) {
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    Sched_Thread *t = &c->scheduler->threads[i];
    /* on_core: still leaving run() on another host thread (parallel mode). */
    if (!t->handle_closed || !t->owns_cpu_state || t->thread.cpu_state == running || t->on_core) continue;
    if (t->state != THREAD_STATE_DEAD && t->state != THREAD_STATE_CREATED) continue;
    thread_destroy(env, &t->thread);
    scheduler_free_thread(c->scheduler, t);
  }
}

void hle_svc_create_thread(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint32_t priority = (uint32_t)r->x[4];
  int32_t core = (int32_t)(uint32_t)r->x[5];
  if (priority > THREAD_PRIORITY_LOWEST) { r->x[0] = HLE_RESULT_INVALID_PRIORITY; return; }
  if (core == CORE_USE_PROCESS_DEFAULT) core = c->process->npdm.main_thread_core_number;
  if (core < 0 || (uint32_t)core >= THREAD_CORE_COUNT) { r->x[0] = HLE_RESULT_INVALID_CORE_ID; return; }

  const Thread_Env env = {c->cpu_backend, c->vmm, &c->process->tls, c};
  reclaim_threads(c, &env, s);
  Sched_Thread *t = scheduler_new_thread(c->scheduler);
  if (!t) { r->x[0] = HLE_RESULT_RESOURCE_EXHAUSTED; return; }
  const Thread_Create_Params params = {r->x[1], r->x[2], r->x[3] & ~(uint64_t)(THREAD_STACK_ALIGN - 1u), priority,
                                       (uint32_t)core};
  if (!error_is_ok(thread_create(&env, &params, &t->thread))) {
    scheduler_free_thread(c->scheduler, t);
    r->x[0] = HLE_RESULT_OUT_OF_MEMORY;
    return;
  }
  if (!error_is_ok(handle_table_add(&c->process->handles, KERNEL_OBJECT_THREAD, t, &t->handle))) {
    thread_destroy(&env, &t->thread);
    scheduler_free_thread(c->scheduler, t);
    r->x[0] = HLE_RESULT_OUT_OF_HANDLES;
    return;
  }
  t->owns_cpu_state = true;
  t->core_mask = 1ull << core;
  t->state = THREAD_STATE_CREATED;
  r->x[0] = HLE_RESULT_SUCCESS;
  r->x[1] = t->handle;
}

void hle_svc_start_thread(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  Sched_Thread *t = thread_from_handle(c, s, (uint32_t)r->x[0]);
  if (!t) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  if (t->state != THREAD_STATE_CREATED) { r->x[0] = HLE_RESULT_INVALID_STATE; return; }
  t->state = THREAD_STATE_RUNNABLE;
  r->x[0] = HLE_RESULT_SUCCESS;
}

void hle_svc_exit_thread(HLE_Context *c, CPU_State *s) {
  Sched_Thread *t = current(c, s);
  if (!t) return;
  scheduler_exit_thread(c->scheduler, t, c->cpu_backend);
  bool any_alive = false;
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) any_alive |= is_alive(&c->scheduler->threads[i]);
  if (!any_alive) c->scheduler->process_exited = true;
}

void hle_svc_sleep_thread(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const int64_t ns = (int64_t)r->x[0];
  r->x[0] = HLE_RESULT_SUCCESS;
  Sched_Thread *t = current(c, s);
  if (ns <= SLEEP_YIELD_LIMIT) {
    /* A short run ending in a yield is a poll (scheduler.h). Every other
     * SVC clears the mark (hle_on_svc). */
    if (t) t->spinning = c->cpu_backend->get_cycles_consumed(s) <= SCHEDULER_SPIN_RUN_CYCLES;
    if (t && t->spinning) c->scheduler->polling_yields++;
    /* A yield: the scheduler rotates on the SVC exit. With nothing else
     * to run, spinning only burns virtual time until the next timeout or
     * device event; sleep until then instead (the idle jump follows). */
    uint64_t wake_at = SCHEDULER_WAIT_FOREVER;
    if (t && scheduler_alone(c->scheduler, t, &wake_at) && wake_at != SCHEDULER_WAIT_FOREVER &&
        wake_at > c->scheduler->ticks) {
      scheduler_block(c->scheduler, t, WAIT_SLEEP, SCHEDULER_WAIT_FOREVER);
      t->wake_at = wake_at;
      c->scheduler->lone_yields++;
    }
    return;
  }
  if (t) scheduler_block(c->scheduler, t, WAIT_SLEEP, (uint64_t)ns);
}

void hle_svc_get_thread_priority(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  Sched_Thread *t = thread_from_handle(c, s, (uint32_t)r->x[1]);
  if (!t) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  r->x[0] = HLE_RESULT_SUCCESS;
  r->x[1] = t->thread.priority;
}

void hle_svc_set_thread_priority(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  Sched_Thread *t = thread_from_handle(c, s, (uint32_t)r->x[0]);
  const uint32_t priority = (uint32_t)r->x[1];
  if (!t) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  if (priority > THREAD_PRIORITY_LOWEST) { r->x[0] = HLE_RESULT_INVALID_PRIORITY; return; }
  t->thread.priority = priority;
  r->x[0] = HLE_RESULT_SUCCESS;
}

void hle_svc_get_thread_core_mask(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  Sched_Thread *t = thread_from_handle(c, s, (uint32_t)r->x[2]);
  if (!t) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  r->x[0] = HLE_RESULT_SUCCESS;
  r->x[1] = t->thread.preferred_core;
  r->x[2] = t->core_mask;
}

void hle_svc_set_thread_core_mask(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  Sched_Thread *t = thread_from_handle(c, s, (uint32_t)r->x[0]);
  const int32_t core = (int32_t)(uint32_t)r->x[1];
  if (!t) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  if (core >= 0) {
    if ((uint32_t)core >= THREAD_CORE_COUNT) { r->x[0] = HLE_RESULT_INVALID_CORE_ID; return; }
    t->thread.preferred_core = (uint32_t)core;
  }
  if (r->x[2]) t->core_mask = r->x[2];
  r->x[0] = HLE_RESULT_SUCCESS;
}

void hle_svc_get_current_processor_number(HLE_Context *c, CPU_State *s) {
  Sched_Thread *t = current(c, s);
  regs(c, s)->x[0] = t ? t->thread.preferred_core : 0;
}

/* ------------------------------------------------------------------ */
/* WaitSynchronization / CancelSynchronization.                        */
/* ------------------------------------------------------------------ */

/* Whether a waitable handle is signaled now; false + *valid = false for
 * a handle that cannot be waited on. */
static bool handle_signaled(HLE_Context *c, CPU_State *s, uint32_t handle, bool *valid) {
  *valid = true;
  Kernel_Event *event = (Kernel_Event *)handle_table_get(&c->process->handles, handle, KERNEL_OBJECT_EVENT_READABLE);
  if (event) return event->signaled;
  Sched_Thread *t = thread_from_handle(c, s, handle);
  if (t) return t->state == THREAD_STATE_DEAD;
  *valid = false;
  return false;
}

void hle_signal_event(HLE_Context *c, Kernel_Event *event) {
  if (!event) return;
  event->signaled = true;
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    Sched_Thread *t = &c->scheduler->threads[i];
    if (t->state != THREAD_STATE_WAITING || t->wait != WAIT_SYNCHRONIZATION) continue;
    for (uint32_t h = 0; h < t->wait_handle_count; h++) {
      if (handle_table_get(&c->process->handles, t->wait_handles[h], KERNEL_OBJECT_EVENT_READABLE) == event) {
        scheduler_wake(c->scheduler, t, HLE_RESULT_SUCCESS);
        c->cpu_backend->get_register_file(t->thread.cpu_state)->x[1] = h;
        break;
      }
    }
  }
}

uint32_t hle_create_event(HLE_Context *c, uint32_t *readable, uint32_t *writable, Kernel_Event **out) {
  Kernel_Event *event = event_create(c->events);
  if (!event) return HLE_RESULT_RESOURCE_EXHAUSTED;
  if (!error_is_ok(handle_table_add(&c->process->handles, KERNEL_OBJECT_EVENT_READABLE, event, readable))) {
    event_release(event);
    return HLE_RESULT_OUT_OF_HANDLES;
  }
  if (writable) {
    event_retain(event);
    if (!error_is_ok(handle_table_add(&c->process->handles, KERNEL_OBJECT_EVENT_WRITABLE, event, writable))) {
      (void)handle_table_remove(&c->process->handles, *readable, NULL, NULL);
      event_release(event);
      event_release(event);
      return HLE_RESULT_OUT_OF_HANDLES;
    }
  }
  if (out) *out = event;
  return HLE_RESULT_SUCCESS;
}

void hle_svc_create_event(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  uint32_t readable = 0, writable = 0;
  r->x[0] = hle_create_event(c, &readable, &writable, NULL);
  if (r->x[0] == HLE_RESULT_SUCCESS) {
    r->x[1] = writable;
    r->x[2] = readable;
  }
}

void hle_svc_signal_event(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  Kernel_Event *e = (Kernel_Event *)handle_table_get(&c->process->handles, (uint32_t)r->x[0], KERNEL_OBJECT_EVENT_WRITABLE);
  if (!e) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  hle_signal_event(c, e);
  r->x[0] = HLE_RESULT_SUCCESS;
}

static Kernel_Event *any_event(HLE_Context *c, uint32_t handle) {
  Kernel_Event *e = (Kernel_Event *)handle_table_get(&c->process->handles, handle, KERNEL_OBJECT_EVENT_WRITABLE);
  return e ? e : (Kernel_Event *)handle_table_get(&c->process->handles, handle, KERNEL_OBJECT_EVENT_READABLE);
}

void hle_svc_clear_event(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  Kernel_Event *e = any_event(c, (uint32_t)r->x[0]);
  if (!e) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  e->signaled = false;
  r->x[0] = HLE_RESULT_SUCCESS;
}

void hle_svc_reset_signal(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  Kernel_Event *e = (Kernel_Event *)handle_table_get(&c->process->handles, (uint32_t)r->x[0], KERNEL_OBJECT_EVENT_READABLE);
  if (!e) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  if (!e->signaled) { r->x[0] = HLE_RESULT_INVALID_STATE; return; }
  e->signaled = false;
  r->x[0] = HLE_RESULT_SUCCESS;
}

void hle_svc_wait_synchronization(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint64_t handles_gva = r->x[1];
  const uint32_t count = (uint32_t)r->x[2];
  const int64_t timeout = (int64_t)r->x[3];
  Sched_Thread *self = current(c, s);
  if (count > SCHEDULER_MAX_WAIT_HANDLES) { r->x[0] = HLE_RESULT_OUT_OF_RANGE; return; }
  uint32_t handles[SCHEDULER_MAX_WAIT_HANDLES];
  if (count && !error_is_ok(vmm_read_block(c->vmm, handles_gva, handles, count * sizeof(uint32_t)))) {
    r->x[0] = HLE_RESULT_INVALID_USER_POINTER;
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    bool valid = false;
    const bool signaled = handle_signaled(c, s, handles[i], &valid);
    if (!valid) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
    if (signaled) {
      r->x[0] = HLE_RESULT_SUCCESS;
      r->x[1] = i;
      return;
    }
  }
  if (self && self->cancel_pending) {
    self->cancel_pending = false;
    r->x[0] = HLE_RESULT_CANCELLED;
    return;
  }
  if (timeout == 0 || !self) { r->x[0] = HLE_RESULT_TIMED_OUT; return; }
  memcpy(self->wait_handles, handles, count * sizeof(uint32_t));
  self->wait_handle_count = count;
  scheduler_block(c->scheduler, self, WAIT_SYNCHRONIZATION, timeout < 0 ? SCHEDULER_WAIT_FOREVER : (uint64_t)timeout);
}

void hle_svc_cancel_synchronization(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  Sched_Thread *t = thread_from_handle(c, s, (uint32_t)r->x[0]);
  if (!t) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  if (t->state == THREAD_STATE_WAITING && t->wait == WAIT_SYNCHRONIZATION) {
    scheduler_wake(c->scheduler, t, HLE_RESULT_CANCELLED);
  } else {
    t->cancel_pending = true;
  }
  r->x[0] = HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Mutexes and condition variables.                                     */
/* ------------------------------------------------------------------ */

/* Best waiter of `kind` on `address`: lowest priority value, then FIFO. */
static Sched_Thread *best_waiter(Scheduler *sched, Wait_Kind kind, uint64_t address, uint32_t *total) {
  Sched_Thread *best = NULL;
  uint32_t n = 0;
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
    Sched_Thread *t = &sched->threads[i];
    if (t->state != THREAD_STATE_WAITING || t->wait != kind || t->wait_address != address) continue;
    n++;
    if (!best || t->thread.priority < best->thread.priority ||
        (t->thread.priority == best->thread.priority && t->wait_sequence < best->wait_sequence)) {
      best = t;
    }
  }
  if (total) *total = n;
  return best;
}

/* Guest synchronization words (mutexes, condvar keys, address arbiters)
 * also change under guest LDXR/STXR running on other host threads in
 * parallel mode (docs/PARALLEL.md). The kernel therefore updates them
 * only with atomic compare-and-swaps - never a read and then a write,
 * which could undo a guest update made in between. With one thread at a
 * time every swap succeeds first time, exactly as a plain write. */
static bool cas_word(HLE_Context *c, uint64_t address, uint32_t *expected, uint32_t desired, bool *swapped) {
  return error_is_ok(vmm_compare_exchange32(c->vmm, address, expected, desired, swapped));
}

/* An atomic store of `value` (seq_cst, so ordered with the kernel's other
 * word updates as other cores see them). */
static bool store_word(HLE_Context *c, uint64_t address, uint32_t value) {
  if (address & 3u) return error_is_ok(vmm_write32(c->vmm, address, value));
  uint32_t seen = 0;
  if (!error_is_ok(vmm_read32(c->vmm, address, &seen))) return false;
  bool swapped = false;
  while (!swapped) {
    if (!cas_word(c, address, &seen, value, &swapped)) return false;
  }
  return true;
}

/* Releases the mutex at `address` to its best waiter (or to nobody). */
static uint32_t release_mutex(HLE_Context *c, uint64_t address) {
  uint32_t waiters = 0;
  Sched_Thread *next = best_waiter(c->scheduler, WAIT_ARBITER_LOCK, address, &waiters);
  const uint32_t value = next ? (next->wait_tag | (waiters > 1u ? HLE_MUTEX_HAS_WAITERS : 0u)) : 0u;
  if (!store_word(c, address, value)) return HLE_RESULT_INVALID_MEMORY_STATE;
  if (next) scheduler_wake(c->scheduler, next, HLE_RESULT_SUCCESS);
  return HLE_RESULT_SUCCESS;
}

void hle_svc_arbitrate_lock(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint32_t owner = (uint32_t)r->x[0], tag = (uint32_t)r->x[2];
  const uint64_t address = r->x[1];
  if (address & 3u) { r->x[0] = HLE_RESULT_INVALID_POINTER; return; }
  uint32_t value = 0;
  if (!error_is_ok(vmm_read32(c->vmm, address, &value))) { r->x[0] = HLE_RESULT_INVALID_MEMORY_STATE; return; }
  r->x[0] = HLE_RESULT_SUCCESS;
  if (value != (owner | HLE_MUTEX_HAS_WAITERS)) return; /* the lock changed hands already */
  if (!thread_from_handle(c, s, owner)) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  Sched_Thread *self = current(c, s);
  if (!self) return;
  self->wait_address = address;
  self->wait_tag = tag;
  scheduler_block(c->scheduler, self, WAIT_ARBITER_LOCK, SCHEDULER_WAIT_FOREVER);
}

void hle_svc_arbitrate_unlock(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint64_t address = r->x[0];
  if (address & 3u) { r->x[0] = HLE_RESULT_INVALID_POINTER; return; }
  r->x[0] = release_mutex(c, address);
}

void hle_svc_wait_process_wide_key_atomic(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint64_t mutex = r->x[0], key = r->x[1];
  const uint32_t tag = (uint32_t)r->x[2];
  const int64_t timeout = (int64_t)r->x[3];
  if (mutex & 3u) { r->x[0] = HLE_RESULT_INVALID_POINTER; return; }
  /* "Has waiters" goes into the key before the mutex is released: a
   * signaler on another core takes the mutex, then reads the key to
   * decide whether to call SignalProcessWideKey. In the other order it can
   * read 0 and the wakeup is lost. */
  if (!store_word(c, key, 1u)) { r->x[0] = HLE_RESULT_INVALID_MEMORY_STATE; return; }
  const uint32_t released = release_mutex(c, mutex);
  if (released != HLE_RESULT_SUCCESS) { r->x[0] = released; return; }
  if (timeout == 0) { r->x[0] = HLE_RESULT_TIMED_OUT; return; }
  Sched_Thread *self = current(c, s);
  if (!self) { r->x[0] = HLE_RESULT_TIMED_OUT; return; }
  self->wait_address = key;
  self->mutex_address = mutex;
  self->wait_tag = tag;
  scheduler_block(c->scheduler, self, WAIT_CONDITION, timeout < 0 ? SCHEDULER_WAIT_FOREVER : (uint64_t)timeout);
}

void hle_svc_signal_process_wide_key(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint64_t key = r->x[0];
  const int32_t count = (int32_t)(uint32_t)r->x[1];
  int32_t woken = 0;
  while (count <= 0 || woken < count) {
    Sched_Thread *t = best_waiter(c->scheduler, WAIT_CONDITION, key, NULL);
    if (!t) break;
    woken++;
    /* Re-acquire the mutex for the woken thread, inside the kernel: take
     * it if free, else mark it contended - atomically against its owner
     * releasing it on another core meanwhile. */
    uint32_t value = 0;
    bool swapped = false, ok = error_is_ok(vmm_read32(c->vmm, t->mutex_address, &value));
    while (ok && !swapped) {
      ok = cas_word(c, t->mutex_address, &value, value == 0 ? t->wait_tag : (value | HLE_MUTEX_HAS_WAITERS),
                    &swapped);
    }
    if (!ok) {
      scheduler_wake(c->scheduler, t, HLE_RESULT_INVALID_MEMORY_STATE);
      continue;
    }
    if (value == 0) {
      scheduler_wake(c->scheduler, t, HLE_RESULT_SUCCESS);
      continue;
    }
    t->wait = WAIT_ARBITER_LOCK; /* now waiting for the mutex, forever */
    t->wait_address = t->mutex_address;
    t->wake_at = SCHEDULER_WAIT_FOREVER;
  }
  if (!best_waiter(c->scheduler, WAIT_CONDITION, key, NULL)) (void)store_word(c, key, 0u);
  r->x[0] = HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* WaitForAddress / SignalToAddress.                                    */
/* ------------------------------------------------------------------ */

enum { ARBITRATION_WAIT_IF_LESS_THAN = 0, ARBITRATION_DECREMENT_AND_WAIT_IF_LESS_THAN = 1, ARBITRATION_WAIT_IF_EQUAL = 2 };
enum { SIGNAL_ONLY = 0, SIGNAL_AND_INCREMENT_IF_EQUAL = 1, SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL = 2 };

void hle_svc_wait_for_address(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint64_t address = r->x[0];
  const uint32_t type = (uint32_t)r->x[1];
  const int32_t value = (int32_t)(uint32_t)r->x[2];
  const int64_t timeout = (int64_t)r->x[3];
  if (address & 3u) { r->x[0] = HLE_RESULT_INVALID_POINTER; return; }
  if (type > ARBITRATION_WAIT_IF_EQUAL) { r->x[0] = HLE_RESULT_INVALID_ENUM_VALUE; return; }
  uint32_t raw = 0;
  if (!error_is_ok(vmm_read32(c->vmm, address, &raw))) { r->x[0] = HLE_RESULT_INVALID_MEMORY_STATE; return; }
  for (;;) {
    const int32_t current_value = (int32_t)raw;
    const bool wait = type == ARBITRATION_WAIT_IF_EQUAL ? current_value == value : current_value < value;
    if (!wait) { r->x[0] = HLE_RESULT_INVALID_STATE; return; }
    if (type != ARBITRATION_DECREMENT_AND_WAIT_IF_LESS_THAN) break;
    /* The decrement is atomic: on a lost race, decide again on the new value. */
    bool swapped = false;
    if (!cas_word(c, address, &raw, raw - 1u, &swapped)) { r->x[0] = HLE_RESULT_INVALID_MEMORY_STATE; return; }
    if (swapped) break;
  }
  if (timeout == 0) { r->x[0] = HLE_RESULT_TIMED_OUT; return; }
  Sched_Thread *self = current(c, s);
  if (!self) { r->x[0] = HLE_RESULT_TIMED_OUT; return; }
  self->wait_address = address;
  scheduler_block(c->scheduler, self, WAIT_ADDRESS, timeout < 0 ? SCHEDULER_WAIT_FOREVER : (uint64_t)timeout);
}

void hle_svc_signal_to_address(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint64_t address = r->x[0];
  const uint32_t type = (uint32_t)r->x[1];
  const int32_t value = (int32_t)(uint32_t)r->x[2];
  const int32_t count = (int32_t)(uint32_t)r->x[3];
  if (address & 3u) { r->x[0] = HLE_RESULT_INVALID_POINTER; return; }
  if (type > SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL) { r->x[0] = HLE_RESULT_INVALID_ENUM_VALUE; return; }
  if (type != SIGNAL_ONLY) {
    uint32_t raw = 0;
    if (!error_is_ok(vmm_read32(c->vmm, address, &raw))) { r->x[0] = HLE_RESULT_INVALID_MEMORY_STATE; return; }
    if ((int32_t)raw != value) { r->x[0] = HLE_RESULT_INVALID_STATE; return; }
    int32_t new_value = value + 1;
    if (type == SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL) {
      uint32_t waiters = 0;
      (void)best_waiter(c->scheduler, WAIT_ADDRESS, address, &waiters);
      if (count <= 0) {
        new_value = waiters > 0 ? value - 2 : value + 1;
      } else if (waiters > 0) {
        /* Atmosphère's count of waiters after the first, capped at count + 1. */
        const uint32_t others = waiters - 1u < (uint32_t)count + 1u ? waiters - 1u : (uint32_t)count + 1u;
        new_value = others == 0 ? value + 1 : (others <= (uint32_t)count ? value - 1 : value);
      }
    }
    /* Only if it still holds `value` (another core may have changed it). */
    bool swapped = false;
    if (!cas_word(c, address, &raw, (uint32_t)new_value, &swapped)) {
      r->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
      return;
    }
    if (!swapped) { r->x[0] = HLE_RESULT_INVALID_STATE; return; }
  }
  int32_t woken = 0;
  while (count <= 0 || woken < count) {
    Sched_Thread *t = best_waiter(c->scheduler, WAIT_ADDRESS, address, NULL);
    if (!t) break;
    scheduler_wake(c->scheduler, t, HLE_RESULT_SUCCESS);
    woken++;
  }
  r->x[0] = HLE_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Info and debug.                                                     */
/* ------------------------------------------------------------------ */

void hle_svc_get_system_tick(HLE_Context *c, CPU_State *s) {
  regs(c, s)->x[0] = c->cpu_backend->get_sys_reg(s, CPU_SYSREG_CNTVCT_EL0);
}

void hle_svc_get_process_id(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint32_t handle = (uint32_t)r->x[1];
  if (handle != HANDLE_PSEUDO_CURRENT_PROCESS && !thread_from_handle(c, s, handle)) {
    r->x[0] = HLE_RESULT_INVALID_HANDLE;
    return;
  }
  r->x[0] = HLE_RESULT_SUCCESS;
  r->x[1] = HLE_PROCESS_ID;
}

#define THREAD_ACTIVITY_RUNNABLE 0u
#define THREAD_ACTIVITY_PAUSED 1u
#define THREAD_CONTEXT_BYTES 0x320u
#define THREAD_CONTEXT_GPRS 29u
#define THREAD_CONTEXT_PSR 0x108u
#define THREAD_CONTEXT_VECTORS 0x110u
#define THREAD_CONTEXT_FPCR 0x310u
#define THREAD_CONTEXT_TPIDR 0x318u

void hle_svc_set_thread_activity(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  Sched_Thread *t = thread_from_handle(c, s, (uint32_t)r->x[0]);
  if (!t) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  const uint64_t activity = r->x[1];
  if (activity != THREAD_ACTIVITY_RUNNABLE && activity != THREAD_ACTIVITY_PAUSED) {
    r->x[0] = HLE_RESULT_INVALID_ENUM_VALUE;
    return;
  }
  t->paused = activity == THREAD_ACTIVITY_PAUSED;
  r->x[0] = HLE_RESULT_SUCCESS;
  /* Pausing a thread that runs on another core (parallel mode): like
   * Horizon, return only once it is off that core, so a following
   * GetThreadContext3 (a GC scanning registers) sees it stopped. */
  Sched_Thread *self = current(c, s);
  if (t->paused && t->on_core && self && self != t) {
    scheduler_block(c->scheduler, self, WAIT_OFF_CORE, SCHEDULER_WAIT_FOREVER);
    self->wait_address = (uint64_t)(t - c->scheduler->threads);
  }
}

static void put_u64(uint8_t *p, uint64_t v) { memcpy(p, &v, sizeof(v)); }
static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }

void hle_svc_get_thread_context3(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint64_t out = r->x[0];
  Sched_Thread *t = thread_from_handle(c, s, (uint32_t)r->x[1]);
  if (!t || !t->thread.cpu_state) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  CPU_State *target = t->thread.cpu_state;
  const CPU_Register_File *tr = c->cpu_backend->get_register_file(target);
  uint8_t context[THREAD_CONTEXT_BYTES];
  memset(context, 0, sizeof(context));
  for (uint32_t i = 0; i < THREAD_CONTEXT_GPRS; i++) put_u64(context + 8u * i, tr->x[i]);
  put_u64(context + 8u * 29u, tr->x[29]); /* fp */
  put_u64(context + 8u * 30u, tr->x[30]); /* lr */
  put_u64(context + 8u * 31u, tr->sp);
  put_u64(context + 8u * 32u, tr->pc);
  put_u32(context + THREAD_CONTEXT_PSR, tr->pstate);
  for (uint8_t v = 0; v < 32u; v++) {
    const CPU_Vector_Register reg = c->cpu_backend->get_vector_reg(target, v);
    put_u64(context + THREAD_CONTEXT_VECTORS + 16u * v, reg.lo);
    put_u64(context + THREAD_CONTEXT_VECTORS + 16u * v + 8u, reg.hi);
  }
  put_u32(context + THREAD_CONTEXT_FPCR, (uint32_t)c->cpu_backend->get_sys_reg(target, CPU_SYSREG_FPCR));
  put_u32(context + THREAD_CONTEXT_FPCR + 4u, (uint32_t)c->cpu_backend->get_sys_reg(target, CPU_SYSREG_FPSR));
  put_u64(context + THREAD_CONTEXT_TPIDR, c->cpu_backend->get_sys_reg(target, CPU_SYSREG_TPIDR_EL0));
  r->x[0] = error_is_ok(vmm_write_block(c->vmm, out, context, sizeof(context))) ? HLE_RESULT_SUCCESS
                                                                                 : HLE_RESULT_INVALID_POINTER;
}

void hle_svc_get_thread_id(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  Sched_Thread *t = thread_from_handle(c, s, (uint32_t)r->x[1]);
  if (!t) { r->x[0] = HLE_RESULT_INVALID_HANDLE; return; }
  r->x[0] = HLE_RESULT_SUCCESS;
  r->x[1] = t->thread_id;
}

void hle_svc_break(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint32_t reason = (uint32_t)r->x[0];
  log_error("[hle] svcBreak reason=0x%x info=0x%llx size=0x%llx at pc=0x%010llx", reason,
            (unsigned long long)r->x[1], (unsigned long long)r->x[2], (unsigned long long)r->pc);
  /* libnx's fatal paths pass the failing Result as a 4-byte info block. */
  uint32_t result = 0;
  if (r->x[2] == sizeof(result) && error_is_ok(vmm_read_block(c->vmm, r->x[1], &result, sizeof(result))))
    log_error("[hle] svcBreak info: result 0x%x (module %u, description %u)", result, result & 0x1FFu, result >> 9);
  r->x[0] = HLE_RESULT_SUCCESS;
  if (reason & HLE_BREAK_NOTIFICATION_ONLY) return;
  c->scheduler->process_crashed = true;
  c->scheduler->crash_pc = r->pc;
}

void hle_svc_output_debug_string(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint64_t length = r->x[1] < HLE_DEBUG_STRING_MAX ? r->x[1] : HLE_DEBUG_STRING_MAX;
  char text[HLE_DEBUG_STRING_MAX + 1];
  if (length && !error_is_ok(vmm_read_block(c->vmm, r->x[0], text, length))) {
    r->x[0] = HLE_RESULT_INVALID_USER_POINTER;
    return;
  }
  size_t n = (size_t)length;
  while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\0')) n--;
  text[n] = '\0';
  log_info("[guest] %s", text);
  if (c->debug_output) c->debug_output(c->debug_userdata, text, n);
  r->x[0] = HLE_RESULT_SUCCESS;
}

static uint64_t used_memory(const Process *p) {
  return p->code_bytes_mapped + p->heap_size + p->main_thread_stack.size;
}

static uint64_t entropy(const Process *p, uint64_t subtype) {
  /* splitmix64 over the program id: deterministic per title, distinct words. */
  uint64_t z = p->npdm.program_id + 0x9E3779B97F4A7C15ull * (subtype + 1u);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

void hle_svc_get_info(HLE_Context *c, CPU_State *s) {
  CPU_Register_File *r = regs(c, s);
  const uint32_t type = (uint32_t)r->x[1];
  const uint64_t subtype = r->x[3];
  const Process *p = c->process;
  const Address_Space *as = &p->address_space;
  uint64_t value;
  switch (type) {
  case 0: value = HLE_ALL_CORES_MASK; break;
  case 1: value = HLE_ALL_PRIORITIES_MASK; break;
  case 2: value = as->alias.base; break;
  case 3: value = as->alias.size; break;
  case 4: value = as->heap.base; break;
  case 5: value = as->heap.size; break;
  case 6: case 21: value = HLE_APPLICATION_MEMORY_BYTES; break;
  case 7: case 22: value = used_memory(p); break;
  case 8: value = 0; break;          /* no debugger */
  case 9: value = 0; break;          /* no resource-limit handle */
  case 10: value = 0; break;         /* idle ticks */
  case 11:
    if (subtype >= RANDOM_ENTROPY_WORDS) { r->x[0] = HLE_RESULT_INVALID_COMBINATION; return; }
    value = entropy(p, subtype);
    break;
  case 12: value = as->aslr.base; break;
  case 13: value = as->aslr.size; break;
  case 14: value = as->stack.base; break;
  case 15: value = as->stack.size; break;
  case 16: value = p->npdm.system_resource_size; break;
  case 17: value = 0; break;
  case 18: value = p->npdm.program_id; break;
  case 20: value = 0; break;         /* no user exception context */
  case 23: value = 1; break;         /* IsApplication */
  case 24: value = SCHEDULER_MAX_THREADS; break;
  case 25: case 0xF0000002u: value = c->scheduler->ticks; break;
  case 26: value = 1; break;
  case 27: case 28: value = 0; break; /* IoRegionHint, AliasRegionExtraSize (18.0.0+) */
  default:
    log_warn("[hle] GetInfo: unimplemented info type %u (subtype %llu)", type, (unsigned long long)subtype);
    r->x[0] = HLE_RESULT_INVALID_ENUM_VALUE;
    return;
  }
  r->x[0] = HLE_RESULT_SUCCESS;
  r->x[1] = value;
}

void hle_svc_set_memory_attribute(HLE_Context *c, CPU_State *s) {
  /* Only the uncached attribute exists for user processes; caching is
   * not observable through vmm, so accepting it is exact. */
  regs(c, s)->x[0] = HLE_RESULT_SUCCESS;
}
