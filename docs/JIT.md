# The ARM64 → WebAssembly JIT

`CPU_BACKEND_JIT` (`core/cpu/backends/jit/`) compiles hot guest A64 code
to WebAssembly at run time and runs it in the same linear memory as the
rest of the core. It is Voland's own code: Ballistic and `recompiler/`
are not used (CLAUDE.md rule 10). This document describes how it works,
why it is shaped the way it is, how it is tested and how to measure it.

DESIGN.md §11 describes the original JIT plan, built around Ballistic,
a compiler worker and a persistent translation cache. The JIT that
exists differs from it in several places. Where they disagree, this
document describes the code.

## Goal and model

In the browser, interpreting guest code is the main cost once rendering
moves to the GPU. The JIT's job is to run guest code several times
faster than the predecoded interpreter, without giving up any
correctness.

**Mixed mode over the interpreter.** The JIT does not replace the
interpreter; it runs on top of it.

- The architectural state is the interpreter's. `Jit_State`
  (`jit_internal.h`) starts with an `Interp_State`, so every accessor,
  the SVC, undefined-instruction and breakpoint plumbing, and the cache
  maintenance entry points are the interpreter's own. Only `create()`
  and `run()` differ. HLE sees no difference between the two backends.
- Cold code runs in the predecoded interpreter
  (`interp_predecode_run_block`). Each block start's execution count is
  kept in a small hashed counter table.
- When a count reaches the hot threshold (256 by default;
  `jit_set_hot_threshold`, `voland-cli --jit-threshold N`), the region at
  that PC is compiled to one wasm function and installed.
- Anything the compiler does not inline is executed by the interpreter,
  one instruction at a time, from inside the compiled code
  (`jit_helper_interpret`). Coverage can grow one instruction class at a
  time, and the result is always the interpreter's.

**The interpreter is the reference.** Every compiled form is
differentially tested against it (see "Testing"), and the two backends
must agree on registers, flags, vector registers, FPCR/FPSR, memory,
faults and cycle counts.

**Exact cycle accounting.** Like the interpreter, the JIT counts one
cycle per guest instruction and stops at exactly the same budget
boundaries. A compiled region is entered only if the remaining budget
covers its whole entry block; otherwise the interpreter runs that block.
So a game run under the JIT and under the interpreter, in the same
build, has identical virtual time and SVC counts. That makes whole-game
differential checks possible.

**Natively, nothing is compiled.** There is no wasm engine in a native
build: installation fails, and `run()` is the interpreter's loop. The
`native-jit` preset still builds and tests the emitter and the
mixed-mode loop. Compiled code only executes under Emscripten (in the
browser, or in Node for tests).

## The run loop

`jit_run()` (`jit.c`), per call:

1. Pick up any finished asynchronous compilations (see "Sync vs async
   compile").
2. At each block start, look the PC up in this host thread's region
   cache. A hit that is valid in the current code generation, and whose
   entry block fits the remaining budget, is called through a function
   pointer.
3. A compiled region returns `JIT_BLOCK_CONTINUE` (the state in memory is
   complete; carry on from `regs.pc`) or `JIT_BLOCK_STOP` (the run ends
   with `Jit_State.exit_reason`: an SVC, a fault, an undefined
   instruction, the budget).
4. On a miss, bump the PC's hit counter. At the threshold, compile. If
   there is still nothing to run, the interpreter runs one predecoded
   block.

Compiled regions chain directly to each other (see "Region
transitions"), so the C loop sees mostly misses and stops.

## Code generation

`jit_compile.c` turns a region into a complete wasm module, written
byte by byte into a per-thread buffer by `jit_wasm.h`, a minimal binary
writer. No third-party code is involved.

The module imports:

- `env.memory`: the core's shared memory64;
- `env.table`: the core's 64-bit function table, for chaining;
- `env.interpret`, `env.read`, `env.store`, `env.write`, `env.simd`: the
  `jit_helper_*` functions.

It exports one function, `b`, with the signature
`(param $state i64) (result i32)`.

**Function shape.**

```
prologue: l1 and every guest register the region touches -> locals
block $raw                       ;; leave with $status, state complete
  block $exit                    ;; leave normally: $npc, $ncycles
    ...region blocks, dispatched with br_table...
  end
  epilogue: written locals -> state, pc = $npc, cycles += $ncycles
  return JIT_BLOCK_CONTINUE
end
return $status
```

The body is emitted twice. The first pass discovers the region's blocks
and collects which registers it reads and writes; the second emits the
real code, with loads and stores of exactly those registers. Only locals
the first pass saw used are declared: engines zero every declared local
on each call, and small regions are called millions of times.

**Guest registers in locals.** Inside a region, X0–X30, SP and NZCV live
in wasm locals. They are written back at every exit, before every
interpreter call, and wherever the state must be complete.

**Lazy flags.** After ADDS, SUBS, ANDS and similar, only the operands
and the result are kept. A reader right after them (CMP + B.cond)
tests the operands directly: two compares rather than a 40-operation
NZCV build. NZCV is built only where it can be observed: exits, the
interpreter, and edges into region blocks that may read the flags
before writing them.

**Loads and stores.** The softmmu walk of `vmm_translate_inline()`
(DESIGN.md §5) is emitted inline against the same page tables: the L1
index, the L2 entry, the permission bits, the host offset. No pointer
arithmetic into guest RAM happens beyond what that walk produces.

- In front of the walk sits a one-entry translation cache per
  permission, in locals: an access within the page last translated for
  the same permission is `host = gva + delta`. Mappings only change in
  SVCs, which leave compiled code, so this is exact in serial mode. It is
  compiled out when guest threads run on several host threads, because
  another core may remap mid-function.
- Page-crossing accesses go to `jit_helper_read`, `jit_helper_store` or
  `jit_helper_write`, which are all-or-nothing, exactly like the
  interpreter's `interp_read`/`interp_write`.
- When the walk or a helper refuses (unmapped, no permission), the
  instruction is handed to the interpreter, which raises the fault with
  the state exactly as it would have been. Compiled code never reports a
  fault itself.

**Exclusives and ordering.** LDXR/STXR/LDAXR/STLXR/LDAR/STLR are
compiled inline. Code compiled while guest threads run on several host
threads (`cpu_multicore()`, see `docs/PARALLEL.md`) differs: a
store-exclusive is a `cmpxchg` against the value LDXR read, DMB/DSB are
`atomic.fence`, and acquire/release accesses are fenced. Each cache
entry records which mode it was compiled for, and a mode change
retires it.

**SIMD and floating point.**

- Integer Advanced SIMD bitwise operations and permutes are compiled
  inline to SIMD128. Instructions whose result depends only on the
  destination (MOVI, MVNI, ORR/BIC immediate, FMOV immediate) are
  evaluated at compile time by running the interpreter on two scratch
  states.
- Everything else in SIMD&FP is executed by a direct call into the
  interpreter's executor (`jit_helper_simd`). Only the general registers
  or NZCV that instruction touches are synchronised around the call, so
  it stays exact without a full spill.
- The hottest scalar and vector FP operations (FADD, FSUB, FMUL, FDIV,
  FSQRT, FMLA and friends, FMIN/FMAX, FCMP) have native wasm fast paths
  behind exactness guards. The interpreter's FP is softfloat with Arm
  semantics and sticky FPSR flags. Native f32/f64 gives identical bits
  and flags when FPCR is 0 (round to nearest, no FZ, no DN), no NaN is
  involved, the result is finite and either an exact zero or strictly
  above the smallest normal, and FPSR.IXC is already set (so an inexact
  result changes nothing). Vector forms apply the same rules to every
  active lane. Anything else takes the exact path, which has not changed
  any state yet when the guard fails.
- **More inline forms, chosen from a census of the direct calls during
  Silksong gameplay** (`voland-cli --jit-fallbacks-from N`; the profile
  marks entries that are a fast path's exact arm):
  - **Pure moves:** element moves (DUP general, INS general and element,
    SMOV, UMOV, scalar DUP) and vector FABS/FNEG.
  - **SIMD128 integer ops:** pairwise ADDP/SMAXP/UMAXP/SMINP/UMINP, and
    SSHLL/USHLL(2).
  - **Conversions under the scalar rules:** SCVTF/UCVTF and
    FCVTZS/FCVTZU, as vector, scalar-element and fixed-point forms. Only
    the integer-to-FP rounding can be inexact; the fixed-point scale is
    an exact power of two. FP-to-integer requires every active lane in
    range, so neither saturation nor IOC can occur.
  - **FP compares:** FCMEQ/FCMGE/FCMGT/FACGE/FACGT and the compares with
    zero, when no active lane is a NaN and FPCR is 0.
  - **Exactness instead of a set IXC.** In SSBU, 94% of SIMD&FP direct calls
    ran with FPSR.IXC clear, so the fast paths above refused nearly
    everything. The scalar arithmetic (FADD, FSUB, FMUL, FNMUL, FDIV,
    FSQRT) and the integer↔FP conversions now decide natively whether
    their result needed rounding (`L_EXACT`):
    - singles are widened to f64, where the sum (checked by TwoSum),
      product, quotient check and square check are exact;
    - double sums use Knuth's TwoSum, and products, quotients and roots
      use Dekker's split product, inside 2^±480;
    - an integer converts exactly when its significant bits fit the
      fraction (clz + ctz);
    - a truncation is exact when trunc(x) == x.

    An exact result leaves the flags alone. An inexact one sets IXC in the
    fast arm, exactly as softfloat would, since the result guards already
    exclude overflow and underflow. Only "not known" (Dekker out of range)
    still needs IXC set beforehand. Seeded mutations were each caught in
    100k streams: no IXC set, a sum or a truncation's exactness wrong, the
    Dekker error ignored, an integer always exact.
  - **Tests:** `jit_diff_test` generates these forms on registers holding
    ordinary floats; seeded mutations of each were caught. One mutation
    is not caught in 20000 streams: signed instead of unsigned truncation
    for lanes in [2^31, 2^32).

**Code validity.** Regions are compiled only from pages that are
executable and not writable.

- Each cache entry records the code ranges it was compiled from (up to
  four pages) and a 64-bit hash of their instruction words.
- When the code generation moves (any vmm mapping change, IC IVAU,
  `invalidate_cache`/`clear_cache`), an entry is not thrown away. It is
  re-checked the next time its PC comes up: still executable and not
  writable, and the same hash. Compiled code depends on nothing else.
  Games map and unmap data constantly, and retiring every region on
  every mapping change meant recompiling everything.
- Decoded and compiled code is flushed only by IC IVAU, not by every
  `SYS` with CRn = 7. DC ZVA, which `memset` runs constantly, used to
  flush it.

## Region compilation

A region is the block at the entry PC plus the blocks it reaches, all
in one wasm function. Inside the function, a `loop` and a `br_table` over
the region's blocks act as the dispatcher, so a branch between region
blocks is a local jump.

What joins a region:

- **Direct branches**: B, B.cond, CBZ/CBNZ, TBZ/TBNZ, both arms of a
  conditional, and the fall-through after an instruction handed to the
  interpreter.
- **Calls into small callees.** A BL target joins the region if it is a
  leaf function or a PLT stub: within its first 16 instructions it
  returns or branches away (RET or BR) without calling anything. The
  return site joins too, and a RET goes back to an in-region return site
  through a guarded internal transition. Larger callees stay regions of
  their own, compiled once instead of into every caller.
- **Predicted PLT branches.** For an ADRP/LDR/BR stub, the branch target
  is read from the GOT at compile time. The prediction is guarded, so a
  stale GOT only costs an exit, never correctness.

Limits: 32 blocks per region (`JIT_MAX_REGION_BLOCKS`), 128 instructions
per block (`JIT_MAX_BLOCK_INSNS`), and four code pages
(`JIT_MAX_REGION_PAGES`), each validated by its own range hash.
`voland-cli --jit-no-calls` turns call spanning off.

Before a branch to another region block, the code checks that the
target block's instructions still fit the budget. If they do not, the
region exits and the dispatcher interprets the block, which keeps budget
boundaries exact.

## Region transitions

A transition between compiled regions costs far more than an
instruction inside one. `tests/jit_bench.c` put it at about 8–10 ns
under Node on one development machine, the cost of 30–40 straight-line
instructions. Games call tiny functions and PLT stubs millions of times,
so this shapes the design in two ways:

1. **Regions grow across calls** (above). In the benchmark, a BL/RET loop
   went from 3.3 to 0.87 ns per instruction when callees joined the
   caller's region.
2. **Exits chain without returning to C.** A region's exit hashes the
   next PC into the region cache and checks each way of its set. A way
   is entered only if it holds that PC, it was validated in the current
   code generation, and its entry block fits the remaining budget. That
   is the same check the dispatcher makes. The exit then tail-calls the
   function with `return_call_indirect` on the core's table, so chains
   of regions never grow the host stack. Only a miss returns to the C
   loop.

Compiled code reads the cache's address from `Jit_State.thread_cache`
and the code generation from the shared `g_vmm_generation`. It never
bakes in a per-thread address, so a region compiles to the same bytes
on every core (see "Parallel guest threads").

Spanning only small callees did better than spanning every callee. On a
Node run of a commercial game's boot-to-gameplay input recipe (process
CPU time on one development machine):

| | CPU to slice 3.5M | 500k gameplay slices | module bytes |
|---|---|---|---|
| no spanning | 594 s | 32.6 s | 320 MB |
| span every callee | 836 s | 25.8 s | 660 MB |
| span small callees | 492 s | 19.5 s | 352 MB |

Spanning every callee copied large functions into every caller, which
doubled the bytes to compile and lost in load time.

## The region cache

Each host thread has a region cache of 2^17 entries
(`JIT_CACHE_BITS`), organised as 32K sets × 4 ways (`jit_internal.h`).
A PC's set is chosen by Fibonacci hashing.

- A new region takes an empty way of its set. Otherwise it takes, by
  second chance, the first way not entered since the replacement hand
  last passed. Both chained exits and the dispatcher mark a way as
  entered.
- An evicted region's function is removed from the table
  (`removeFunction`) when its way is reused.

The cache used to be direct mapped. Hot regions that hashed to the same
slot then evicted each other forever: in steady gameplay there were as
many evictions as compiles, and evicted code ran in the interpreter until
it became hot again. Measured on the same Node workload, to slice 3.5M,
one core:

| cache | regions compiled | evicted | module bytes | interpreted block runs |
|---|---|---|---|---|
| direct mapped, 2^17 | 111,896 | 64,760 | 813 MB | 34.7 M |
| direct mapped, 2^19 (4× memory) | 71,781 | 19,381 | 468 MB | 24.4 M |
| 4-way, 2^17 | 54,782 | 1,278 | 415 MB | 20.1 M |

The problem was slot conflicts, not capacity: four ways at the old size
beat a direct-mapped cache four times larger. Virtual time and SVC counts
were identical in all three runs.

**Hot threshold.** 256 interpreted executions before compiling. A
threshold of 16 compiled about 2.5× the modules for the same guest speed,
and module compilation is the JIT's largest overhead while games load.

## Sync vs async compile

Compiled modules are installed into the core's function table with
Emscripten's `addFunction` (`EM_JS` glue in `jit.c`):

- **Synchronous (the default).** `new WebAssembly.Module(bytes)`,
  `new WebAssembly.Instance(module, imports)`, then `addFunction` on the
  exported `b`. The core runs in a worker, where synchronous compilation
  is allowed. The region is callable as soon as `compile()` returns.
- **Asynchronous (opt-in).** `WebAssembly.compile()` runs on V8's
  background threads while the block keeps running in the interpreter.
  The promise resolves between the CPU worker's bursts and queues
  `(id, table index)`; the next `run()` installs it if the code it was
  compiled from is still what is mapped and nothing replaced it. Only
  the thread that returns to its event loop (the CPU worker's own) can
  see a promise resolve, so cores in parallel mode always compile
  synchronously. If many compilations are requested and none ever
  completes, the JIT falls back to synchronous for good.

Asynchronous compilation measured slower in the browser: at a game's
title screen, 14.9 against 18.0 fps, with a slower warm-up, because hot
code ran interpreted for longer. It stays available for experiments: set
`globalThis.volandJitAsync = true` in the CPU worker.

`jit_compile_block` keeps its working state in statics, so a mutex
serialises compilation across host threads. Each thread has its own
512 KiB output buffer.

## Parallel guest threads

The JIT supports the parallel scheduler (`supports_multicore`). The
details are in `docs/PARALLEL.md`, "The JIT on several cores". In short:

- Every wasm thread has its own function table, so each host thread has
  its own `Jit_Thread`: region cache, hit counters, module buffer and
  statistics.
- Regions compile to identical bytes on every core, so V8 shares one
  compiled module process-wide.
- In multicore mode, hit counters are shared, and a region one core has
  compiled is compiled by another core on first sight instead of being
  interpreted up to the threshold again.

## The web build

- **Presets** (`CMakePresets.json`):

  | Preset | Backend | Purpose |
  |---|---|---|
  | `web` | `jit` | The shipping browser build. |
  | `web-jit` | `jit` | The same backend in its own build directory; runs the Node tests (`ctest --preset web-jit`). |
  | `web-interpreter` | `interpreter` | The browser build on the reference interpreter, for A/B comparisons. |
  | `native-jit` | `jit` | Native: the interpreter plus the emitter. Builds and tests the mixed-mode loop. |

- **Link flags.** `-sALLOW_TABLE_GROWTH=1` lets `addFunction` grow the
  function table. Linear memory never grows (DESIGN.md §4, §24).
- **Backend id.** `cpu_backend_id_ffi()` returns 3 for the JIT
  (`CpuBackendId.Jit` in `platform/web/bindings/core.ts`). The CPU worker
  logs the backend at load.
- **No per-frame messages.** Compilation and installation happen inside
  the CPU worker; nothing crosses `postMessage`.

## V8 quirks

Engine behaviour the JIT relies on or works around. These are
observations of specific V8 versions (Node 24 / V8 13, and Chromium of
the same period), not specification guarantees.

- **`v128.load32_splat` and `v128.load64_splat` above 4 GiB.** In
  Liftoff (V8's baseline tier), these returned zeros at memory64
  addresses at or above 4 GiB. Linear memory is about 5.25 GB, and the
  CPU state lives above 4 GiB. The JIT never emits these instructions:
  a by-element FP operation does a scalar load and a splat instead
  (`jit_compile.c`).
- **`WebAssembly.Module` refuses views of shared memory.** The emitted
  bytes are in the core's shared memory, so they are copied into one
  reused, non-shared staging buffer before compilation.
- **`addFunction` returns a BigInt in wasm64 builds**, because table
  indices are 64-bit there.
- **Wasm code space.** V8 reserves a limited code space per process
  (4 GiB here). When each core baked its own cache address into its
  modules, three cores compiled three copies of every region and
  exhausted it under Node. Identical bytes per core fixed that, because
  V8 shares identical modules.
- **Small functions are not inlined across calls.** A shared softmmu
  walk function (one call per access) made modules smaller, but
  memory-heavy loops ran 20–90% slower in `tests/jit_bench.c`. The walk
  is therefore emitted inline. `JIT_SHARED_WALK` in the environment
  selects the call, for experiments; `JIT_NO_TLB` disables the
  one-entry translation cache.
- **Declared locals are zeroed on every call.** Only the locals a
  region uses are declared.
- **Compiled modules cannot be persisted.** Chromium refuses to store a
  `WebAssembly.Module` in IndexedDB (`DataCloneError`). A cross-session
  cache would have to store the emitted bytes and compile them again,
  which saves the emit (small) but not V8's compile (DESIGN.md §11,
  "Persistence reality"). V8's own wasm code cache only applies to
  `compileStreaming` of HTTP-cached responses of 128 KB or more. No
  cross-session cache exists today.
- **Liftoff vs TurboFan.** Running Chromium with `--no-liftoff` was about
  5% faster at a title screen, within measurement noise, and a page
  cannot set engine flags anyway. The JIT does nothing special for
  tiering.

## Testing

- **`tests/jit_diff_test.c`** runs random A64 instruction streams through
  the interpreter's `run()` and the JIT's `run()` with a hot threshold of
  1, so every block is compiled on first sight, with the same sequence of
  budgets. It compares registers, SP, PC, NZCV, the V registers, TPIDR,
  the exit reason, the fault address, the cycle count and every data
  byte.
  - Generators are biased towards flag-setter → reader pairs, the
    LDAXR → STLXR idiom, aligned and page-edge bases, and a second code
    page with callees and a PLT-style stub.
  - Budgets vary, so compiled regions are both entered and refused.
    Streams loop back on themselves, so regions run repeatedly.
  - Natively (`ctest --preset native-jit`) it exercises only the
    mixed-mode loop. Under Emscripten (`tests/jit_wasm`,
    `ctest --preset web-jit`, or
    `node build/web-jit/tests/jit_wasm/jit_diff_test.js N SEED`) blocks
    really are compiled and executed.
  - Mutations injected into the compiler (an ADD emitted as SUB, a fused
    GE condition, a wrong STXR status, a dropped vector high half) were
    each caught.
- **`tests/predecode_test.c`** and **`tests/a64_diff_test.c`** keep the
  reference honest: the predecoded interpreter against the
  one-instruction decoder, and the interpreter against real ARM64
  hardware where the host is arm64.
- **Whole-game check.** Run the same title with `--backend interpreter`
  and `--backend jit` in the same Node build (`voland-cli-node`, below)
  for the same number of slices. Virtual time, SVC count and the final
  frame hash must be identical. Comparing against the *native*
  interpreter is weaker: native and wasm builds can diverge in host-side
  floating point (the software rasterizer) without the guest's history
  differing, so compare like builds.

## Measuring

All figures in this document come from one development machine. They
show the size of an effect, not a promise.

- **`tests/jit_bench.c`**: a micro-benchmark of region transitions (a
  call through a PLT-style stub into a tiny function and back) and of
  straight-line ALU work, in nanoseconds per guest instruction. It is
  only meaningful under Emscripten:

  ```
  cmake --build --preset web-jit --target jit_bench_wasm
  node build/web-jit/tests/jit_wasm/jit_bench.js [iterations]
  ```

- **`voland-cli` under Node** (`tests/jit_wasm/CMakeLists.txt`): the same
  core and flags as the browser, in the same engine.

  ```
  cmake --build --preset web-jit --target voland-cli-node
  node build/web-jit/tests/jit_wasm/voland-cli.js run GAME.nca --backend jit ...
  ```

  - `--backend interpreter` vs `--backend jit` compares what the browser
    would run.
  - Profile with `node --cpu-prof`; the target is linked with
    `--profiling-funcs`, so profiles show wasm function names.
  - The summary prints the JIT counters (`Jit_Stats`): regions compiled,
    evictions, module bytes, interpreted block runs, interpreter
    fallbacks by class, and compile and install time.
  - `--jit-fallbacks` prints the commonest interpreted opcodes
    (`--jit-fallbacks-from N` from slice N on). `--jit-hot N` prints the
    hottest regions. `--jit-dump DIR` writes every module as
    `DIR/<pc>.wasm` plus its guest code as `DIR/<pc>.s`.

- **The browser**: `platform/web/tools/perf.mjs` boots a game in
  Chromium on the real GPU and prints slices/s, virtual ticks/s, fps,
  where the CPU worker's time goes, and (with `--profile` or `--phases`)
  a CDP profile split by code kind: JIT-compiled code, JIT helpers,
  interpreter fallback and the rest. It also counts run-time
  `WebAssembly.Module` compilations per phase.
  `tools/profiling/categorize-cpuprofile.js` buckets a saved profile the
  same way. Build the core with `-DVOLAND_WASM_PROFILING=ON` for wasm
  function names.

For orientation, at one commercial game's title screen in Chromium the
JIT went from about 7.7 fps (interpreter) to about 19 fps over the
course of the work described here: vector FP fast paths, the exactness
guards, and regions spanning small calls each gave a visible step. At
that point the CPU worker's profile was dominated by JIT-compiled code,
with the remaining interpreter fallbacks mostly SIMD&FP instructions
without a fast path.

## AArch32

32-bit titles (Mario Kart 8 Deluxe) run on `CPU_BACKEND_A32_JIT` (`jit.c`), which is this runtime over the A32 interpreter (`cpu/backends/a32`).

**State and runtime.**
- `A32_State` overlays `Jit_State`: a union of the interpreter's `Interp_State` and the JIT's state, followed by the A32-only fields. `Jit_State.isa` says which instruction set a state runs.
- The runtime is unchanged: the region cache, chaining, async compile and code generations work the same way.
- Three things dispatch on the ISA:
  - the cold path (`a32_run_block`: up to a branch, 64 instructions at most);
  - the per-instruction fallback (`jit_helper_interpret` → `a32_jit_interpret`);
  - the direct call (`jit_helper_simd` → `a32_execute`).

**Front end.** `jit_compile_a32.inc` is included by `jit_compile.c` and uses its emitter, chosen by `Jit_Link.aarch32`.
- r0-r14 are x0-x14 in locals, zero-extended. NZCV uses the same lazy flags at 32-bit width; A32 ADDS/SUBS/CMP set flags as A64's W forms do.
- A new lazy kind, `FLAGS_NZ`, covers A32 logical ops with S and MULS: N and Z come from the result, while C (the shifter's carry, written eagerly) and V stay in L_NZCV.
- A conditional instruction is wrapped in `if (cond)`. When its body reads or writes the flags, they are made live before the `if`, so both arms agree.
- Inlined:
  - data processing in all three operand forms, including ALU writes to the PC (jump tables, `MOV pc, lr`);
  - MOVW/MOVT, MUL/MLA/MLS and the long multiplies;
  - LDR/STR/LDRB/STRB, LDRH/LDRSB/LDRSH/STRH and LDRD/STRD;
  - LDM/STM in all four modes, using one softmmu walk for the whole run, with POP-PC returns;
  - B/BL, BX/BLX (register);
  - SXT/UXT(A)B/H, BFI/BFC/UBFX/SBFX, CLZ, REV/REV16;
  - LDREX/STREX, LDAEX/STLEX (byte, halfword, word) and LDA/STL, with the interpreter's per-thread monitor (exact address and size);
  - MRC/MCR of TPIDRURO and TPIDRURW;
  - VLDR/VSTR, VLDM/VSTM (VPUSH/VPOP), VLD1/VST1 of one to four D registers (one walk; a page crossing goes to the interpreter), VMOV (core registers, immediate), VMRS APSR_nzcv;
  - Advanced SIMD on wasm SIMD128 (Qn is V[n]): F32 VADD/VSUB/VMUL/VMLA/VMLS, vector and by scalar; VAND/VBIC/VORR/VORN/VEOR/VBSL/VBIT/VBIF; integer VADD/VSUB and VMUL.I16/I32; VEXT; VDUP (scalar and core); VMOV/VMVN/VORR/VBIC (immediate); VMOV to and from scalars; VLD1/VST1 of one lane and VLD1 to all lanes. Advanced SIMD floats always use the standard FPSCR value (flush to zero, default NaN), so the fast arm needs every input normal or zero, every result normal or an exact zero, and IXC already set;
  - VFP arithmetic with the A64 fast paths' exactness guards: VADD/VSUB/VMUL/VNMUL/VDIV/VSQRT, VMLA/VMLS/VNMLA/VNMLS, VFMA/VFMS/VFNMA/VFNMS (single), VCMP/VCMPE, VCVT between F32 and F64 and to and from 32-bit integers. The exact arm is the interpreter.
- **Calls are spanned as on A64.** A BL into a small ARM callee (one that returns within 16 instructions through BX LR, POP {..., PC}, LDR PC, [SP], #4 or MOV PC, LR without calling anything), or into a PLT stub (ADD ip, pc; ADD ip, ip; LDR pc, [ip, #off]!), runs inside the region. Every indirect exit then checks the region's return sites first.
  - A PLT stub's target is the GOT slot's current contents, read at compile time.
  - BX/BLX (register) targets are predicted from the region's entry state: the registers as they are when the region is compiled, carried through the entry block's immediate-offset LDRs and register MOVs (a vtable call: `ldr r1, [r0, #12]; blx r1`).
  - Each prediction is a guarded branch; a miss leaves as before.
  - On MK8DX in a race, these cut region entries from 702 M to 440 M over 950,000 slices, and browser fps while driving rose from 16.1 to 16.6.
- A loaded or computed PC with bit 0 set (Thumb) goes to the interpreter before anything is committed.
- VFP and Advanced SIMD instructions are direct calls to the A32 interpreter, synchronizing only the general registers they name. Inside a compiled condition they are passed with the condition rewritten to AL.
- Everything else goes to the interpreter one instruction at a time. Instructions that may write the PC end the block.

**Tested by** `tests/a32_jit_diff_test.c` (`a32_jit_diff_test_node`), which runs random A32 streams through both backends with the same budgets. The streams use every inlined form plus VFP/NEON, conditions, PUSH/POP and BX LR returns, and loops. A third of them also get small callees (each return form), a PLT stub and a vtable that BLs and `LDR r12, [r9, #off]; BLX r12` call. Planting a wrong jump at a return site or at a predicted target fails it within about 20 streams. Registers, PC, NZCV, D registers, FPSCR, exit, fault, cycles and memory are compared. A planted carry bug fails it within 31 streams.

Measured on MK8DX with voland-cli under Node, no rendering:
- **First 20,000 slices:** 29.2 s on the A32 interpreter, 10.7 s on the JIT (first cut, before the media and VFP direct paths). Virtual time and SVC counts are identical.
- **60,000 slices, to the title screen:** 11.1 s on the JIT, about 34% of real time including boot.
- **140,000 slices (title and menus):** the VFP compare/convert/multiply-accumulate, exclusive and TPIDRURO paths cut direct interpreter calls from 20.0 M to 10.5 M and helper calls from 8.0 M to 1.8 M. Wall time went from 358 s to 289 s on a busy machine. VLD1/VST1 and VLDM/VSTM then took direct calls down to 5.6 M, and the Advanced SIMD forms to 1.4 M.

## Limits and next steps

- Compiled modules are one function per region; batching several regions
  per module would cut per-module overhead during loads.
- SIMD&FP instructions without a fast path still run through the
  interpreter's softfloat.
- Nothing persists across sessions (see "V8 quirks").
- In parallel mode each core compiles into its own cache, which
  multiplies compiles and evictions. Sharing the compiled code is
  possible because the bytes are identical, but the tables are per
  thread.
