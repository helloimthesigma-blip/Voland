# Bot 1: parallel guest threads

Read `/BOTS.md` first; its rules apply. Worktree `../Voland-bot1`, branch
`local/bot1`, status file `docs/handoff/BOT_1_STATUS.md`.

## Why

All guest threads run as green threads on **one** host thread
(`core/hle/kernel/scheduler.{h,c}`, `emulator_run_slice` in
`core/emulator.c`). Silksong keeps two guest threads busy: the main thread
at 53% of cycles and thread 32 at 36%. Running guest threads on two or
three host threads at once is worth up to about **1.8×**. That multiplies
with the JIT the JIT agent is building.

## The task

An opt-in **parallel scheduler mode**. The existing serial mode stays the
default for `voland-cli` and tests, because it is deterministic and the
golden-hash checks depend on it. The web build turns parallel mode on.

### Suggested design (refine it; write yours in `docs/PARALLEL.md`)

- **Host threads.** N host threads (2–3; the Switch gives applications 3
  cores) each loop:
  1. Take the kernel lock.
  2. Pick a runnable guest thread that no other host thread is running,
     and mark it running.
  3. Release the lock.
  4. `backend->run(state, budget)` **without** the lock.
  5. Retake the lock and handle the exit reason.
- **SVCs and HLE.** The SVC handler callback runs inside `run`. Wrap
  SVC/HLE dispatch in the kernel lock (a "big kernel lock"). The rest of
  `emulator_run_slice` takes the same lock: device updates (`vi_update`,
  `hid_update`, audio, `nvdrv_poll_completions`) and the GPU producer
  (raster3d is reached through nvdrv ioctls).
- **Virtual time** (`scheduler.ticks`) must stay sane when cores run
  concurrently. A per-host-thread local clock, with global time advancing
  by wall progress (for example, the max or mean of the active cores'
  clocks), keeps the game's frame pacing right. Document the choice.
- **Exclusive monitors.**
  - Today LDXR/STXR use a per-thread monitor that is correct only because
    execution is serial (`interp_load_store.c` `exclusive()`, plus the
    grace mechanism in `interpreter.c`).
  - In parallel mode, STXR must be a host compare-and-swap against the
    value LDXR read (standard, ABA-tolerant).
  - Plain stores by other threads must not be lost; the CAS handles that.
- **Interpreter predecode cache.** `g_blocks` in `interp_predecode.c` is a
  single global. Make it per-host-thread, or safe to share.
- **Shared state.** Audit the remaining globals that run on the CPU path:
  `_Thread_local` or locks. Self-modifying code and module loads must
  invalidate the decode caches on every host thread.
- **Web.** Emscripten pthreads already exist (`PTHREAD_POOL_SIZE=8`).
  - The CPU worker's run loop (`platform/web/workers/cpu.worker.ts`, and
    `emulator_run_slice_ffi` in `core/stubs/wasm_entry.c`) must start the
    worker threads.
  - Rule 3 still holds: nothing in HLE blocks or busy-waits a guest.
    Host threads waiting for work may block on a futex/condvar.

### Files

- **You own:** `core/hle/kernel/scheduler.{h,c}`, the new
  `core/hle/kernel/parallel*.{h,c}`, `docs/PARALLEL.md`, and `tests/`
  for these.
- **You may touch, with care:**
  - `core/emulator.{h,c}` (the run loop) and `core/stubs/wasm_entry.c`.
  - `platform/web/workers/cpu.worker.ts`.
  - The interpreter's exclusive-monitor code and its predecode cache. The
    JIT agent works on `core/cpu/backends/jit/` and reads the
    interpreter: keep interpreter changes small and well-separated, and
    describe them in your status file.
- **Do not touch:** `core/gpu/**`, `core/cpu/backends/jit/**`,
  `platform/web/workers/gpu*.ts`.

## Milestones

1. **Design doc plus serial-equivalence tests.** Parallel mode with 1 host
   thread gives bit-identical results to serial mode (same frame hashes in
   the homebrew set and Silksong to 900k slices).
2. **2–3 host threads, natively.** Silksong reaches the title and gameplay
   without crashes or deadlocks. Measure slices per second vs serial.
3. **Web.** Turn it on in the web build and measure fps with the
   `chromium-gpu` command from BOTS.md.
4. **Stress test.** Exclusive/atomic tests with two real host threads
   (counter increments via LDXR/STXR loops must not lose updates).
