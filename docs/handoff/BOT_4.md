# Bot 4: browser-side performance (build, CPU worker, wasm)

Read `/BOTS.md` first; its rules apply. Worktree `../Voland-bot4`, branch
`local/bot4`, status file `docs/handoff/BOT_4_STATUS.md`.

## Why

Silksong runs at about 3 fps in the browser on the real GPU. The native
build with the same work (GPU mode) is much faster, so part of the gap is
wasm and browser overhead. That overhead is independent of the JIT the JIT
agent is building. Find it and remove it.

## The task

1. **Measure the native vs browser gap precisely.**
   - Same work, same slice count: guest cycles per second (or slices per
     second) natively (`voland-cli --gpu-stream /dev/null`) vs in the
     browser's CPU worker. You may add a counter the page can read.
   - Profile the CPU worker in Chromium. Use Playwright plus a CDP session
     on the worker target with the `Profiler` domain, or
     `--js-flags=--prof` / `--cpu-prof`.
   - Report the top functions. Compare against native `sample` on macOS
     (`sample <pid> 10`).
2. **Build flags.** See `CMakeLists.txt` around the Emscripten section,
   and DESIGN §24. Every flag change updates §24 in the same commit.
   - Optimisation level (`-O3`), `-flto`, wasm-opt passes.
   - `-msimd128` is already on; check `-mbulk-memory` and
     `-mnontrapping-fptoint`.
   - Remove assertion and stack-check overhead in release.
   - The allocator: dlmalloc vs mimalloc (`-sMALLOC=mimalloc` with
     threads).
   - memory64 costs: check whether bounds checks dominate. Memory64 is
     required (DESIGN §4), so it stays, but look for cheap wins.
3. **Hot-path hygiene in code that is not the interpreter's
   instruction semantics:**
   - The CPU worker's run loop (`platform/web/workers/cpu.worker.ts`):
     how often it yields to the event loop, slice budget, message polling.
   - `emulator_run_slice` per-slice overhead: hid/vi/audio updates every
     slice, `nvdrv_poll_completions`.
   - Logging cost: `log_debug` formatting in hot paths. Is it compiled out
     or filtered early in release?
   - Atomics and SharedArrayBuffer access patterns.
4. **Frame pacing in the browser.**
   - Presentation should not stall the CPU worker: GPU stream backpressure
     (`core/gpu/gpu_stream.c` waits when the 4 MB ring is full; count the
     stalls).
   - Is the 4 MB `LAYOUT_GPU_RING_SIZE` enough? Changing the layout
     updates DESIGN §4.

### Files

- **You own:** the Emscripten/compile sections of the top-level
  `CMakeLists.txt` and `CMakePresets.json`, DESIGN §24,
  `platform/web/workers/cpu.worker.ts` (the run loop), `core/stubs/wasm_entry.c`,
  `core/common/log.{h,c}`, and profiling tools under `platform/web/tools/`.
- **You may touch, with care:** `core/emulator.c` (per-slice overhead
  only). Bot 1 changes the run loop for parallel threads, so keep your
  edits there small and describe them in your status file.
  `core/common/layout.{h,c}` (ring size).
- **Do not touch:** the interpreter and JIT backends (the JIT agent),
  `core/gpu/**` (the coordinator and bot 3), the scheduler (bot 1),
  nvdec/video (bot 2).

## Milestones

1. **Measurements:** native vs browser cycles per second, plus the browser
   profile top 20, in your status file.
2. **Flag and loop changes**, each with a before/after fps from the
   `chromium-gpu` measurement in BOTS.md. Keep only the ones that help.
3. **A repeatable perf harness:** one command that prints browser cycles
   per second and fps for Silksong at the title screen. Commit it so
   everyone can use it.
