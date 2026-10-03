# Profiling tools

Scratch-grade tools that turn Voland's profiling output into answers about
*guest* code. Our own code; they need `clang` with the AArch64 target and
`llvm-objdump` (set `LLVM_OBJDUMP` if it isn't on `PATH`, e.g.
`/opt/homebrew/opt/llvm/bin/llvm-objdump`).

## Inputs (voland-cli, native)

| Variable | Gives |
|---|---|
| `VOLAND_PC_PROFILE=1` | hottest 256-byte guest code blocks at exit (cycles per slice-stop PC) |
| `VOLAND_PC_PROFILE_FROM=N` | profile from slice N on |
| `VOLAND_STATS_FROM=N` | from slice N: virtual time executed vs idle jumps, per-thread share, SVC rates, SleepThread return sites |
| `VOLAND_PC_SAMPLES=FILE` | with `VOLAND_STATS_FROM`: stop PCs of budget-ended slices (an unbiased sample of executed instructions) |
| `VOLAND_DUMP_MODULES=DIR` | each module's `.text` + `modules.txt`, for offline disassembly |

A slice that ends at an SVC charges its cycles to the SVC site, so blocks
with many short slices in the PC profile are call sites, not hot code.

## Tools

- `guest_disasm.py DIR module+0xOFF [BEFORE AFTER]`: disassembles the dumped
  `.text` around a profile block, with real module offsets.
- `insn_mix.py DIR samples.bin [slot,slot...]`: instruction-class mix of
  `VOLAND_PC_SAMPLES`, overall and per thread slot.
- `categorize-cpuprofile.js FILE.cpuprofile`: buckets a browser CPU-worker
  profile (`platform/web/tools/perf.mjs --profile`) into JIT-compiled code,
  JIT helpers, interpreter fallback, Maxwell shader interpretation, the
  raster3d producer, HLE and other.

Example (Silksong gameplay, see docs/handoff/BOT_2_STATUS.md):

    VOLAND_STATS_FROM=4800000 VOLAND_PC_SAMPLES=s.bin VOLAND_DUMP_MODULES=mods \
      voland-cli run GAME.nca --gpu-stream /dev/null --max-slices 5000000 --input ...
    tools/profiling/insn_mix.py mods s.bin
