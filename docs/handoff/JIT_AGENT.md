# Handoff: the ARM64 → WebAssembly JIT (second agent)

You are a second Claude Code agent working on Voland in parallel with
another agent (the "renderer agent"), which is building the WebGPU
renderer. The repo owner started you and wants the two of you to finish
sooner by not overlapping. Read this whole file before you touch anything.

## The goal

The user's legally owned, self-decrypted *Hollow Knight: Silksong*
already boots to gameplay in Voland's web build, but it runs at about
1 fps. The owner wants **15 fps at least, 30 fps ideally**. Two pieces
are needed:

1. **A WebGPU renderer.** The renderer agent owns this; it is **not your
   job**.
2. **An ARM64 → WebAssembly JIT. This is your job.** Today the web build
   interprets every guest instruction (`core/cpu/backends/interpreter/`,
   predecoded blocks in `interp_predecode.c`). Natively, interpretation is
   about 20% of frame time, and the GPU work is the other 75%. In the
   browser the interpreter is several times slower again, so with the GPU
   work moved off the CPU, the interpreter becomes the wall. Target: guest
   code at least **5–10× faster than the predecoded interpreter in the
   browser**.

## Rules (non-negotiable; also in CLAUDE.md)

- **Decryption.** Never write code that decrypts Nintendo content or
  touches keys. The NCA is already decrypted.
- **Other emulators.** Never copy code from dynarmic, yuzu, Ryubing or any
  other emulator. You may read them to understand behaviour; the
  reference for semantics is the Arm ARM (DDI 0487) pseudocode plus our
  interpreter.
- **Ballistic and `recompiler/`.** `core/cpu/backends/ballistic/**` and
  `recompiler/` stay untouched, and Ballistic's IR and headers are not
  used. Rule 10 in CLAUDE.md was rewritten on 2026-10-03 to allow **our
  own** JIT in `core/cpu/backends/jit/`.
- **Memory.** One linear memory, fixed size, never grown. Every guest
  memory access goes through the softmmu semantics in `core/common/vmm.h`.
  The JIT emits the same two-level page-table walk as
  `vmm_translate_inline()` against the same tables, and does no pointer
  arithmetic into guest RAM beyond what that walk produces.
- **Network.** No Nintendo servers, no telemetry.
- **Binaries.** Don't commit third-party binaries.
- **Process rules.** The owner has waived the CLAUDE.md process rules
  (header-review stops, phase gating, PRs). Hard rules 1–10 still stand.
  Conventional commits.

## Where you work (avoid conflicts)

- **Your own worktree and branch:**
  ```
  cd ~/Voland
  git worktree add ../Voland-jit -b local/jit local/dev
  cd ../Voland-jit
  ```
  Work and commit only there. Push with `git push origin local/jit`.
  **Never push to `local/dev`**; the renderer agent merges your branch.
  The repo is private (helloimthesigma-blip/Voland); keep it that way and never
  push to `upstream`.
- **Files you own:** `core/cpu/backends/jit/**`, `tests/jit_*`,
  `docs/JIT.md` (your design doc), and any new JS glue the JIT needs.
- **Files you may touch minimally** (small, clearly delimited edits; I
  merge them):
  - `core/CMakeLists.txt`, top-level `CMakeLists.txt` (link flags),
    `CMakePresets.json`, `tests/CMakeLists.txt`;
  - `core/cpu/cpu.c` (backend registry), `core/cpu/cpu.h` (registry
    extern only);
  - `core/cpu/backends/interpreter/*` (only to expose what the JIT needs,
    such as an exported per-instruction step or helper entry points);
  - `platform/web/workers/cpu.worker.ts`, `platform/web/src/bindings/*`.
- **Files you do NOT touch:** `core/gpu/**`, `core/hle/services/vi/**`,
  `core/hle/services/nvdrv/**`, `platform/web/workers/gpu.worker.ts`, and
  anything `gpu*`. The renderer agent owns them.
- **DESIGN.md.** Put your design in `docs/JIT.md`. Leave `docs/DESIGN.md`
  and `README.md` alone; the renderer agent adds the changelog and status
  lines from your `docs/handoff/JIT_STATUS.md`.
- **Status file.** Keep `docs/handoff/JIT_STATUS.md` current in your
  branch: what works, measured speedups, what is next, and anything you
  need from the other side. Commit it with each milestone and push, so
  the other agent can `git fetch` and read it.

## The codebase facts you need

- **CPU interface:** `core/cpu/cpu.h`. Every backend implements
  `run(state, cycle_budget) → CPU_ExitReason`. There is no
  run-to-completion; SVCs exit with `CPU_EXIT_SVC` after the handler runs.
  Register index 31 is invalid at the interface (SP has its own
  accessors).
- **Interpreter:**
  - `Interp_State` is in `interp_internal.h`. It holds the register file,
    V0–V31, FPCR/FPSR, TPIDR, the exclusive monitor and cycle counters.
    The JIT should embed or reuse this exact state so it can hand any
    instruction it can't compile to the interpreter (mixed mode) and so
    HLE keeps working unchanged.
  - Predecoded blocks and the block cache are in `interp_predecode.c`.
    `vmm_generation()` invalidates decoded code, and `invalidate_cache` /
    `clear_cache` flush it.
- **Cycle accounting:** one cycle per instruction (`interpreter.c`
  ~line 195, including the exclusive-monitor grace).
  - **Keep it exact.** Count guest instructions exactly like the
    interpreter and stop at the same budget boundaries, so that a JIT run
    and an interpreter run of the same game are bit-identical. That makes
    whole-game differential testing possible: identical frame hashes.
  - If you need to relax this for speed later, make it a switch and keep
    the exact mode for testing.
- **Softmmu:**
  - `vmm_page_table_l1()`; L1 has 8192 entries and L2 has 16384 PTEs.
  - PTE: bits 63..12 hold the host offset, bit 0 is R, bit 1 is W, bit 2
    is X.
  - Accesses that cross a page go through the out-of-line
    `vmm_read_cross_page` / `vmm_write_cross_page`.
  - A fault must leave the architectural state exactly as the interpreter
    leaves it: state unchanged and `fault_address` set.
- **Web build (`CMakeLists.txt` ~line 75):** wasm64 (`-m64`),
  `-pthread`, shared imported memory of 5.25 GB, no growth, ES6
  modularised core loaded by `platform/web/workers/cpu.worker.ts`.
  - Emscripten is at `~/emsdk` (`source ~/emsdk/emsdk_env.sh`), then
    `cmake --preset web && cmake --build --preset web`.
  - Node 24 is available (it can run memory64 + threads wasm for tests).
- **Tests:**
  - `ctest --preset native-noop` must stay green (45 tests).
  - `tests/a64_diff_test.c` runs the interpreter against real ARM64
    hardware (this Mac is arm64), and `predecode_test.c` covers the
    block cache.
  - Web: `cd platform/web && npm run typecheck && npm test && npm run e2e`.
- **Real workload:**
  - Silksong NCA: `~/games/game.nca`.
  - Native CLI recipe to reach gameplay (deterministic, about 45 min
    native):
    ```
    build/native-release/platform/cli/voland-cli run "$NCA" --backend interpreter \
      --input 860000:1:3000 --input 940000:1:3000 --input 1020000:1:3000 \
      --input 3000000:1:3000 --input 3100000:1:3000 --input 3200000:1:3000
    ```
  - `--snapshot-at N --snapshot-dir DIR` lets you iterate from a late
    point. Launch anything longer than 2 h with `nohup`.
  - Check `voland-cli --help` for `--dump-frames-every`.

## Suggested design (you own it; improve it)

1. **Mixed-mode backend `CPU_BACKEND_JIT`.**
   - Runs predecoded/interpreted blocks first.
   - Counts block executions; past a threshold (for example 50), compiles
     the block, or better a region of hot blocks, to a WebAssembly
     function.
   - Any instruction the compiler doesn't support ends the compiled block
     and falls back to the interpreter for that instruction. Coverage can
     then grow incrementally while staying correct from day one.
2. **Emitter in C.**
   - Write wasm binary bytes into an arena: no malloc in hot paths, no
     third-party code. The module imports the shared memory64 and the
     helper functions it needs.
   - On the web, compile synchronously in the CPU worker with
     `new WebAssembly.Module(bytes)` and
     `new WebAssembly.Instance(mod, imports)`. Install the exported
     function into the Emscripten function table (`addFunction`, which
     needs `-sALLOW_TABLE_GROWTH=1`; table growth is fine, memory growth
     is not). C then calls the block through a function pointer.
   - Use `EM_JS` or a `--js-library` for the glue.
   - Batch compilation if per-module overhead dominates.
3. **Code generation.**
   - Keep the guest registers in wasm locals inside a block, loaded on
     entry and stored at every exit. NZCV can be computed lazily.
   - Inline the softmmu walk for loads and stores, with a fault exit that
     restores precise state.
   - Use direct wasm ops for integer ALU, branches and loads/stores first;
     those are most of a Unity/IL2CPP game. Then do scalar FP and NEON
     (wasm SIMD128 maps well).
   - Mind FPCR (rounding, FZ, DN) and NaN propagation; match the
     interpreter's softfloat results bit-exactly, or call its helpers
     through imports.
   - Chain blocks: direct branches to already-compiled targets jump
     without returning to the C dispatcher. For example, use a region
     function with a `loop` + `br_table` over its blocks, or a per-state
     "next block" cache.
4. **Differential testing (mandatory; the interpreter is the oracle).**
   - Per instruction or block: random states run through the JIT and the
     interpreter must give the same registers, flags, V registers,
     FPSR/FPCR, memory and fault behaviour. Run it under Node with an
     Emscripten build of the test (`tests/jit_diff_test.c`).
   - Whole game: the Silksong recipe under the JIT should produce frame
     hashes identical to the interpreter's (use `--dump-frames-every`
     with the native interpreter as the baseline).
   - Natively the JIT can't execute wasm; a native build may compile the
     emitter only and validate the bytes, while execution tests run under
     Node.
5. **Measurement.** Report guest instructions per second in the browser
   for interpreter vs. JIT on the Silksong title screen and on gameplay,
   and keep the numbers in `JIT_STATUS.md`.

## Milestones (commit and push each)

1. Skeleton backend (`CPU_BACKEND_JIT` = interpreter passthrough),
   selectable with `-DCPU_BACKEND=jit`, with a web preset or option. All
   tests green.
2. Emitter, wasm module loading and table install. One trivial block type
   (ADD/SUB/MOV/B) differentially tested under Node.
3. Integer ALU, branches, loads/stores with inline softmmu, and NZCV.
   Homebrew boots under the JIT in the browser.
4. Block chaining and region compilation. Measure.
5. FP/NEON. Silksong frame hashes match. Measure fps.

When you finish or get blocked, write it in `docs/handoff/JIT_STATUS.md`,
commit, push `local/jit`, and tell the owner in your final message.
