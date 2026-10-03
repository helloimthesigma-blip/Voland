# Bot 1 status: parallel guest threads (branch `local/bot1`)

Owner: bot 1. The design is `docs/PARALLEL.md`.

## Milestones

- [x] **1. Design doc and serial-equivalence tests.**
  - **Mode.** `emulator_set_host_cores(emu, n)`: 0 is serial and stays the
    default for the CLI and tests; `n >= 1` runs guest threads on `n`
    core threads while the caller drives slices.
  - **CLI.** `voland-cli run ... --host-cores N`.
  - **Tests.** `tests/parallel_test.c` (ctest `parallel_test`):
    - One core is bit-identical to serial: same slices, SVCs, virtual time
      and output on threads/condvar/events/atomics at budgets 7, 31, 100,
      997 and 100000.
    - Two and three cores complete every program.
    - Atomics stress: 3 host threads, 20 rounds, no lost update. Disabling
      the CAS makes it fail (checked).
    - Host calls from a core reach the driver.
  - **Same test under Node with the browser's Emscripten flags** (wasm64,
    pthreads): passes.
  - **Silksong to 900k slices (title screen), serial vs one core:**
    identical.
    - Virtual time 456,923,989 ticks, 466,196 SVCs, the same GPU stream
      (32,835 records, 23,056 draws, 1,218 presents), and the same thread
      table (PCs and wait states).
    - The whole 48k-line debug log matches line for line, apart from host
      heap addresses.
    - Cost of one core: +6.4% host instructions (1.666e12 vs 1.566e12),
      from the per-slice handoff to the core thread.
- [ ] 2. Two or three host threads natively: Silksong reaches the title and
  gameplay; measure. *In progress.*
- [ ] 3. Web: on by default (2 cores) in this branch; fps not measured yet.
- [x] 4. Stress test: exclusives on real host threads, see milestone 1
  (`atomics.s`).

## Files touched outside my own

All are small and delimited:

- **`core/hle/hle.c`.** `hle_on_svc` brackets the dispatch with
  `scheduler_kernel_enter/exit`. These are no-ops in serial mode.
- **`core/hle/kernel/svc_thread.c`.**
  - `reclaim_threads` skips threads still `on_core`.
  - SetThreadActivity(Paused) on a thread running on another core blocks
    the caller (`WAIT_OFF_CORE`) until that thread is off its core.
    Silksong's GC does this before GetThreadContext3.
- **`core/cpu/cpu.{h,c}`.** `cpu_set_multicore()` / `cpu_multicore()`.
- **Interpreter.** These only change behaviour when `cpu_multicore()` is
  set, which needs two or more cores; serial results are unchanged.
  - `interp_internal.h`: `exclusive_value[2]` and `exclusive_size` in
    `Interp_State`. This is for the JIT agent: fields were added after
    `exclusive_address`.
  - `interp_load_store.c`: LDXR records what it read. In multicore mode
    STXR is a host CAS against it (`store_exclusive_shared`), and
    LDAR/STLR/LDAXR get fences.
  - `interp_branch_system.c`: DMB/DSB become host fences in multicore
    mode.
  - `interp_predecode.c`: the block cache is per host thread, and the
    flush epoch is atomic.
- **`core/common/log.c`.** On web pthread builds, lines logged off the main
  runtime thread go to `emscripten_errn` instead of a stderr syscall, which
  Emscripten would proxy synchronously to the waiting CPU worker.
- **`core/emulator.{h,c}`.** `emulator_set_host_cores`, the slice path,
  teardown.
- **`core/stubs/wasm_entry.c`.** `emulator_set_host_cores_ffi`; game-file
  reads and guest output go through `parallel_on_driver`.
- **`CMakeLists.txt`.** The exported FFI, plus
  `add_subdirectory(tests/parallel_wasm)` for Emscripten builds (all
  targets are `EXCLUDE_FROM_ALL`).
- **Web.**
  - `platform/web/workers/cpu.worker.ts`: `DEFAULT_HOST_CORES = 2`, and
    the `set-host-cores` message.
  - `platform/web/bindings/{core,protocol}.ts`: the FFI type and the
    message.
  - `platform/web/src/main.ts`: `?cores=N`.
- **`platform/cli/voland_cli.c`.** `--host-cores N`. Snapshots stop the
  cores before `fork()` and restart them in the child.

## Measurements

See the milestone notes. Native two- and three-core numbers are pending.

## Needs from others

- **JIT agent.**
  - `emulator_set_host_cores` refuses a backend with `supports_jit`, so
    it stays serial: the JIT code cache would be shared by host threads.
  - To lift that: make the code cache per host thread (as
    `interp_predecode.c` now does) or safe to share, and route
    LDXR/STXR/LDAXR/STLXR/LDAR/STLR/DMB through the interpreter, or do the
    same CAS and fences when `cpu_multicore()` is set.
  - After the SVC handler returns, the backend must not write guest
    registers: another core may already be writing X0/X1 of a woken
    thread.
- **Bot 4 / coordinator.** `PTHREAD_POOL_SIZE=8` is shared by the pixel
  workers and the guest cores. `emulator_set_host_cores` shrinks the pixel
  workers to fit. A pool of 12 would let both be full-size; that is a §24
  flag change I have not made.
