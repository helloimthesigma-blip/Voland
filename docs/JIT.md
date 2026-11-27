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
- When a count reaches the hot threshold (8192 by default, see "Hot threshold";
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

**Guest registers.** Inside a region, NZCV and the busiest guest
registers live in wasm locals: at most 8 of X0–X30 and SP, each used at
least twice in the region (the analysis pass counts). The others are
loaded from and stored to the state where the code uses them. Locals are
written back at every exit, before every interpreter call, and wherever
the state must be complete; the others need nothing there.

Until 0.5.6 every register a region touched lived in a local. Each local
is live across the region's dispatch loop, so with 31 registers, SP, the
translation caches and the temporaries V8 ran out of machine registers
and spilled, and every region entry and exit moved every one of them.
Hollow Knight's main thread enters a region every ~27 instructions.
`JIT_LOCAL_REGS=N` sets the count (32 restores the old behaviour); in the
browser (2026-11-10, two runs each):

| JIT_LOCAL_REGS | Hollow Knight walk / fight / standing fps | Silksong bench / walk / end fps |
|---|---|---|
| 32 (before) | 46.2-48.1 / 49.0-50.8 / 52.8 | 56.3-57.0 / 55.0-55.3 / 57.3-57.8 |
| 12 | 48.6-48.7 / 50.2-51.5 / 53.0-54.3 | |
| 8 | 49.0-50.2 / 50.9-52.4 / 54.0-55.1 | 58.8 / 57.1-57.2 / 59.3 |
| 4 | 49.5-50.0 / 51.8-52.9 / 55.1-55.9 | |
| 0 | 49.9-50.0 / 51.5-52.6 / 54.9-55.4 | |

Below 8 it is flat for Hollow Knight; 8 leaves a tight loop its hot
registers. Super Mario Odyssey's New Donk City (C++, main thread bound,
2026-11-23): 12 gave 47.6 fps against 48.5-48.6 at 8. `tests/jit_bench.c` in a Chromium worker was the same or
slightly faster at 8 in every case.

What did not help (each tried in the browser and reverted, 2026-11-10):
a page-indexed translation array behind the one-entry caches (it took
~95% of the page-table walks away but cost ~7% fps), calls that return
(below), lowering the hot threshold (below), smaller regions (16 or 8
blocks: fewer transitions saved matter more than smaller functions), the
probe as a shared module function (neutral), acyclic regions without
the dispatch loop (fight 40.9-48.1 vs 49.0-53.2 fps), and regions that
stop at other regions' entries instead of copying their code (each guest
instruction is in ~3.8 regions; this cut the copies but cost far more in
transitions and compiles: fight 39.4 vs 54.1 fps, Nine Sols 34.8 vs 45.1,
Celeste's load 50 vs 36 s). Code added to a
region costs more than the events it saves.

**Lazy flags.** After ADDS, SUBS, ANDS and similar, only the operands
and the result are kept. A reader right after them (CMP + B.cond)
tests the operands directly: two compares rather than a 40-operation
NZCV build. NZCV is built only where it can be observed: exits, the
interpreter, and edges into region blocks that may read the flags
before writing them.

**Loads and stores.** The softmmu walk of `vmm_translate_inline()`
(DESIGN.md §5) runs against the same page tables: the L1 index, the L2
entry, the permission bits, the host offset. No pointer arithmetic into
guest RAM happens beyond what that walk produces. The walk is inlined at
every access that misses its translation cache. It can instead be a
function of each module (`FUNC_WALK`, `JIT_SHARED_WALK=1`): modules are
22% smaller, and on Silksong's load under Node the shared walk took CPU
from 167 to 150 s and wall time from 68 to 65.5 s (Celeste 60 to 57 s,
Nine Sols 192 to 182 s), so it was the default until 0.5.5. But V8 does
not inline the call, and Unity gameplay misses the cache about once per
10 instructions: Hollow Knight's Crossroads fight ran ~53 fps inline
against ~49-51 with the call, for about 3% more wall time on Silksong's
load. The walk requires every permission bit asked for, so the SP
entry's read-and-write walk is the same code.

- In front of the walk sits a one-entry translation cache per
  permission, in locals: an access within the page last translated for
  the same permission is `host = gva + delta`.
  - SP-based accesses have an entry of their own (pages both readable
    and writable, filled by a walk that requires both), carried between
    regions like the others. A function's stack frame and the heap data
    it works on no longer evict each other: on Celeste's loading, 3.0 G
    SP-based accesses missed 0.2 M times.
  - The two busiest other base registers of a region get slots of their
    own, which start each call from the shared entry and refill it on a
    miss: a loop reading two buffers keeps both pages.
    `JIT_TLB_SLOTS_MAX=N` in the environment sets 0 to `JIT_TLB_SLOTS`
    (4) for A/B runs. Four was the default until 2026-11-25. With 64-block
    regions, two made Super Mario Odyssey's New Donk City faster in six of
    six browser pairs (+1.2-1.8%); 0 and 1 measured the same as 2 there.
    Seaside and Luncheon came out even, and Silksong, Mario Kart 8 Deluxe
    and Tears of the Kingdom were no slower.
  - A region loads and stores only the carried entries it uses.
  - The cache goes from region to region through the state
    (`Jit_State.tlb_*`). A region's prologue takes it only if the
    translation generation (`vmm_map_generation`, bumped by every unmap
    and every permission a page loses) is the one the cache was filled
    in; otherwise it starts empty. New mappings bump nothing: no cache
    can describe a page that was unmapped. The code generation that
    compiled regions and predecoded blocks are checked against moves only
    when an executable page is unmapped or loses a permission, and on
    code flushes; Silksong's load used to move it 1831 times in 300k
    slices (svcUnmapPhysicalMemory frees page by page), sending every
    region back to rehash its pages, and now moves it once.
  - Mappings only change in SVCs, which leave compiled code, so this is
    exact in serial mode.
  - With guest threads on several host threads, another core may remap
    while a region runs. The next region sees the new generation, and so
    does this one after every fence (`emit_tlb_recheck`: LDAR/STLR,
    LDAXR, DMB/DSB, and the interpreter helper, which runs the A32
    backend's exclusives and barriers) and on every backward branch. On
    hardware the remapping core's TLB invalidation reaches every core
    before it goes on, barrier or not; guests also order by plain
    dependent loads, so fences alone left about one boot in 15 crashing.
    Until
    2026-11-16 a region kept its entries until it exited. A guest thread
    spinning in one region on another thread's signal then used a page
    that thread had unmapped and mapped again, through the old physical
    page, which by then belonged to another allocation. Tears of the
    Kingdom decommits and recommits heap pages constantly, and about one
    boot in four crashed in its loader on corrupted data. The
    interpreter, which walks the tables on every access, never crashed.
  - Before 2026-10-09 the cache was compiled out in multicore mode, so
    every access did the full walk; and each region started empty, so a
    tiny region walked on its first access to each page.
  - `tests/jit_tlb_test.c` remaps a page between two regions, serially
    and in multicore mode.
  - `tests/jit_tlb_fence_test.c` remaps a page from another host thread
    while an A64 region spins on a flag, by LDAR and by a plain load; each
    fails without its recheck.
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

- Integer Advanced SIMD is compiled inline to SIMD128: bitwise operations
  and permutes, three-same arithmetic (ADD/SUB, MUL/MLA/MLS, the
  compares and CMTST, S/U MAX/MIN, S/U ABD/ABA), two-register misc
  (ABS, NEG, compares with zero, NOT, CNT) and shifts by immediate (SHL,
  SSHR/USHR with the architecture's whole-lane shifts, SSRA/USRA), each
  for the lane sizes wasm has an instruction for. LD1/ST1 of one to four
  registers, one-lane LD1/ST1 and LD1R are inline too, checked as one
  access (the state's scratch holds four Q registers for page-crossing
  ones); LD2-LD4 still go to the interpreter. A fallback profile
  (`--jit-fallbacks`) of Nine Sols' load (Unity IL2CPP) had 80 M direct
  calls in 250k slices, 48 M of them structure transfers and 11 M a
  plain ADD.4S; with these inline, 15 M remain, and its load took 16%
  less CPU (Celeste 21%). Instructions whose result depends only on the
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
  - **The environment, once per function.** Scalar FADD/FSUB/FMUL/FDIV/
    FNMUL/FSQRT read FPCR and FPSR into a local (`L_FPENV`) when the
    function starts and again after every interpreter or direct call and
    whenever a fast arm sets IXC (MSR goes through the interpreter). While
    FPCR is as assumed and IXC is set, one branch on the result checks and
    that local leads to the fast arm: no exactness, no FPSR update. The
    IXC update itself is a branch, not a store in every instruction.
  - **Scalars in locals (`fp_cache`).** The low f32/f64 of V[n] is kept in
    a local after it is loaded or written; writes still go through to the
    state, so only the reloads go. Compile-time bookkeeping: a store into
    V[n] (any width, `mem` and the v128 stores) or a direct call forgets
    entries; an entry changed inside an if/block is forgotten at its else
    and end; region blocks start empty; an instruction the compiler gives
    up on restores the state it started with.
  - jit_bench's scalar FP loop: 1.8 -> 1.5 (environment) -> 1.19 ns per
    instruction (one branch, scalars in locals). Super Mario Odyssey in
    Cap Kingdom: 41-44 -> 45-46 fps, its busier scenes 31-33 -> 35-37.
  - **Recorded flags widen the fast arms.** Once FPSR already holds a
    flag, an operation that would raise it again changes nothing, so the
    native IEEE result is the architecture's whenever FPCR is 0:
    - vector FADD/FSUB/FMUL/FDIV with UFC and IXC set: any finite result,
      subnormals included (Silksong squares tiny lanes);
    - the same with IOC set as well: NaN lanes too, replaced by the
      architecture's NaN (a signalling n quieted, then m, then a quiet n
      or m, else the default NaN) - unused lanes holding NaNs sent every
      vector op of a Silksong run to softfloat;
    - single FMADD keeps its fast arm on an exact f32 midpoint when
      TwoSum shows the f64 sum exact (only a rounded sum can round
      twice); sums of floats of similar size land there often.
    Silksong's direct SIMD&FP calls: ~10M -> ~1M per 10 s. FCCMP, SSHL/USHL
    (32-bit lanes), (S|U)ADDL/(S|U)SUBL and SQXTN have fast paths too.
    The parallel jit report counts direct SIMD&FP calls and the last FPCR,
    and `VOLAND_JIT_FALLBACKS=N` lists what still goes through them (and
    classifies the vector single FMLA and FCMEQ/GE/GT calls by the guard
    their lanes miss: NaN, infinity, subnormal, tiny result, midpoint).
  - Under FZ a subnormal input lane is flushed to a signed zero (and
    raises IDC). `JIT_FLUSH_RETRY=1` gives vector FP exact arms a native
    retry on flushed inputs when FZ and IDC are set; off by default -
    it cut TOTK's helper calls ~75% but its cold code (+15% module
    bytes) cost more fps than the calls (2026-11-26). Flushing on the
    fast path itself lost too.
  - `jit_diff_test` puts FADD, MSR FPSR/FPCR, FADD in a row, and runs of
    scalar arithmetic among other writers of the same V registers (LDR,
    FMOV from a general register, vector FADD, INS, MOVI, short B.cond).
    Leaving out the refresh after the interpreter's call, the store hook
    or the v128 stores' forgetting fails it. The else/end/block-start
    rules and forgetting all after a direct call are not observable with
    today's emitters (every writer also stores in its arm); they stay as
    the invariant's defenses.
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
blocks is a local jump. The blocks nest in order inside the loop, so a
**forward** branch is a plain `br` to the end of the target's label,
where its code starts; only backward branches go through the `br_table`
(an indirect jump). That took 6.5% off Celeste's load under Node and
halved the BL/RET loop of `tests/jit_bench.c`.

**Branch hints.** Each module carries a `metadata.code.branch_hint`
section (the WebAssembly branch-hinting proposal) before its code
section: the translation-cache hit is likely, and the softmmu walk, the
budget exits, helper failures and the generation check's miss are
unlikely. V8 then moves the cold arms out of the hot path. Chrome honours
the section by default (Chrome 155 no longer has the flag); Node 24 needs
`--experimental-wasm-branch-hinting`. Celeste's load to slice 200k under
Node: 53.0 s without, 42.2 s with; in Chromium its main load phase went
from 62.5 to 49.2 s of worker time together with the changes above.
`JIT_NO_HINTS` in the environment leaves the section out. A `name`
section calls the region function `j<pc>`, so engine profiles (DevTools,
`node --cpu-prof`, `--perf-basic-prof`) tell regions apart; `JIT_NO_NAMES`
leaves it out.

What joins a region:

- **Direct branches**: B, B.cond, CBZ/CBNZ, TBZ/TBNZ, both arms of a
  conditional, and the fall-through after an instruction handed to the
  interpreter.
- **Calls into small callees.** A BL target joins the region if it is a
  leaf function or a PLT stub: within its first 4 instructions (16 for
  A32) it returns or branches away (RET or BR) without calling anything.
  So does a function that returns within 12 instructions and calls only
  such leaves. (Until 0.5.7 the A64 limits were 16 and 48: managed code's
  helpers such as an array's length with its GC poll, which Celeste calls
  170 M times while it loads, joined their callers.) The size counts the
  instructions a call runs, following unconditional branches, so a
  function that tail-jumps through shims qualifies: IL2CPP's GC write
  barrier (two moves, B, a store, B, the card-marking body) was Nine Sols'
  busiest region, 25 M entries in 420k slices. Following them took region
  entries down 4.9% (Nine Sols), 4.6% (Silksong) and 17.6% (Hollow Knight)
  in 150k Node slices, for 1-6% more module bytes; also allowing calls to
  any function in the 48-instruction form saved another 6% of Nine Sols'
  entries but cost 20% more module bytes and no time.
  `JIT_SMALL_CALLEE_INSNS=N` and `JIT_SMALL_CALLER_INSNS=N` set the two
  limits for A/B runs. Every spanned callee is one more copy of its code,
  and Unity's main threads are bound by instruction delivery (below), so
  smaller limits won in the browser on 0.5.6 (two runs each):

  | A64 limits | Hollow Knight fight / standing | Nine Sols | Silksong end | Celeste to 200 frames |
  |---|---|---|---|---|
  | 64/128, 64-block regions | 50.5-51.0 / 53.8-53.9 | | ~59 | |
  | 16/48 (until 0.5.7) | 51.5-52.7 / 54.4-55.9 | 43.9-44.0 | 59.1-59.3 | 33-35 s |
  | 8/24 | 53.0-55.1 / 55.8-56.8 | 44.4-44.8 | 59.4-59.5 | 33-35 s |
  | 4/12 (0.5.7) | 53.7-54.2 / 56.9-57.3 | 44.9-45.4 | 59.75 | 34 s |

  Super Mario Odyssey (C++, not IL2CPP) agrees: in New Donk City 8/24 gave
  47.6-48.0 fps against 48.1-48.6 at 4/12 (2026-11-23, three runs each). The
  return site joins too, and a RET goes back to an in-region return site
  through a guarded internal transition. Larger callees stay regions of
  their own, compiled once instead of into every caller.
- **Predicted PLT branches.** For an ADRP/LDR/BR stub, the branch target
  is read from the GOT at compile time. The prediction is guarded, so a
  stale GOT only costs an exit, never correctness.

Limits: 64 blocks per region (the environment variable
`JIT_MAX_REGION_BLOCKS` sets 1-128 for A/B runs), 128 instructions per block
(`JIT_MAX_BLOCK_INSNS`), and four code pages (`JIT_MAX_REGION_PAGES`),
each validated by its own range hash.
`voland-cli --jit-no-calls` turns call spanning off.

The block limit was 32 until 2026-11-25. Measured in the browser on the
mini, one browser profile per test, alternating, three pairs each:
- Super Mario Odyssey, where the game's main thread holds a core: 64
  blocks gave New Donk City 48.6-49.3 against 47.7-48.7 fps, Seaside
  51.2-51.7 against 48.5-51.0, Luncheon 50.6-50.7 against 49.3-50.3.
  Each won all three pairs.
- 128 against 64 was within noise (New Donk City 49.0-49.4 against
  48.3-49.5, Seaside 50.6-52.1 against 50.5-51.7).
- No other game got slower at 64: Silksong at Bellhart (57.4 against
  56.6 fps on average, noisy), Mario Kart 8 Deluxe races (57.4 against
  57.2), Tears of the Kingdom at Lookout Landing ~3 minutes in (12.5
  against 11.5).
- Smaller regions were slower: Hollow Knight's fight ran 50.2 fps at 16
  and 45.2-45.9 at 8, against 51.0-52.4 at 32.

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
   loop. The probe is unrolled over the four ways in every region;
   `JIT_SHARED_PROBE=1` makes it a module function each exit calls
   (smaller regions for one more call; A/B runs).

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

**Where the transitions come from** (`VOLAND_JIT_ENTRIES=1`, browser,
2026-11-10). Hollow Knight's main thread in a fight: 789 M exits, of
which 387 M returns (49%), 285 M direct branches and calls out of the
region, 117 M indirect branches (BR/BLR), next to nothing for the budget
or the interpreter. Silksong's busiest thread: 380 M of 796 M returns
(48%), 241 M branches and calls, 175 M indirect.

**Tried: calls that return.** A BL to a callee outside the region made a
wasm call (not a tail call) through a shared handler, with the return
address pushed on a small stack in `Jit_State`; a RET to the top of that
stack left with a "returned" status, and the caller reloaded its
registers and went on at the return site as a block of its own region.
It took Hollow Knight's main thread from 820 M to 634 M region entries
(178 M returns went back into their caller) and a large-callee loop in
`tests/jit_bench.c` from 0.41 to 0.25 ns per instruction - but the fight
ran about 8% slower in the browser (three runs each: fight 41-46 fps vs
47-50, every phase lower). The caller reloads every register its whole
region uses after each return (a small return-site region loads only its
own), each calling region carries the handler and its return-site
blocks, and 42 M calls never came back to their caller. Reverted. Tried
again on top of register residency (0.5.6, which makes the caller's
reload at most 8 registers): still 6-7% slower (fight 48.9-49.7 vs
51.0-55.0 fps, three runs each).

**Tried: predicted A64 virtual calls** (2026-11-27). Tears of the
Kingdom's busiest thread leaves its regions mostly by returns (47%) and
indirect branches (38%; Hollow Knight's: 15%), C++ virtual calls among
them. As the A32 front end does for BLX, the entry block's registers at
compile time, carried through `LDR Xt, [Xn, #imm]` and `MOV`, predicted
a BLR's target, and a small predicted callee joined the region behind a
guard with its return site. Region entries fell 7% (320.6 M to 298.4 M
on that thread; indirect exits 120.7 M to 110.0 M), but four alternating
browser pairs at Lookout Landing gave 13.73 against 13.63 fps (two of
four). A prediction also makes a region's bytes depend on the compiling
core's registers, so cores stop sharing one compiled module. Seeding
every block, not just the entry, with the registers no block writes (a
virtual call in a loop over one object: `ldr x8, [x20]; ldr x8, [x8,
#0x110]; blr x8` calling `mov w0, #4; ret` getters, ~2 M times each over
80k slices of its boot) cut entries only 8% in all. Reverted;
jit_diff_test keeps the virtual-call sequences (in the entry block and
in a later one).

**Where the time goes: the front end.** Hollow Knight's and Nine Sols'
main threads are each about one host core and run 3.9-4.8 ns per guest
instruction, against 0.7-0.9 for their loop-heavy audio and render
threads (per-thread host time from two `threads` dumps around a
measurement; their code has no hot spot: `VOLAND_PC_HIST=1`). Apple's
CPU counters (`xcrun xctrace record --template "CPU Counters" --attach
<renderer pid>`, table `MetricAggregationForThread`) split the cycles of
the workers that run guest code (2026-11-10, M4 mini, browser):

| | useful | delivery (front end) | processing (back end) | discarded |
|---|---|---|---|---|
| Hollow Knight, Crossroads fight | 27% | 44% | 20% | 9% |
| Nine Sols, first cave | 13-24% | 48-58% | ~20% | ~8% |
| Tears of the Kingdom, Lookout Landing (2026-11-27), each of 3 guest cores | 15-16% | 51-54% | 23-24% | ~8% |

(The GPU thread, C code, in the same TOTK trace: 59% useful, 12%
delivery.)

Instruction delivery - i-cache, iTLB, fetch - is the bottleneck, not
memory or execution: compiled regions are ~10-20 KB of machine code each
(`node --print-wasm-code`, Hollow Knight: median 10 KB), and a Unity main
thread runs through thousands of them per frame. That is why every
change that added code to regions lost even where it saved work, and why
register residency (less code at every entry and exit) won. Code size is
the lever.

**Node is not the browser here.** Node 24 (V8 13.6) bounds-checks every
memory64 access explicitly (about five instructions each); Chromium 149
uses guard regions and checks none, the 5.25 GB shared memory included
(same function: 104 vs 64 bytes of machine code). Under Node a load/store
loop takes 1.09 ns per instruction, in a Chromium worker 0.32; a region
transition costs about 14 ns in both, so in the browser transitions
weigh even more against straight-line code, and work that only saves
state loads and stores barely shows. Measure JIT changes in the browser
(perf.mjs), or run `tests/jit_bench.c` in a Chromium worker.

**What a transition is made of** (2026-11-10, Chromium 149 on an M1, a
two-module ping-pong through a shared table, one memory counter per hop):
the bare cross-instance `return_call_indirect` with a constant index takes
3.6 ns; the full region-cache probe (hash, way compare, generation, budget,
`referenced` store) adds 3.5 ns, but only 0.6 ns of that is the checks.
The rest is the call's index coming from a load: an index loaded from
memory or a global costs about 2.5 ns more than one computed from a
register, because the callee's instance (and its memory base, which its
first state load needs) is reached through that load. Hashing at compile
time for a constant target saved nothing (the hash is off the critical
path). Neither the shared probe nor JIT_LOCAL_REGS changes jit_bench's
transition cases. So a transition's floor here is ~4 ns and at most
~2 ns of the ~8-10 ns in jit_bench can still be cut (hoisting the probe's
loads ahead of the exit), about 1.5% of Hollow Knight's main thread at 27
guest instructions per entry: below what a game A/B resolves; not done.

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

**Hot threshold.** 8192 interpreted executions before compiling (since
2026-10-28; 4096 before). Against 4096: Tears of the Kingdom on three cores
in Node got 4% and 5.7% further in the same wall time (two runs) with ~30%
less compile and install time, and in the browser ran 10.33-10.47 fps vs
10.08-10.17 (3 runs each); Silksong's browser title (35-39 fps) and SSBU in
Node (same virtual time at 110k bursts) were unchanged; 16384 fell behind
4096. 4096 was
reverted to 256 for a day on 2026-10-27 when Super Smash Bros. Ultimate
crashed at its title with 4096; the same crash (a null fence in its movie
player's frame-status thread) then showed up at 256 on Node too, and its
cause was NVDEC completing instantly (DESIGN.md §13 note). With that
fixed, two Node 3-core runs at 4096 ran 200k bursts clean and reached 33%
more virtual time than 256 in the same wall time. A threshold of 16 compiled about 2.5× the modules of
256 for the same guest speed, and module compilation is the JIT's largest
overhead while games load. Tears of the Kingdom on three cores (Node, the
same wall time): 4096 got 19% further than 256 with 59% fewer modules and
no evictions at the live cap; 16384 got no further than 4096. In the
browser its gloom scene went from ~2.2 to ~3.7 fps and the 150 s average
from 8.0-8.3 to 8.6-9.1 fps; Silksong's title is unchanged (40-45 fps).
Hollow Knight's gameplay, where much of the main thread's per-frame code
stays under the threshold for minutes, still did best at 8192
(2026-11-10, browser, two runs each: walk 47.3/46.7 fps at 8192,
44.7/44.3 at 1024, 39.9/37.3 at 256; the fight 49.6/48.3, 47.0/46.2,
46.0/45.5) - compiled code cost more than interpreting it. Celeste's
load, mostly one-shot managed init code, too: 34/34 s to the title at
8192, 35/36 s at 2048, 35/35 s at 512 (2026-11-10, mini).
The cross-session hot list (below) did not change the answer: Super
Mario Odyssey's New Donk City with a warm player profile (a warm-up pass,
then the measured pass, each threshold on its own copy of the profile)
ran 48.7/48.1 fps at 8192 and 44.3/43.7 at 2048 (2026-11-24, mini),
though core 0 still spends ~9% in the interpreter there.
`VOLAND_JIT_THRESHOLD=N` overrides it (in the browser through
`?env=VOLAND_JIT_THRESHOLD=N`), for A/B runs without a rebuild.
`VOLAND_JIT_FALLBACKS=N` is the browser's `--jit-fallbacks`: every N
interpreted block starts it logs the most interpreted block PCs and
instruction encodings, then resets them. `VOLAND_JIT_ENTRIES=1` counts,
per guest thread, the regions entered (dispatcher and chained exits) and
the translation-cache misses, and the regions' exits by kind (a direct
branch out, a block that did not fit the budget, a call to a callee
outside the region, a return, an indirect branch, an instruction left to
the interpreter); the `[threads]` dump prints them under each thread.
Hollow Knight's busiest thread in a fight: 22.8 G instructions,
820 M region entries (one per ~28 instructions) and 2.29 G misses (one
per ~10).

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

## Hot code across sessions

Compiled modules cannot be stored (see "V8 quirks"), but the knowledge of
*what* got hot can. Every region entry the JIT installs is kept as a byte
offset into the process's code (the modules, mapped back to back from the
code base, so ASLR does not change it) in a fixed 2^17-slot set in
`jit.c` (`jit_hot_*`, at most 98,304 offsets). The CPU worker stores the
set in OPFS (`jit-hot/<title>-<key>.bin`, `workers/jit-hot-store.ts`)
every 30 s once it has grown; the key hashes every module's name and
build id and the code size, so an update or an exefs mod starts a new
list. When the title loads again the list is imported, and an imported
offset compiles the **first** time its PC is reached instead of after
`JIT_DEFAULT_HOT_THRESHOLD` interpreted runs. The mark is then cleared,
so a region that fails to compile, or is evicted later, goes back to the
threshold, and other cores find it through `g_compiled_somewhere` as
usual. Offsets outside the code region (ldr:ro's NROs) are not kept.

Why not simply a lower threshold: in Super Mario Odyssey's Mushroom
Kingdom (browser, steady state, every worker profiled) 4096 cut core 0's
interpreter time from 9.0% to 5.7%, but compiled code took 17% more
time per frame (15.6 against 13.3 ms; 3458 against 2956 regions
sampled) and the game ran at 50.7 against 55.6 fps. The extra regions
are lukewarm code, and this JIT is bound by instruction delivery (see
"Region transitions"). The hot list compiles exactly what an earlier
session found hot, only sooner.

Measured (browser, mini, 2026-11-23): Super Mario Odyssey at Fossil
Falls, one browser profile with its shaders cached, the first six Cappy
throws after loading (2 s windows each), three alternating pairs: 54.7 /
54.6 / 54.8 fps without the list, 57.0 / 57.3 / 57.3 with it (~10,000
regions imported); a minute later both hold ~59.
Tears of the Kingdom at Lookout Landing (same setup, 42-52k regions
imported, three alternating pairs, 20 s measures): ~1 minute after
loading 7.4 / 8.2 / 8.8 fps without the list, 11.9 / 10.8 / 9.6 with it;
~3 minutes after, 8.1 / 12.2 / 11.4 without, 12.3 / 14.2 / 14.4 with.
SMO's Mushroom Kingdom, where the main thread alone holds core 0 (19.6
of 20 s; ~10% of it interpreting warm code before): 60 s of play, three
pairs, 56.9 / 57.1 / 56.3 fps without the list, 59.5 / 59.8 / 59.7 with
it; 40-60 ms frames 95 / 67 / 108 against 62 / 71 / 38.

`VOLAND_JIT_HOT=0` (`?env=` in the browser) neither imports nor exports,
for A/B runs, and leaves the stored list as it is. `tests/jit_hot_test.c`
checks the list natively and, under Node, that an imported offset
compiles on first sight with the threshold far away.

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
  one-entry translation cache, and `JIT_TLB_VERIFY` walks the tables on
  every cache hit too and logs hits whose translation is no longer
  current (`[jit] TLB verify`, also in fault reports). In 25 Tears of the
  Kingdom boots after the recheck fixes it logged none, and one boot
  still crashed: that crash has another cause.
- **Declared locals are zeroed on every call.** Only the locals a
  region uses are declared.
- **Compiled modules cannot be persisted.** Chromium refuses to store a
  `WebAssembly.Module` in IndexedDB (`DataCloneError`). A cross-session
  cache would have to store the emitted bytes and compile them again,
  which saves the emit (small) but not V8's compile (DESIGN.md §11,
  "Persistence reality"). V8's own wasm code cache only applies to
  `compileStreaming` of HTTP-cached responses of 128 KB or more. What
  persists instead is the list of hot entry PCs ("Hot code across
  sessions").
- **Liftoff vs TurboFan.** Running Chromium with `--no-liftoff` was about
  5% faster at a title screen, within measurement noise, and a page
  cannot set engine flags anyway. The JIT does nothing special for
  tiering. In gameplay (Super Mario Odyssey's New Donk City, 2026-11-23)
  `--no-liftoff` was slower, 47.3-47.8 against 48.4-48.5 fps: Liftoff code
  does not linger in hot regions.

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
  - Advanced SIMD on wasm SIMD128 (Qn is V[n]): F32 VADD/VSUB/VMUL/VMLA/VMLS, vector and by scalar; VAND/VBIC/VORR/VORN/VEOR/VBSL/VBIT/VBIF; integer VADD/VSUB and VMUL.I16/I32; VEXT; VDUP (scalar and core); VMOV/VMVN/VORR/VBIC (immediate); VMOV to and from scalars; VLD1/VST1 of one lane and VLD1 to all lanes; VTRN/VUZP/VZIP and VTBL (byte shuffles and swizzles); F32 VPADD, VMAX/VMIN, VCEQ/VCGE/VCGT (register) and VCGT/VCGE/VCEQ/VCLE/VCLT #0 (native when every input is normal or zero: nothing rounds, flushes or raises), VCVT.F32.S32/U32 (IXC already set), VRECPS/VRSQRTS (AArch32's are unfused - FPRecipStep/FPRSqrtStep round the product - so single-precision wasm operations with VMLS's guards). Advanced SIMD floats always use the standard FPSCR value (flush to zero, default NaN), so the fast arm needs every input normal or zero, every result normal or an exact zero, and IXC already set;
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

## Versions

`JIT_VERSION` (jit.h) is shared by the A64 and A32 backends; the web UI
shows it next to the backend name.

- **0.1.0** - the A64 front end: regions, chaining, the inline softmmu walk,
  async compile, differential testing against the interpreter.
- **0.2.0** - FP/SIMD fast paths with exactness guards, call spanning and
  predicted PLT branches, multicore (per-thread caches, exclusives as CAS).
- **0.3.0** (2026-10-09) - the A32 front end (MK8DX): VFP and Advanced
  SIMD inline, A32 call spanning with PLT and vtable prediction; the
  translation cache carried across regions and kept in multicore mode.
- **0.4.0** (2026-10-19) - load speed (Celeste): direct forward branches,
  branch hints, small callers spanned, an SP translation entry and
  per-register slots.
- **0.5.0** (2026-10-21) - Unity titles (Nine Sols, Silksong): integer
  SIMD and LD1/ST1/LD1R inline, the shared softmmu walk by default,
  registers spilled and reloaded in pairs, separate code and translation
  generations. Under Node, CPU to the same slice: Nine Sols 345 -> 182 s
  (with raster3d's compute memo), Celeste 79 -> 57 s, Silksong 176 ->
  150 s.
- **0.5.1** (2026-10-21) - flush-to-zero: a region compiled under FPCR =
  FZ alone (TOTK) keeps the scalar and vector FP fast paths, guarded on
  that FPCR and on no subnormal input or result. TOTK under Node: direct
  SIMD&FP calls 751 M -> 77 M, 255 -> 211 s to the same slice.
  Store-exclusives need the same address and size in every mode.
- **0.5.2** (2026-10-25) - CRC32 and CRC32C (B/H/W/X) compile inline:
  unrolled byte-at-a-time table steps instead of a round trip through
  the interpreter. TOTK hashes its resource names with CRC32B; under Node
  to 250k slices its "other" interpreter fallbacks went 3.85 M -> 0.
- **0.5.3** (2026-10-25) - FZ fast arms accept FPCR 0 too (the
  not-subnormal checks make both exact), and once any thread has run with
  FZ every region compiles FZ-capable guards. TOTK mixes FZ and default
  threads over shared code, and a region compiled under the other mode
  sent every call to the exact arm. Also FRINT{N,P,M,Z,I} and
  FCVT{N,P,M}{S,U} have fast arms, and the fallback profile keeps SIMD&FP
  opcode bits 20:16. TOTK under Node to 250k slices: direct SIMD&FP
  calls 23.1 M -> 4.2 M. Its gloom-scene fps did not move (3.4), so
  those calls were not that scene's cost.
- **0.5.4** (2026-10-26) - `request_stop`: another host thread can end a
  run early (a better thread preempting it on parallel cores). The flag
  is checked by the dispatcher; the state's `cycle_budget` drops to 0 so a
  chain of compiled regions stops at the next region, and a region's
  entry room is clamped to 0 instead of wrapping when the budget is
  already behind.
- **0.5.5** (2026-11-09) - the softmmu walk is inlined again at each
  access (it was a shared module function since 0.5.0): Unity gameplay
  misses the translation cache about once per 10 instructions, and the
  call cost Hollow Knight's busiest fight ~3 fps. `JIT_SHARED_WALK=1`
  restores the call. `VOLAND_JIT_ENTRIES=1` counts region entries and
  cache misses per guest thread.
- **0.5.7** (2026-11-10) - smaller spanned callees on A64: leaves of up
  to 4 instructions and callers of up to 12 (were 16 and 48). Hollow
  Knight's fight +4-5%, Nine Sols +2-3% in the browser; loads unchanged.
- **0.5.6** (2026-11-10) - register residency: at most 8 guest registers
  (the region's busiest) live in locals, the rest in the state. Hollow
  Knight's fight +4-5%, Silksong +3-4% in the browser.
  `JIT_LOCAL_REGS=32` restores 0.5.5.
- **0.5.8** (2026-11-22) - TOTK's FP slow paths. The fallback profile
  (`VOLAND_JIT_FALLBACKS`) now also says why single-precision scalar FP
  reached the helper (FPCR, FPSR.IXC, input and result classes). At
  Lookout Landing: most ran under FPCR = FZ + DN (0x3000000), and the
  fz_safe guards took only 0 or FZ - DN changes only NaN results, which
  no guarded fast arm produces, so they accept it (and FZ + DN counts as
  FZ for compiling FZ-capable regions). Also: infinite results from an
  infinite input in FMUL/FADD/FSUB/FDIV/FNMUL/FSQRT (IXC set); FCMP with
  quiet NaNs (unordered, 0011); FSQRT of negatives and NaNs (default NaN
  once IOC is set; a quiet NaN raises nothing); vector FCM* compares,
  FCVT between single and double, scalar (SIMD form) SCVTF/UCVTF/FCVTZ*
  and vector SCVTF/UCVTF made FZ-safe; FCVT Sd, Hn compiles (it had no
  fast path). Browser, Lookout Landing, 6 alternating pairs: 8.20 -> 9.38
  fps (every pair faster; game speed 92-99% -> 99-100%), direct SIMD&FP
  calls ~29 M -> ~16 M per 10 s. What remains: FMUL overflowing to
  infinity, FCVTNS, FCVTAS/FRINTA, vector FCVTL, FMLA by element.
- **0.5.9** (2026-11-22) - the scalar FP tail TOTK still sent to the
  helper: FMUL/FNMUL/FADD/FSUB overflowing to infinity once OFC and IXC
  are recorded; ties-away rounding (no wasm instruction: trunc, then
  +-1 when the exact fraction is >= 0.5, selected so -0 stays -0) for
  FRINTA, FCVTAS/FCVTAU and the scalar (vector form) FCVT{N,M,A,P}{S,U}.
  Lookout Landing: single-precision helper calls per fallback report
  549 k -> 19 k; 4 alternating browser pairs 8.46 -> 9.20 fps (3 of 4
  faster - within this rig's noise). What remains is vector: FCVTL,
  FCVTN, FMLA/FMUL by element, FMAXP/FADDP. (0.6.3: FSQRT, the compares, FMAXP & co. and FCVTZS/U.)

- **0.6.0** (2026-11-23) - hot code across sessions: the region entries
  a title compiled are stored per title and code key, and compile on
  first sight in later sessions ("Hot code across sessions"). SMO's first
  Cappy throws after loading 54.7 -> 57.2 fps, its Mushroom Kingdom
  56.7 -> 59.7 fps; TOTK at Lookout Landing
  ~3 minutes in 10.5 -> 13.6 fps (means of three pairs).
- **0.6.1** (2026-11-25) - regions of up to 64 blocks instead of 32
  (see "Limits"). SMO's slowest kingdoms each won three of three pairs
  (New Donk City 48.2 -> 49.1 fps, Seaside 50.0 -> 51.4, Luncheon 49.9
  -> 50.7, means); no other game got slower.
- **0.6.2** (2026-11-25) - two translation-cache slots per region
  instead of four, and the fence recheck clears only the slots a region
  uses. New Donk City +1.2-1.8% (six of six pairs); other games even.
- **0.6.3** (2026-11-26) - more of TOTK's vector FP natively (it runs
  under FPCR = FZ; a profile at Lookout Landing had ~3% of each guest
  core in the helper's FSQRT alone): vector FSQRT (negative and NaN
  lanes as the scalar form); FCMEQ/FCMGE/FCMGT/FACGE/FACGT and the
  compares with zero take NaN lanes natively once IOC is set (FCMEQ's
  quiet NaNs always) - they compare false in wasm too; FMAXP/FMINP/
  FMAXNMP/FMINNMP (FADDP's shuffles, then FMAX's guard); vector FCVTZS/
  FCVTZU FZ-safe, and for singles NaN or out-of-range lanes once IOC is
  set (wasm's saturating truncation gives the architecture's results).
  jit_diff_test gained targeted sequences for each (its random streams
  never caught a broken NaN guard here: IOC is sticky and later code
  raises it anyway). Lookout Landing ~3 min, three alternating pairs
  each: FSQRT 12.75 -> 12.92 fps, the rest on top 13.18 -> 13.42.
- **0.6.4** (2026-11-26) - vector FMLA/FMLS (three same and by
  element) FZ-safe: their guard took FPCR 0 only, so under TOTK's FZ
  every one went to the helper; now subnormal inputs (n, m, the addend)
  go to the exact arm instead. ADDV, SMAXV/UMAXV, SMINV/UMINV natively
  (halving shuffles); LD2/ST2 (multiple structures) natively (interleave
  shuffles, stored inline or staged as ST1). `JIT_FLUSH_RETRY` (above).
  jit_diff_test: FPSR seeded with IDC, an FZ flush sequence, across-lanes
  forms, and LD/ST multiple offset forms with Rm = 0 (all were
  unallocated before). Lookout Landing helper calls 17-19M -> ~5M per
  20 s measure; fps 11.7 -> 12.7 and 10.8 -> 13.1 in two 4-pair runs
  (7 of 8 pairs) taken while the mini was busy with other work.
- **0.6.5** (2026-11-27) - FRECPE and FRSQRTE (single precision, vector
  and scalar), vector FABD, SQXTUN, SHLL, (S|U)ADDW/(S|U)SUBW and the
  scalar single FRECPS/FRSQRTS natively. The estimates: for a normal
  input the estimate's fraction depends only on input bits 22-15
  (FRSQRTE: also the exponent's parity), so a 256/512-entry table filled
  from the interpreter's own estimates gives it, and the exponent
  (FRECPE: and the sign) is arithmetic. Lanes
  that are zero, subnormal, infinite, NaN, negative (FRSQRTE) or whose
  estimate would be subnormal (FRECPE) take the helper. At Lookout
  Landing they were ~33k of ~420k helper calls per fallback report.
  Vector FABD (the top one, ~22k) takes FSUB's fast arms and its NaN-lane
  fix-up, then clears the sign bits (FPAbs clears a NaN's too). SQXTUN
  is wasm's unsigned saturating narrow (of signed lanes, as the
  architecture's), QC set as SQXTN's; SHLL an unsigned widen and a shift
  by the element size; the W forms widen m only. Together ~42k more.
  FRECPS and FRSQRTS (~31k) are FMSUB's fused f64 arm with 2.0 or 3.0
  as the addend, FRSQRTS halving the f64 sum (exact; the result is not
  tiny, so the f32 rounding is of the same bits); inf * 0 (2.0 or 1.5
  without IOC) is a NaN there and takes the helper.
  jit_diff_test: targeted sequences of the estimates on normal numbers of
  every exponent, and of FABD with IXC (or IOC, UFC and IXC) recorded and
  a negative NaN lane, of SQXTUN, SHLL and the W forms on mixed lanes
  with FPSR read back, and of FRECPS/FRSQRTS near 1 (and inf * 0). A wrong exponent bias in either estimate, a
  scalar form keeping the upper lanes, a dropped abs in either FABD arm,
  a signed narrow for SQXTUN, a wrong SHLL shift, a widened n for
  UADDW, and FRSQRTS unhalved, the two constants swapped or n*m added
  were each caught; the random streams caught none of them. Lookout
  Landing, four alternating pairs against 0.6.4: direct SIMD&FP calls
  ~5.4 M -> ~3.7 M per 20 s measure, but 13.78 -> 13.68 fps (no change:
  the helper is no longer where TOTK's time goes).

## What a region costs on V8

From V8's arm64 output (`node --no-liftoff --no-wasm-lazy-compilation
--print-wasm-code` on a module `--jit-dump` wrote):

- Straight-line guest code compiles to about five host instructions per
  guest instruction: a byte-scanning loop's `add`, `tst`, `b.eq`,
  `ldrb`, `cbz` became ~26, counting the per-block budget check, the
  flags materialized at block ends, the translation-cache check and V8's
  own bounds check.
- Every access to the state (a guest register in a prologue or an
  epilogue, a TLB entry, the cycle counters) costs ~7 instructions: the
  memory64 bounds check builds a different 34-bit constant per offset
  (movz/movk/movk), compares and branches before the add and the load.
  Registers move in pairs for that reason, but measured gains were small
  (~1%): the checks predict well.
- A region's machine code is large (tens of KB for a few hundred guest
  instructions), mostly out-of-line paths. Silksong's load enters a
  region every ~30 guest instructions (289 M entries in 300k slices, 8.8
  G instructions), and each entry pays the prologue (~19 state loads on
  average), stack spills and the 4-way chain probe.

## Limits and next steps

- Compiled modules are one function per region; batching several regions
  per module would cut per-module overhead during loads, but not memory:
  in Chrome a region costs ~30 KB per host thread at TOTK's sizes, and a
  synthetic module of similar wasm size only ~6 KB (8 per module: no
  less) - the cost is the machine code real regions compile to.
- SIMD&FP instructions without a fast path still run through the
  interpreter's softfloat.
- Compiled code does not persist across sessions (see "V8 quirks"); only
  the list of what was hot does ("Hot code across sessions").
- In parallel mode each core compiles into its own cache, which
  multiplies compiles and evictions. Sharing the compiled code is
  possible because the bytes are identical, but the tables are per
  thread.
