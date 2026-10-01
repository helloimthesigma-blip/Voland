/**
 * Guest thread scheduler (§7): every guest thread is a green thread in
 * the one CPU worker. scheduler_tick() picks the highest-priority
 * runnable thread (round-robin within a priority, oldest-run first),
 * runs it for a bounded budget through the CPU backend, and dispatches on
 * the exit reason. Blocking SVCs never block the host: they mark the
 * current thread WAITING/SLEEPING here and return; the thread becomes
 * runnable again when something signals it or its timeout passes.
 *
 * Virtual time (§7 "Virtual time"): a tick counter at the Switch timer
 * frequency (19.2 MHz) advances by the cycles each run consumed, scaled
 * from the nominal 1.02 GHz CPU clock; it backs CNTVCT_EL0, GetSystemTick
 * and every timeout. When nothing is runnable it jumps straight to the
 * earliest wake time. Coupling to the wall clock is the frame-pacing
 * layer's business, not the scheduler's.
 *
 * Sync primitives are the scheduler's wait states (§12: mutexes and
 * condvars are built on ArbitrateLock/Unlock and the process-wide key
 * SVCs), implemented in svc_thread.c.
 */
#ifndef SWITCH_HLE_KERNEL_SCHEDULER_H
#define SWITCH_HLE_KERNEL_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu/cpu.h"
#include "hle/kernel/thread.h"

#define SCHEDULER_MAX_THREADS 128u
#define SCHEDULER_MAX_WAIT_HANDLES 64u /* Horizon's WaitSynchronization limit */
#define SCHEDULER_TIMER_HZ 19200000ull
#define SCHEDULER_CPU_HZ 1020000000ull
#define SCHEDULER_WAIT_FOREVER UINT64_MAX

typedef enum Thread_Run_State {
  THREAD_STATE_FREE = 0,    /* slot unused */
  THREAD_STATE_CREATED,     /* CreateThread done, StartThread not yet */
  THREAD_STATE_RUNNABLE,
  THREAD_STATE_WAITING,     /* on a wait object, maybe with a timeout */
  THREAD_STATE_DEAD,        /* exited; handle still valid, signaled */
} Thread_Run_State;

typedef enum Wait_Kind {
  WAIT_NONE = 0,
  WAIT_SLEEP,          /* SleepThread: timeout only */
  WAIT_SYNCHRONIZATION,/* WaitSynchronization on handles */
  WAIT_ARBITER_LOCK,   /* ArbitrateLock: waiting for a mutex word */
  WAIT_CONDITION,      /* WaitProcessWideKeyAtomic: waiting on a key */
  WAIT_ADDRESS,        /* WaitForAddress */
} Wait_Kind;

typedef struct Sched_Thread {
  Guest_Thread thread;      /* cpu_state, TLS block, priority, core */
  Thread_Run_State state;
  uint32_t handle;          /* its own handle (the mutex owner tag) */
  uint64_t thread_id;
  bool owns_cpu_state;      /* false for the main thread (Emulator owns it) */

  Wait_Kind wait;
  uint64_t wake_at;         /* virtual ticks; SCHEDULER_WAIT_FOREVER = no timeout */
  uint64_t wait_address;    /* mutex word / condvar key / WaitForAddress */
  uint64_t mutex_address;   /* condvar: the mutex to re-acquire */
  uint32_t wait_tag;        /* the value to write into a mutex word on acquire */
  uint32_t wait_handles[SCHEDULER_MAX_WAIT_HANDLES];
  uint32_t wait_handle_count;
  bool cancel_pending;      /* CancelSynchronization before the wait */
  uint64_t wait_sequence;   /* FIFO order among waiters of equal priority */
  uint64_t core_mask;       /* affinity mask (recorded; §7 runs one worker) */
  uint64_t last_run;        /* round-robin stamp */
} Sched_Thread;

typedef struct Scheduler {
  Sched_Thread threads[SCHEDULER_MAX_THREADS];
  const CPU_Backend *backend; /* set by scheduler_init's caller; reaches woken threads' registers */
  uint64_t ticks;           /* virtual time */
  /* The earliest time a device (vsync) will signal something, or
   * SCHEDULER_WAIT_FOREVER. Set by the Emulator before each tick: when
   * every thread waits, time jumps here instead of reporting deadlock. */
  uint64_t device_wake_at;
  uint64_t cycle_remainder; /* sub-tick cycles carried between runs */
  uint64_t run_counter;
  uint64_t wait_counter;
  uint64_t next_thread_id;
  int32_t current;          /* index of the running thread, -1 outside run */
  bool process_exited;
  bool process_crashed;     /* svcBreak or an unhandled fault */
  uint64_t crash_pc;
  uint64_t crash_address;
} Scheduler;

typedef enum Scheduler_Status {
  SCHEDULER_RAN,       /* a thread ran (exit reason in *reason) */
  SCHEDULER_IDLE,      /* nothing runnable; time advanced to the next wake */
  SCHEDULER_DEADLOCK,  /* nothing runnable and nothing will ever wake */
  SCHEDULER_EXITED,    /* ExitProcess, or every thread exited */
  SCHEDULER_CRASHED,   /* svcBreak or a fault the HLE could not resolve */
} Scheduler_Status;

void scheduler_init(Scheduler *sched, const CPU_Backend *backend);

/* Allocates a slot for a thread. NULL when full. The caller fills
 * `thread`, `handle` and sets the state. */
Sched_Thread *scheduler_new_thread(Scheduler *sched);
void scheduler_free_thread(Scheduler *sched, Sched_Thread *thread);

Sched_Thread *scheduler_current(Scheduler *sched);
Sched_Thread *scheduler_thread_by_state(Scheduler *sched, const CPU_State *state);

/* Runs one slice. `backend` drives the chosen thread for `budget` cycles. */
Scheduler_Status scheduler_tick(Scheduler *sched, const CPU_Backend *backend, uint64_t budget,
                                CPU_ExitReason *reason);

/* Blocks the current thread (called from inside an SVC handler). The
 * thread's X0 result is written when it wakes (scheduler_wake). */
void scheduler_block(Scheduler *sched, Sched_Thread *thread, Wait_Kind kind, uint64_t timeout_ns);

/* Wakes `thread` with `result` in W0 (and leaves the rest of its
 * registers to the caller, who sets e.g. the WaitSynchronization index). */
void scheduler_wake(Scheduler *sched, Sched_Thread *thread, uint32_t result);

/* Marks a thread dead and wakes every WaitSynchronization on it. */
void scheduler_exit_thread(Scheduler *sched, Sched_Thread *thread, const CPU_Backend *backend);

uint64_t scheduler_ns_to_ticks(uint64_t ns);

#endif /* SWITCH_HLE_KERNEL_SCHEDULER_H */
