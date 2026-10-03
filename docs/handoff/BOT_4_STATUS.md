# Bot 4 status: browser-side performance

Branch `local/bot4`, worktree `~/WORKSPACE/Voland-bot4`. The task is in
`BOT_4.md`.

## Results so far (Silksong title screen, Chromium on the real GPU)

These are measured with `platform/web/tools/perf.mjs`: warm up to slice
about 860,000 (the title screen), then measure for 60 s. The machine was
heavily loaded (load average 15–20, six agents), so treat absolute numbers
as ±10%.

| Build | fps | slices/s | virtual ticks/s | worker time yielding |
|---|---|---|---|---|
| `local/dev` 2dd8000 (baseline) | 4.97 | 5,855 | 1.59 M (8.3% of real time) | 28.8% |
| + run-loop yield, read-ahead, quiet log | **7.73** | 9,122 | 2.47 M (12.9%) | 0.2% |

Reaching the title screen also got faster: 244 s → 164 s of worker time.

### What changed

1. **Run-loop yield** (`cpu.worker.ts`). Between 12 ms bursts the worker
   used `setTimeout(runBurst, 0)`, which nesting clamps to about 4 ms: 29%
   of the worker's time was idle. It now yields with a data-less message
   on a private `MessageChannel`, which is the next task, unclamped.
   Lifecycle messages and timers still interleave. A `burstPending` guard
   keeps one burst chain alive however often load and resume ask for one;
   before, a reload could start a second chain.
2. **Game-file read-ahead** (`cpu.worker.ts`). Reads go through a 32 MiB
   LRU of 1 MiB chunks. Early in the boot, file reads took 12% of worker
   time; each `FileReaderSync` + `Blob.slice` costs a fixed amount, and
   the core reads RomFS in about 25 KiB pieces. At the title screen there
   are no reads, so this helps loading only.
3. **Logging** (`core/common/log.c`). The web build defaults to INFO. Each
   line goes out in one `emscripten_err` call instead of Emscripten's
   char-by-char `printChar` (1.5% of the worker at the title screen; the
   boot logs about 37,000 DEBUG lines). Native builds are unchanged.
4. **Measurement plumbing.**
   - `emulator_perf_counters_ffi` (`wasm_entry.c`) reports ticks, SVCs,
     GPU stream bytes and GPU stream stalls.
   - The worker keeps run-loop counters on
     `globalThis.__VOLAND_CPU_PERF__`; the harness reads them, and nothing
     posts them.
   - `-DVOLAND_WASM_PROFILING=ON` adds `--profiling-funcs`, so profiles
     show wasm function names.

### Browser profile at the title screen (after the fixes, 15 s, self time)

```
27.0% interp_run          4.2% op_load_unsigned   2.4% op_reference
14.3% interp_execute      3.5% add_impl           2.2% interp_read
 5.8% mul_impl            2.9% fp_three_same      2.2% complete_load
                          2.5% by_element         2.1% round_value
                          1.9% op_logical_shifted 1.7% texture_load
                          1.8% op_add_sub_shifted 1.7% fp_div
```

Nearly all of it is guest ARM interpretation, which belongs to the JIT
agent. `mul_impl`, `add_impl`, `round_value`, `fp_div` and `by_element`
are softfloat/NEON helpers. `texture_load` (GPU producer) is 1.7%.

### Facts checked

- Bulk memory and non-trapping fptoint are already on (emsdk 6 default):
  the module has 272 `memory.copy` and 66 `trunc_sat` ops.
- The `Release` web build already compiles and links at `-O3`;
  ASSERTIONS=0.
- GPU stream stalls at the title screen: 0 (2.3 MiB/s streamed). The
  4 MiB ring is not a bottleneck there.

### Gameplay (slice 4.8M, BOTS.md input recipe through `perf.mjs --press`)

| Build | fps | slices/s | ticks/s | GPU stalls |
|---|---|---|---|---|
| bot4 e997fce | **0.78** | 439 | 0.27 M (1.4% of real time) | 3,298 in 162 s (18 MiB/s streamed) |

The CPU worker profile in gameplay (15 s, self time):

```
56.5% texture_load   17.0% ise_decode   7.7% interp_run   3.1% interp_execute
```

**73% of the CPU worker in gameplay is the GPU producer's texture path**
(`core/gpu/raster3d.c`): `texture_load` re-hashes every used texture every
frame (`texture_epoch` bumps in `raster3d_end_frame`; `guest_hash` and
`content_hash` are inlined into `texture_load`), and ASTC decoding
(`ise_decode`) runs continuously. The interpreter is only about 11% there.
Fixing this is worth up to about 3.7× in gameplay before the interpreter
becomes the wall again. It is bot 3's "Next 3" and the coordinator's code;
see "Needs".

The GPU stalls cost little: no wait function shows in the profile.

**Native at the same point is different.** The CLI ran from a snapshot at
slice 4.8M (GPU mode) and `sample` took 15 s of 50k slices, about 1,600
slices/s wall time on the loaded machine:

- `texture_load` is about 4% of non-idle samples, and `ise_decode` does not
  appear at all.
- The likely cause is `RASTER_TEXTURE_POOL_BYTES`: 512 MB under
  `__EMSCRIPTEN__` vs. 1 GB natively, against Silksong's decoded working
  set of about 500 MB (the comment in `raster3d.h`). In the browser the pool
  evicts and ASTC is re-decoded every frame.
- The coordinator (session voland-34) took the texture-cache fix on
  2026-10-03 and has been sent this.

### Other checks

- **Native vs. browser, same span** (slices 860k → 1.41M, title screen,
  `--budget 200000 --gpu-stream /dev/null`, CPU time):
  - Native: 35.5 s for 550k slices and 149.1M ticks, about 15,500 slices/s
    and 4.2 M ticks/s.
  - Browser after the fixes: 9,100 slices/s and 2.47 M ticks/s.
  - The browser is about **1.7× slower than native**. Boot to the title:
    87.5 s native vs. 164 s in the browser (1.9×).
- **Native `sample` at the title** vs. the browser profile.
  - The interpreter's dispatch core is about 27% of non-idle native samples
    (`interp_predecode_execute` + `interp_execute`) but about 41% in wasm
    (`interp_run` + `interp_execute`).
  - The helpers (`mul_impl`, `add_impl`, `fp_three_same`, `by_element`) are
    in similar proportion.
  - So the wasm penalty sits in the dispatch loop (indirect calls through
    the op table), which is the JIT agent's area.
- **memory64 bounds checks don't dominate.** A load/store micro-benchmark
  in V8 13.6 (Node 24): memory64 255 ms vs. wasm32 297 ms (best of 12).
  No action.
- **`-flto`:** 7.72 vs. 7.73 fps at the title. No gain; not kept.
- **V8 tiering:** `--js-flags=--no-liftoff` gave 8.10 vs. 7.73 fps (+5%,
  within noise on this machine). A page cannot set it anyway.
- **Per-slice overhead** in `emulator_run_slice` (hid/vi/audio/nvdrv
  updates every slice), `scheduler_tick` and the FFI together are under 1%
  of the worker. Nothing to gain there.

### After the coordinator's texture fix (local/dev 8d4eb35 merged into bot4 ccb7fcf)

Same harness and recipe. Load average was 40–50 during these runs, worse
than before.

| Scene | Host cores | fps | slices/s | ticks/s |
|---|---|---|---|---|
| Gameplay (4.8M) | serial (`?cores=0`) | **3.12** (was 0.78) | 1,620 (was 439) | 1.00 M (5.2% of real time) |
| Gameplay (4.8M) | 2 (`?cores=2`) | **3.27** | 2,084 | 1.04 M |
| Title (860k) | serial | 7.00 | 8,268 | 2.24 M |
| Title (860k) | 2, 60 s window | 2.82 (see below) | 6,662 | 2.45 M |
| Title (860k) | 2, 120 s rerun | **8.43** | 9,622 | 2.70 M |

- The gameplay screenshot (`--shot`) shows Hornet in the first room, so
  this is real gameplay.
- Serial gameplay profile: `interp_run` 29%, `interp_execute` 12%,
  `round_value` 5%, `texture_load` gone from the top. Gameplay is
  interpreter-bound now (the JIT agent).
- With 2 cores the CPU worker's own profile is 97.5%
  `emscripten_futex_wait`: it drives slices while the guest threads run
  on pthreads.
- Boot to gameplay: 1,588 s worker time serial vs. 1,309 s with 2 cores
  (−18%).
- **One-off:** in the first 2-core title run, presents stopped (fps 0) for
  the last 36 s of the window while ticks kept advancing and one 0.4 MiB
  file read happened. A 120 s rerun at the same point never dropped. With
  host cores, slice counts aren't deterministic, so this may be the game
  doing something else (a load) rather than a hang. Watch for it.

## Web JIT in the browser (local/dev 85d887f)

- **Title fps.** `web-jit` gave 11.1–12.1 fps across five runs (13.2k–14.3k
  slices/s), against 7.0–7.7 for the interpreter, so about 1.55×. One 7.31
  outlier came during a load spike (load average 184).
- **Node, same build, 200k slices from boot.**
  - Interpreter 480 s, JIT 279 s: 1.72×.
  - Identical virtual time and SVC count.
- **Where the worker's time goes at the title.**
  - JIT modules: 49%.
  - Core wasm: 46%, of which interp_execute 9–12%, softfloat/NEON helpers
    about 22%, jit_helper_simd 2–5% and jit_run 2.6%.
  - Run-time compilation (Module + Instance): 2–3%.
  - JS boundary: about 3.5%.
- Sent to the JIT agent.

## Load time (task from the coordinator, 2026-10-03)

The harness gains `--phases NAME:SLICE,...`. It records a CDP profile per
phase, plus worker time, file reads, GPU bytes and stalls, and counts of
run-time wasm modules. The counts come from a `WebAssembly.Module`/`Instance`
wrapper installed in the worker.

**Before** (local/dev c87a499, JIT core, the BOTS.md recipe; two runs on a
heavily loaded machine):

| Phase (slices) | Worker s, run A | Worker s, run B | File reads (MiB, s), run B | wasm modules (MiB, s compiling) | GPU stalls |
|---|---|---|---|---|---|
| boot (0–860k) | 86 | 201 | 1,213 (334 requested, 20.5 s) | 82,931 (335, 21.2 s) | 154 |
| title (860k–1.1M) | 24 | 34 | 1 | 10,516 (45, 2.5 s) | 106 |
| menus (1.1M–3.0M) | 209 | 208 | 354 (91, 1.9 s) | 74,414 (278, 22.4 s) | 187 |
| New Game → room (3.0M–4.8M) | 505 | 348 | 9 | 98,667 (387, 59.0 s) | 869 |
| **total** | **824** | **791** | | **266,528 (1,045 MiB, 105 s)** | |

The interpreter took 1,309–1,588 s to the same point.

- **Biggest non-guest cost: the JIT's run-time compilation.**
  - About 266k modules averaging about 4 KB, even at the steady title.
    That suggests recompilation churn.
  - In the New Game phase, "Module" is 14% self time and GC 3–4%.
  - The JIT agent's code; sent to them with suggestions.
- **File reads.** 1,213 synchronous 1 MiB chunk reads cover 334 MiB of
  requests at boot. They cost 3.4 s on a quiet disk and 20.5 s under
  contention.
- **Negligible:** logging, `texture_load` (about 1%) and RomFS/LZ4 parsing
  (not in the top 25).

**After** (bot4 dbd7037 + this commit; JIT core, `?cores=0`, quieter
machine; the screenshot shows the first room):

| Phase (slices) | Worker s | File reads (MiB, s) | wasm modules (MiB, s compiling) | GPU stalls (wait s) |
|---|---|---|---|---|
| boot | 103 | 1,822 × 256 KiB (334 requested, 2.5 s) | 85,417 (357, 8.7 s) | 127 (0.0) |
| title | 62 | 101 (22, 0.2 s) | 29,069 (123, 5.9 s) | 111 (0.1) |
| menus | 138 | 255 (69, 0.5 s) | 62,879 (260, 13.2 s) | 196 (0.0) |
| New Game → room | 277 | 17 (4, 0.1 s) | 105,492 (446, 42.8 s) | 719 (0.4) |
| **total** | **580** | **3.3 s** | **282,857 (1,186 MiB, 70.6 s)** | **0.5 s** |

- **File reads.** The cache is now a 128 MiB LRU of 256 KiB chunks, up
  from 32 MiB of 1 MiB. At boot the old cache read 1,213 MiB for 334 MiB
  of requests, because the boot re-reads the same RomFS regions and they
  were evicted. Now it reads 371 MiB (1,486 distinct chunks), with almost
  no re-reads. Under similar disk contention boot reads take 6.5 s, down
  from 20.5–27.4 s; on a quiet disk, 2.5 s.
- **Async read-ahead tried and dropped.** `Blob.arrayBuffer()` for the next
  4 chunks after a miss made boot worse: 4,633 prefetches, more
  synchronous reads, 27.4 s.
- **GPU ring size stays at 4 MiB.** A new counter
  (`emulator_stream_wait_ns`, the time spent in `gpu_stream_wait`) shows
  only 0.5 s of waiting across the whole load, despite 1,153 stalls. The
  coordinator said to skip the resize if stalls cost nothing.
- **Worker time to gameplay is 580 s here vs. 791–824 s before.** Part of
  that is the read fix, and part is a less loaded machine: JIT compile
  time also fell, 105 → 71 s, with no JIT change.
- **What remains is the JIT's own compile cost.** About 283k modules and
  71 s. The JIT agent has since raised the hot threshold to 256 and reuses
  one staging buffer (on `local/jit`); batching regions per module is next
  on their list.

**After the JIT agent's threshold-256 / small-callee / staging-buffer
changes** (bot4 4befdf9 = local/dev 0c5d993, which contains everything in
c7ac43c; JIT core, `?cores=0`):

| Phase (slices) | Worker s | Virtual s | File reads (s) | wasm modules (MiB, s compiling) | Stream waits (s) |
|---|---|---|---|---|---|
| boot | 71.0 | 20.0 | 2.2 | 36,995 (218, 3.8 s) | 0.0 |
| title | 35.9 | 8.3 | 0.2 | 8,757 (52, 1.7 s) | 0.1 |
| menus | 104.7 | 34.6 | 0.6 | 10,408 (55, 2.4 s) | 0.0 |
| New Game → room | 156.8 | 41.3 | 0.1 | 18,141 (102, 5.5 s) | 0.4 |
| **total** | **368.5** (was 580, and 791–824 before that) | 104 | 3.1 | **74,301 (426 MiB, 13.4 s)** (was 283k, 71 s) | 0.5 |

- **Gameplay right after: 11.10 fps** (12k–13k slices/s; 6.7k over the
  measured window, which includes a dip). With the interpreter it was
  3.12.
- **What remains in menus and New Game** (CDP self time):
  - JIT-compiled guest code: 77–79%.
  - `jit_run` 2.4%, `interp_execute` 2%, FP helpers (`round_value`,
    `exact_add`, `fp_sqrt`, `fp_misc`) about 3%, `jit_helper_simd` 0.5%.
  - Run-time compiles ("Module"): 2.2–3.4%.
  - GPU producer: `texture_load` 1.4%, `run_vertices` 0.9%, `sm_run`
    (Maxwell vertex programs) 1.1%.
  - `scheduler_tick` 0.7%, `vmm_read_block` 0.5%.
  - The worker's JS (`runBurst`) 0.5%; idle under 1%; GC 0.1%.
  - Load time is now guest execution. The virtual-time phase lengths come
    from the scripted recipe's waits; the emulator runs at 26–33% of real
    time during them.
- **Caching compiled code across sessions (coordinator's idea):**
  - Chromium 149 cannot store a `WebAssembly.Module` in IndexedDB
    (`DataCloneError: A WebAssembly.Module can not be serialized for
    storage`; probed). A cross-session cache would have to store the
    emitted bytes (OPFS/IndexedDB, keyed by guest-code hash + JIT version)
    and compile them again. That saves the emit, which is small, but not
    V8's compile.
  - V8's own wasm code cache works only for `compileStreaming` of
    HTTP-cached responses of 128 KB or more, and only for TurboFan-tiered
    code. It would need the cached regions batched into one big module
    served through the service worker's Cache Storage. I have not verified
    that it hits.
  - At 13.4 s (3.6% of load), the bigger, simpler win is **asynchronous
    compilation**: `WebAssembly.compile` runs on V8 background threads, the
    block keeps interpreting until its module resolves between bursts, and
    the worker never blocks. Proposed to the JIT agent.

## Next

- Re-measure load phases once the JIT's threshold and staging changes are
  on `local/dev`.
- Gameplay is guest-code bound. The browser-side levers left are small.
- wasm-opt / clang flag variants for the texture and hash loops.

## Needs from others

- **Bot 3 / coordinator (`core/gpu/raster3d.c`): texture re-validation is
  73% of the CPU worker in gameplay** (see above).
  - Ideas: re-validate a texture only when its guest pages were written.
    The softmmu or a write-tracking bit could flag dirty pages, as
    `raster3d` already does for render targets. Or hash a sample of the
    texture each frame and the whole of it only every N frames.
  - Also: why ASTC keeps decoding in steady gameplay. Hash changes? Cache
    thrash at 256 entries?
  - Measure with
    `node platform/web/tools/perf.mjs --game NCA --warmup-slices 4800000 --press ...`
    (the recipe is in the tool's header comment).
