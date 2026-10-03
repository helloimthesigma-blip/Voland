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

## Next

- Re-measure gameplay once the coordinator's texture fix lands on
  `local/dev`.
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
