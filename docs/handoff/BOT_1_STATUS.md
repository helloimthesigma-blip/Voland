# Bot 1 status: parallel guest threads (branch `local/bot1`)

**Summary.** All four milestones are done. Guest threads run on up to 3
host threads in the browser by default: +39% fps at the Silksong title,
and an estimated 1.3–1.9× in gameplay on a machine with free cores. One
core is bit-identical to serial. Next: the JIT agent makes its code cache
per host thread and sets `supports_multicore`, so the two speed-ups
multiply.

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
- [x] **2. Two or three host threads natively.** Silksong reaches the title
  (900k slices on 2 cores) and runs gameplay from a 4.75M-slice snapshot
  on 2, 3 and 4 cores with no crash or deadlock: 12 runs after the
  sync-word fix. Measurements are below.
- [x] **3. Web.** On by default (3 cores, capped at hardwareConcurrency - 2).
  Silksong title screen in Chromium on the real GPU: 5.53 → 7.68 fps
  (+39%). See Measurements.
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
  - `platform/web/workers/cpu.worker.ts`: `DEFAULT_HOST_CORES` (3, capped at hardwareConcurrency - 2), and
    the `set-host-cores` message.
  - `platform/web/bindings/{core,protocol}.ts`: the FFI type and the
    message.
  - `platform/web/src/main.ts`: `?cores=N`.
- **`platform/cli/voland_cli.c`.** `--host-cores N`. Snapshots stop the
  cores before `fork()` and restart them in the child.

## Measurements

The machine runs six agents (load average 20–50 on 8 cores), so wall
time is mostly noise. The numbers below are the scheduler's own account
(`voland-cli: parallel:` line, docs/PARALLEL.md "Measuring") and the
host's instructions-retired counter, which load doesn't change.

| Run | Result |
|---|---|
| Boot, 20k slices, native, 2 cores | Parallelism 1.51× (guest cycles summed over cores / busiest core per slice); 94.8% of slices ran both cores. Virtual time 37.9M vs 24.5M ticks serial (1.55× more progress per slice). ArbitrateLock/Unlock went from 971/1459 to 15223/17301: the guest's mutexes are really contended now. |
| Boot, 20k slices, Node (V8, the browser's wasm), 2 cores | Parallelism 1.52×; 92.5% of slices on both cores. |
| Title, 900k slices, native, 2 cores | No crash or deadlock; 1,350 presents vs 1,218 serial, virtual time 490.9M vs 456.9M. Wall 374 s vs 353 s, but the process got only about 0.8 of a host core on average (machine saturated). |
| One core vs serial | +6.4% host instructions (the per-slice handoff) at a 100k budget. |

### Gameplay

All runs start from one snapshot at 4.75M slices (the gameplay recipe) and
cover 100k slices each. Silksong runs at exactly 60 fps of *virtual* time
in gameplay and idles until vsync, so the wall-clock fps is how fast we
simulate virtual time.

Stability after the kernel sync-word fix (`37cfe86`):

| Mode | Runs | Frames per run | Parallelism | Host instructions per frame |
|---|---|---|---|---|
| Serial | 1 | 164 | 1.00× | 2.20e9 |
| 2 cores | 8 | 145–147 | 1.35–1.36× | 2.25e9 (+2.4%) |
| 3 cores | 4 | 150–152 | 1.93–1.94× | 2.27e9 (+3%) |

- Total host work barely grows. The handoff and lock overhead is small.
- The critical path per frame (instructions per frame ÷ parallelism)
  gives the expected speedup on a machine with free cores: about **1.32×
  for 2 cores and 1.88× for 3**.
- Before the fix, one 2-core run out of 8 stalled: the main thread slept
  forever on a condvar.

Wall time on this machine (load 13–20; noisy, ±20% between identical
runs), as virtual time per wall second against serial:

| Mode | Round 1 | Round 2 | Mean |
|---|---|---|---|
| 2 cores | +25% | +17% | ≈ +21% |
| 3 cores | +41% | +28% | ≈ +34% |
| 4 cores | — | +36% | — |

One core vs serial at gameplay: bit-identical (virtual time
2,126,879,793 in every run).

### Browser (real GPU, title screen)

Measured with `node tools/perf.mjs --game NCA --url-params cores=N --seconds 60`
after the default 860k-slice warmup:

| Mode | fps | Virtual ticks/s | Slices/s | SVCs/s |
|---|---|---|---|---|
| Serial (`cores=0`) | 5.53 | 1.77M | 6541 | 3775 |
| 3 cores (the default) | **7.68 (+39%)** | 2.46M (+38.5%) | 8572 | 6049 |

## JIT + cores with poll coalescing (2026-10-04)

Poll coalescing removed the 25 µs idle slices, so in gameplay the cores
now overlap in 91% of slices (14% before). The web default under the JIT
is 3 cores again (capped at hardwareConcurrency - 2).

| Measurement | Serial JIT | JIT + 3 cores |
|---|---|---|
| Node, full recipe to 7M slices (frames per wall second) | 9.87 | 16.46 (1.67×) |
| Browser gameplay, interleaved pair 1 | 14.63 fps | 16.13 fps |
| Browser gameplay, interleaved pair 2 | 14.53 fps | 16.05 fps |

The browser gain is +10% in both pairs.

- **Long session.** 50 min of warm-up and 25 min of measured 3-core
  gameplay held 15.93 fps, with no "[jit] module rejected", OOM or
  crash. Gameplay renders correctly.
- **Bot 2's browser A/B of poll coalescing alone:** +6.8% fps.
- **Caveat: compile churn.** 3 cores compile about 4.7× the regions
  (1.01M, 871k evicted) in the Node run, so the per-core caches thrash.
  That is the next thing to look at, together with the gap between
  Node's 1.67× and the browser's 1.10×.
