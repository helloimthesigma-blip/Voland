# JIT status (branch `local/jit`)

Owner of this file: the JIT agent. The renderer agent reads it when merging.

## Summary

`CPU_BACKEND_JIT` (`core/cpu/backends/jit/`) compiles hot guest code to
WebAssembly at run time in the CPU worker:

- Mixed mode over the interpreter.
- Regions of blocks are compiled as wasm functions and chained with tail
  calls.
- Lazy NZCV flags.
- The softmmu walk is inlined for loads and stores.
- Anything not compiled runs in the interpreter, one instruction at a time.

It is exact: on Silksong (20,000 slices) the frame hash, virtual time and
SVC count are identical to the interpreter's.

## Milestones

- [x] **1. Skeleton.** `-DCPU_BACKEND=jit`, presets `native-jit` and
  `web-jit`, `voland-cli run --backend jit`, web backend id 3.
- [x] **2. Emitter, module loading and table install.** The C wasm writer is
  in `jit_wasm.h`. Modules are installed synchronously with
  `new WebAssembly.Module` and `addFunction`, then called through a function
  pointer.
- [x] **3. Integer ISA.** Compiled inline:
  - integer ALU, bitfield, multiply and divide;
  - branches;
  - general-register loads and stores of every addressing form;
  - SIMD&FP loads and stores;
  - LDXR/STXR/LDAXR/STLXR/LDAR/STLR;
  - NZCV, MRS/MSR of TPIDR, NZCV, FPCR and FPSR.

  Everything else goes to the interpreter for that one instruction.
- [x] **4. Chaining, regions and measurement.**
  - Regions: blocks in one page linked by direct branches form one wasm
    function, dispatched with `br_table`.
  - Exits tail-call the next compiled region (`return_call_indirect` on the
    core's table64).
  - Blocks survive vmm mapping changes through a code hash.
  - Slow paths are shared per function.
- [~] **5. FP/NEON.** Every SIMD&FP instruction (and LD1-LD4) now runs
  inside compiled code as a direct call into the interpreter's executor.
  Only the general registers or NZCV it touches are synced, so the
  instruction stays exact and avoids a full spill. Native wasm fast paths
  for the hottest FP ops are next. The full gameplay recipe has not been
  checked yet.

## How it is tested

`tests/jit_diff_test.c` runs random A64 streams through the interpreter's
`run()` and the JIT's `run()` with the same budgets, and compares:

- registers, SP, PC and NZCV;
- the V registers and TPIDR;
- the exit reason, fault address and cycle count;
- every data byte.

The streams use biased generators for flag-setter→reader pairs, the
LDAXR→STLXR idiom, and aligned or page-edge bases. The test runs:

- natively, where it exercises only the mixed-mode loop:
  `ctest --preset native-jit`;
- compiled, under Node: `ctest --preset web-jit`, or
  `node build/web-jit/tests/jit_wasm/jit_diff_test.js N SEED`.

Mutations injected into the compiler (an ADD emitted as SUB, fused GE, the
STXR status, the vector high half) are each caught.

## Measurements

The harness is `voland-cli` built for Node (`cmake --build --preset web-jit
--target voland-cli-node`). It is the same core and flags as the browser, in
V8. The workload is Silksong from boot for 20,000 slices with
`--frame-skip 100000`, profiled with `node --cpu-prof`.

Main-thread time:

| | guest execution | JIT compile | total |
|---|---|---|---|
| interpreter | 17.4 s | — | 18.1 s |
| JIT, first working version | 7.2 s | 4.1 s | 12.2 s |
| JIT + lazy flags, threshold 256 | 4.75 s | 1.1 s | 6.3 s |

This boot-heavy window overstates compile cost; the steady state is
dominated by guest execution.

Interpreter fallbacks in the same window, before SIMD&FP memory and
exclusives were inlined:

| class | fallbacks |
|---|---|
| SIMD&FP memory | 9.5 M |
| exclusive / acquire-release | 10.8 M |
| SIMD&FP arithmetic | 3.5 M |

The first two classes are now inlined, so SIMD&FP arithmetic is the next
target.

## Merge status (for the coordinator)

- `local/dev` 8d4eb35 (parallel guest threads) is merged into `local/jit`.
  - Conflicts: `interp_predecode.c`, where `run_block` now uses the
    per-thread block cache, and `voland_cli.c`.
  - Tests: 48/48 native tests pass, and the Node diff test passes.
  - Silksong, 20,000 slices: the JIT matches the interpreter's frame hash
    `d395e456e4e72325`, at 14.8 s wall time against 27.8 s.
- **Multicore mode:** `cpu_multicore()` makes the JIT run the interpreter.
  Compiled functions live in one host thread's function table, and the
  inline exclusives assume one guest thread at a time.
  - Inline LDXR records `exclusive_size` and `exclusive_value` like the
    interpreter does.
- **Web build:** the `web-jit` preset (`CPU_BACKEND=jit`) gives the JIT.
  - It has not been run in the browser.
  - It has not been run through the full gameplay recipe.

## Files touched outside the JIT's own

- `CMakeLists.txt`:
  - `jit` added to the `CPU_BACKEND` option;
  - `-sALLOW_TABLE_GROWTH=1` in the web link flags (DESIGN.md §24 should
    mention it; the table grows, memory never does);
  - `tests/jit_wasm` added under Emscripten when `CPU_BACKEND=jit`.
- `CMakePresets.json`: `native-jit`, `web-jit` (configure, build and test).
- `core/CMakeLists.txt`: the jit sources and `SWITCH_CPU_BACKEND_JIT`.
- `core/cpu/cpu.{h,c}`: registry entry.
- `core/stubs/wasm_entry.c`: `cpu_backend_id_ffi` returns 3 for jit.
- `platform/web/bindings/core.ts`: `CpuBackendId.Jit = 3`.
- `platform/cli/voland_cli.c`:
  - `--backend jit`, `--jit-threshold N` and `--jit-dump DIR`;
  - JIT statistics in the summary.
- Interpreter (`interp_internal.h`, `interp_predecode.c`):
  - `interp_predecode_run_block()`, `interp_code_generation()` and
    `interp_is_cache_maintenance()` are exported.
  - **Behaviour change:** decoded code is now flushed only after IC IVAU.
    Before, every SYS CRn=7 instruction flushed it, including DC ZVA, which
    `memset` runs constantly. This also speeds up the plain interpreter.

## Needs from the renderer side

- **Browser e2e.** The JIT has not been run in the browser yet. Under Node
  it needs nothing from the page beyond what the core already has.
- **Pixel workers spin.** In the Node CLI the software rasterizer's pixel
  workers spin at 100% of a core each while idle (`wasm-function[177]` in
  their profiles). It does not affect correctness, but it skews CPU-time
  measurements. It will matter on low-core machines.
