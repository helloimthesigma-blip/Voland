/**
 * The GPU thread (docs/ASYNC_GPU.md): GPU command processing off the
 * guest's critical path.
 *
 * On real hardware a GPFIFO submission returns as soon as the entries are
 * queued; the GPU runs them later and signals syncpoints as it goes.
 * Synchronously, every submission's draws, compute and texture work ran
 * inside the submitting guest thread's ioctl - about 40% of the CPU
 * worker in an SSBU fight - and every guest thread waited for it.
 *
 * Work for the renderer (submissions, display presents, frame ends,
 * engine writes) goes through gpu_thread_call. In asynchronous mode the
 * call copies its payload into a FIFO and returns; one host thread runs
 * the calls in order. In synchronous mode (the default: deterministic, so
 * tests and frame hashes hold) the call runs at once on the caller.
 *
 * Waiting for the queue to have room (gpu_thread_call) or to drain
 * (gpu_thread_drain) is host-side flow control, like the GPU stream ring's
 * (gpu_stream.h): no guest thread is marked waiting, and the queue holds
 * several frames of work, so a full queue is a host that cannot keep up.
 */
#ifndef SWITCH_GPU_GPU_THREAD_H
#define SWITCH_GPU_GPU_THREAD_H

#include <stdbool.h>
#include <stdint.h>

/* Runs on the GPU thread (or the caller, synchronously) with a copy of
 * the payload passed to gpu_thread_call. */
typedef void (*Gpu_Thread_Fn)(void *user, const void *payload, uint32_t bytes);

typedef struct Gpu_Thread_Impl Gpu_Thread_Impl;

typedef struct Gpu_Thread {
  Gpu_Thread_Impl *impl; /* NULL: synchronous */
} Gpu_Thread;
/* Every function below takes NULL as a synchronous thread. */

/* The largest payload one call carries. */
#define GPU_THREAD_MAX_PAYLOAD (64u * 1024u)

/* Starts the thread: asynchronous mode. False (and synchronous) when
 * threads are unavailable. Safe to call when already started. */
bool gpu_thread_start(Gpu_Thread *t);
/* Drains the queue, then joins the thread: synchronous mode. */
void gpu_thread_stop(Gpu_Thread *t);
bool gpu_thread_async(const Gpu_Thread *t);

/* fn(user, copy of payload, bytes): queued (asynchronous) or now. */
void gpu_thread_call(Gpu_Thread *t, Gpu_Thread_Fn fn, void *user, const void *payload, uint32_t bytes);

/* Returns once every call made before it has finished. */
void gpu_thread_drain(Gpu_Thread *t);
/* Calls queued or running. */
bool gpu_thread_busy(const Gpu_Thread *t);
/* Counts finished calls; gpu_thread_wait waits (up to `timeout_ns` of
 * host time) for it to move past `seen` or for the queue to empty. */
uint32_t gpu_thread_progress(const Gpu_Thread *t);
void gpu_thread_wait(Gpu_Thread *t, uint32_t seen, uint64_t timeout_ns);
/* Calls queued so far (gpu_thread_progress counts the finished ones);
 * gpu_thread_wait_until returns once `calls` of them have finished. */
uint32_t gpu_thread_queued(const Gpu_Thread *t);
void gpu_thread_wait_until(Gpu_Thread *t, uint32_t calls);

/* Guards state the GPU thread reads while the guest's threads change it
 * (the GPU page table). The GPU thread holds it while a call runs. No-ops
 * in synchronous mode. */
void gpu_thread_lock(Gpu_Thread *t);
void gpu_thread_unlock(Gpu_Thread *t);

#endif /* SWITCH_GPU_GPU_THREAD_H */
