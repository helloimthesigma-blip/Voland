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

## Next

- Native vs. browser on the same slice span (the milestone 1 ratio).
- `-flto` A/B (built, not measured yet); wasm-opt variants.
- Per-slice overhead in `emulator_run_slice` (device updates every slice;
  about 9,000 slices/s now).
- GPU stalls during gameplay, not just at the title screen.

## Needs from others

- None yet.
