# Parallel guest threads

Status: opt-in scheduler mode, the default in the web build (three cores
where the machine has room).
The owner is bot 1 (`docs/handoff/BOT_1.md`). This document is the design;
measurements live in `docs/handoff/BOT_1_STATUS.md`.

## Why

Every guest thread used to run as a green thread on one host thread
(DESIGN.md §7). Silksong keeps two guest threads busy, the main thread
(53% of cycles to the title) and thread 32 (36%). Running them on two
host threads at once is worth up to about 1.8×, and the gain multiplies
with the JIT.

## Shape: a driver and N cores

`emulator_set_host_cores(emu, n)`:

- `n = 0`, the default for the CLI and tests, keeps the serial scheduler
  exactly as it was.
- `n >= 1` starts `n` **core threads**. The calling thread becomes the
  **driver**: the CPU worker on the web, the CLI's main thread natively.

Each `emulator_run_slice` is one **slice**:

1. The driver takes the kernel lock and runs the device updates (`vi`,
   `hid`, audio, `nvdrv_poll_completions`), as in serial mode.
2. The driver runs the serial scheduler's preamble: exited/crashed
   checks, `expire_timeouts`, and whether anything is runnable. If nothing
   is, `scheduler_idle` jumps time exactly as in serial mode and the slice
   ends without waking any core.
3. Otherwise the driver opens the slice (a generation counter plus a
   condvar broadcast) and waits.
4. Each core loops, under the lock:
   1. Pick the best runnable thread that is not `on_core`.
   2. Mark it, release the lock, and call `backend->run(state, budget)`
      unlocked.
   3. Retake the lock and account the run (`scheduler_finish_run`).

   A core keeps picking only while **another core is still running** and
   its own cycles in this slice are below the budget. With nothing to
   pick it waits on the condvar for a run to end or a thread to wake.
5. The slice ends when every core has stopped. The driver returns.

Guest code runs only inside `emulator_run_slice`. Between slices, every
API (loading, snapshots, input, frame dumps, `emulator_step`) sees a
quiescent machine, just as in serial mode.

### One core equals serial

With one core there is never "another core still running", so the core
makes exactly one run per slice: the serial scheduler's pick, budget,
CNTVCT and time accounting. `tests/parallel_test.c` checks this on every
guest program at five budgets (slices, SVCs, virtual time and output
identical). Silksong to 900k slices gives the same virtual time, SVC count
and thread table (see the status file).

From two cores on, the run is no longer deterministic.

### Why the driver does not run guest code itself

On the web, two host hooks only work on the CPU worker's own thread:

- reading the game file (`FileReaderSync` over the `File`);
- guest debug output (`Module.volandGuestOutput`).

A core inside an SVC that needs one calls `parallel_on_driver(fn, ctx)`.
The request goes through a small channel (its own mutex and condvar, so it
never needs the kernel lock, which the requesting core holds). The driver
serves it while it waits for the slice. A driver busy running guest code
could not serve it promptly.

Natively the hooks are thread-safe, and the channel is exercised only by
the test.

## The kernel lock

One mutex guards the scheduler and all of HLE:

- **Cores** hold it to pick and to account runs.
- **Every SVC** runs under it: `hle_on_svc` brackets the dispatch with
  `scheduler_kernel_enter` / `scheduler_kernel_exit`, which are no-ops in
  serial mode.
- **`enter`** points `sched->current` at the calling core's thread, so
  `scheduler_current()` keeps meaning "the thread making this SVC" for
  all of HLE unchanged.
- **`exit`** broadcasts to the cores, because the SVC may have woken
  threads.

This covers the GPU producer (raster3d is reached through nvdrv ioctls),
IPC and services, and the vmm mapping SVCs. Device updates run in the
driver before the slice opens, when no core is running.

### Threads leaving a core

- **`on_core`.** `Sched_Thread.on_core` marks a thread inside `run()` on
  some core. `scheduler_pick` skips it.
  - A thread that blocks in an SVC is WAITING while its core finishes
    `run()`.
  - Another core may wake it meanwhile (the wake writes X0/X1 of a
    register file no one is writing), but cannot pick it until it is off
    its core.
  - Backends must not write guest registers after the SVC handler
    returns. The interpreter doesn't.
- **Reclaiming exited threads** (`reclaim_threads` in `svc_thread.c`)
  skips `on_core` threads: an exited thread's CPU state may still be
  leaving `run()`.
- **SetThreadActivity(Paused)** on a thread running on another core
  blocks the caller (`WAIT_OFF_CORE`) until that thread leaves its core,
  as Horizon does.
  - Silksong's GC pauses threads this way and then reads them with
    GetThreadContext3 (64 and 14 calls to the title). Without the wait it
    would read a register file that is still changing.
  - The paused thread finishes at most its current run (one budget).
  - In serial mode a thread making an SVC is never `on_core` elsewhere,
    so this never blocks there.

### Guest synchronization words

The kernel lock serializes HLE, not guest code. A mutex word, condvar key
or address-arbiter counter is also changed by guest LDXR/STXR on other
cores while an SVC handles it. Two rules in `svc_thread.c` follow:

- **Every kernel update of such a word is an atomic compare-and-swap**
  (`vmm_compare_exchange32`), never a read and then a write. Example: a
  SignalProcessWideKey that read "owned" and then wrote "owned + waiters"
  could overwrite the owner's own release made in between, leaving a
  mutex owned by nobody's knowledge (a deadlock).
- **WaitProcessWideKeyAtomic marks the key before it releases the
  mutex.** Signallers (libnx, nn::os) take the mutex, then skip the SVC
  while the key reads 0. Released first, a signaller on another core can
  slip in between and lose the wakeup.
  - A two-core gameplay run once stalled with the main thread asleep on a
    condvar. This ordering and the read-then-write above are the races
    that can cause that.
  - The race window is a few host instructions, too narrow for the stress
    test to hit reliably. The test (condvar.s with key-checking
    signallers, 20 rounds on 2 and 3 cores) guards the path. The evidence
    is repeated Silksong runs (status file).

Each swap is seq_cst, so the kernel's updates are also ordered as other
cores see them. With one core every swap succeeds first time, exactly
like the plain write it replaced.

## Virtual time

`scheduler.ticks` stays the one global clock (19.2 MHz, backing CNTVCT,
GetSystemTick and every timeout).

- **At the start of a run** the run starts at the current time; that
  value goes to CNTVCT.
- **At the end of a run**, time becomes `max(time, start + cycles)`.
  Concurrent runs therefore overlap in virtual time instead of adding up:
  two cores each running 200k cycles advance the clock by 200k cycles'
  worth, not 400k.
  - When no other core moved time during the run (always so with one
    core), the serial update is used verbatim, including the carried
    sub-tick remainder. That is what keeps one core bit-identical.
- **SVC handlers** see the global time, as in serial mode, where they see
  the time at the start of the run.

Consequence for pacing: per virtual second, the game now gets one virtual
CPU per core instead of one virtual CPU shared by all threads. A title
bound by two busy threads runs its frames in less virtual time, and
virtual time advances with the slowest-progressing host core rather than
the sum of all of them. Vsync stays 60 Hz of virtual time.

## Memory model

`cpu_set_multicore(true)` is set when two or more cores are on. With one
core nothing changes.

- **Store-exclusive is a host compare-and-swap** against the bytes the
  load-exclusive read (`Interp_State.exclusive_value`). It succeeds only
  if the monitor is set on the same address and size and memory still
  holds those bytes.
  - A store by another host thread in between makes it fail, never
    lost.
  - ABA-tolerant, like every CAS-based monitor (Arm allows spurious
    passes only in theory, and the guest's retry loops don't care).
  - 1, 2, 4 and 8 bytes use `__atomic_compare_exchange_n`.
  - The 16-byte pair (LDXP/STXP of two X registers) uses the native
    128-bit CAS on AArch64 hosts.
  - On wasm, which has no 16-byte atomics, the pair is serialized by a
    spinlock. A plain store racing into those 16 bytes can still be lost
    there. It is rare and documented here.
- **Barriers.** DMB and DSB are full host fences (`__atomic_thread_fence`,
  seq_cst). ISB stays a no-op.
- **Acquire/release.** LDAR and STLR get a full fence before and after
  the access (RCsc: an STLR then LDAR pair is not reordered). LDAXR gets
  an acquire fence; STLXR's CAS is already seq_cst.
- **Plain loads and stores** stay plain. Aligned accesses of up to 8 bytes
  are single-copy atomic on both hosts (AArch64, and wasm's aligned
  loads/stores in practice).
- **The exclusive monitor** is still cleared at every `run()` entry, and
  the grace rule (keep running up to 64 instructions past the budget
  while a monitor is held) still applies per core.

## Shared state on the CPU path

| State | Treatment |
|---|---|
| Predecode block cache (`interp_predecode.c`) | Per host thread. The first thread uses the static cache; each other thread allocates its own once (a pthread key frees it at exit). The flush epoch is one shared atomic counter, so `IC IVAU`, `invalidate_cache` and vmm generation changes reach every thread's cache at its next block. |
| vmm page tables, `g_vmm_generation` | Shared, read lock-free. Changed only in SVCs, under the kernel lock. A core mid-block can run the rest of its block on the old mapping; host memory is one linear memory, so a stale mapping never touches anything outside it. |
| `Interp_State` | Per guest thread; only its own core touches it, apart from wakes (above). |
| Call trace (`interp_set_call_trace`) | Debug-only globals, not thread-safe: use with 0 or 1 core. |
| Logging | `fprintf(stderr)`, natively fine. On the web it is a syscall proxied to the main runtime thread, so log lines from other threads go straight to the console with `emscripten_errn` (`log.c`). |
| JIT backend | Multicore-capable (`supports_multicore`); see "The JIT on several cores" below. |

## The JIT on several cores

Compiled functions live in the function table of the host thread that
installed them (every wasm thread has its own table). So:

- **Per-thread caches.** Each host thread has a `Jit_Thread` (`jit.c`):
  cache, hit counters, module buffer and counters.
  - The first thread uses the static instance; others allocate one,
    released (functions removed) when the thread exits.
  - `jit_stats()` sums the threads; one shared counter struct made a cache
    line bounce between cores on every block entry.
- **Identical bytes, shared code.** Compiled code reads its thread's cache
  base from `Jit_State.thread_cache`, set at every `run()` entry, rather
  than baking an address in. So a region compiles to the same module bytes
  on every core, and V8 shares one compiled module process-wide.
  - Without this, three cores exhausted V8's 4 GiB wasm code space in
    Node.
  - Once any core has compiled a region, other cores compile it on first
    sight in multicore mode (`g_compiled_somewhere`) instead of
    interpreting it up to the hot threshold again.
- **One code generation.** `g_vmm_generation` (vmm.h) moves on every mapping
  change and every code flush, and chained regions check it directly.
  Another core's unmap or remap therefore stops chaining at once; a
  per-thread copy of the generation let a core run stale code.
- **One compilation at a time.** `jit_compile_block` keeps its working state
  in statics, so a mutex in `jit.c` serializes it. Each thread has its own
  output buffer.
- **Code compiled in multicore mode** (each entry records the mode it was
  compiled for; a mode change flushes):
  - STXR/STLXR are an inline `i64.atomic.rmw*.cmpxchg` against the LDXR
    value;
  - DMB/DSB are `atomic.fence`;
  - LDAR/STLR are fenced on both sides, LDAXR after.

## Web

- **Default and override.** `cpu.worker.ts` applies `DEFAULT_HOST_CORES` on
  every load: 3 (the Switch's application cores), capped at
  `navigator.hardwareConcurrency - 2` and at least 1.
  - **Under the JIT backend the default is serial (0) for now.** Every
    core compiles its own JIT code cache, and in the browser 3 cores were
    about 2.6× slower than serial with the JIT (title screen, 6.6k vs
    17.3k slices/s). `?cores=N` still forces a count. `?cores=N` on the page URL overrides it through a
  `set-host-cores` message; `?cores=0` is serial.
- **The CPU worker is Emscripten's main runtime thread.** A core's
  syscalls (stderr, anything else not marked `__proxy: none`) are proxied
  to it synchronously, and its proxy queue only runs when asked.
  - While it waits for a slice it therefore waits in 1 ms steps and
    calls `emscripten_current_thread_process_queued_calls()` between
    them.
  - Without this, a core's first log line deadlocks the slice.
- **Threads come from the prepopulated pool** (`PTHREAD_POOL_SIZE=8`, §24).
  `emulator_set_host_cores` shrinks the software renderer's pixel workers
  by as many threads as the cores take, so the total never exceeds the
  pool. A `pthread_create` past the pool would need the CPU worker to
  return to its event loop, which it doesn't while waiting for a slice.
- **Core stacks** are 4 MB (`PARALLEL_CORE_STACK_BYTES`); SVC handlers run
  on them.

## Tests

- `tests/parallel_test.c` (ctest `parallel_test`) checks four things:
  - One core gives exactly the serial results for all guest programs.
  - Two and three cores complete every program.
  - `tests/guest/atomics.s` runs on three host threads, 20 rounds:
    three workers increment shared 32- and 64-bit counters with
    LDAXR/STLXR and LDXR/STXR loops plus DMB, and a lost update fails
    it. With the CAS disabled it fails.
  - Host calls from a core run on the driver.
- `tests/parallel_wasm` builds the same test with the browser's Emscripten
  flags (wasm64, pthreads) and runs it in Node:
  ```
  cmake --build --preset web --target parallel_test_wasm
  node build/web/tests/parallel_wasm/parallel_test.js
  ```
  It also builds `voland-cli` for Node (`parallel-cli-node`) for measuring
  in V8.

## Measuring

On a busy machine, wall time says little: the cores compete with
everything else. `voland-cli --host-cores N` therefore ends with the
scheduler's own account:

```
voland-cli: parallel: S slices, C guest cycles over a span of P (C/P x), F% of slices on 2+ cores
```

- **C** sums every core's guest cycles.
- **P** sums, per slice, the busiest core's cycles: the critical path if
  the host cores were free.
- **C / P** is the parallelism the guest actually offers. It is the
  ceiling on the wall-time speedup (less the per-slice handoff, about 6%
  of host instructions at a 100k-cycle budget with one core).

Snapshot jobs (`--snapshot-at`) take `cores N`, so serial and parallel
runs can start from the same game state:

```
printf 'max_slices 4850000\ncores 2\nlog /tmp/two.log\n' > DIR/job
```

In the browser, `?cores=N` selects the mode, and the homebrew e2e spec
passes `VOLAND_PAGE_QUERY` through to the page.

## Not done / limits

- The JIT backend stays serial until its code cache is per-thread or safe
  to share.
- A paused thread finishes its current run (up to one budget) before
  SetThreadActivity returns. There is no backend hook to stop a run early.
- The pacing change above is deliberate: more guest work per virtual
  second. A title that measures its own CPU time against the tick counter
  sees two cores' worth.
