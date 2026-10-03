# Bot 3: WebGPU renderer quality and coverage

Read `/BOTS.md` first; its rules apply. Worktree `../Voland-bot3`, branch
`local/bot3`, status file `docs/handoff/BOT_3_STATUS.md`.

## Background

The WebGPU renderer was finished today by the renderer agent (the
coordinator). Read DESIGN.md's v3.70 → v3.71 changelog,
`docs/GPU_COMMAND_STREAM.md`, `core/gpu/wgsl.{h,c}`, the GPU-mode section
of `core/gpu/raster3d.c` (search for "GPU mode"), and
`platform/web/workers/gpu-executor.ts`.

**How it works.** The CPU side runs vertex programs, assembly, clipping
and culling, then streams screen-space triangles and WGSL-translated pixel
programs to the GPU worker.

**Tools.**

| Tool | Purpose |
|---|---|
| `tests/wgsl_test.c` + `platform/web/tools/wgsl-vectors.mjs` | Differential vectors: translator vs the interpreter, on WebGPU |
| `voland-cli run X --gpu-stream F.vgs` + `node tools/replay-gpu-stream.mjs F.vgs OUT --adapter default` | Record a stream and replay it; writes `OUT.last.ppm` (`--every N` for more) |
| `tools/validate-wgsl.mjs DIR` | Compile a directory of `.wgsl` files |
| `VOLAND_DUMP_WGSL=DIR` | Dump every translated pixel program |

Silksong's title screen matches the software reference (mean difference
0.02/255).

## The task, in priority order

1. **Gameplay verification.**
   - **Already recorded:** a GPU stream to 4.75M slices with the recipe is at
     `$SCRATCH/gp/gpu.vgs`. A software run to the same point writes
     `$SCRATCH/gp/sw.ppm` when it finishes (`sw.log` beside it says
     "running after 4750000 slices"). Use these instead of re-recording, and
     delete `gpu.vgs` when done.
   - Record a GPU stream of Silksong into gameplay (the BOTS.md recipe to
     about 4.75M slices with `--gpu-stream`). It's about 2–3 GB, so delete
     it afterwards.
   - Replay it and compare against a software-mode `--dump-frame` at the
     same slice count (deterministic). Use the CLI `--dump-frames-every`
     plus the replay `--every` to compare several frames.
   - Extract the SHADER records from the stream and validate them all.
   - Fix every mismatch: missing instructions, texture formats, blend or
     stencil states, copies, presents.
2. **Unsupported paths that currently drop or approximate:**
   - SHFL with register operands.
   - Global memory loads in pixel programs.
   - Integer-format copies.
   - Partial clears of integer targets.
   - Depth textures sampled with compare (verify).
   - Cube maps.
   - 3D textures.
   Add a vector to `wgsl_test.c` for each fix.
3. **Mipmaps.** Textures upload level 0 only, as the reference does.
   - On a real GPU, minified textures shimmer. Upload or generate the mip
     chain and pick the LOD with derivatives (`textureSampleLevel` with a
     computed level is fine in uniform control flow; straight-line
     programs have no dispatch loop).
   - This is a deliberate improvement over the reference: write it down as
     a deviation in DESIGN §13.
4. **Executor efficiency.**
   - Silksong uses about 15 render passes per frame for 16 draws: merge
     passes when only the depth attachment differs.
   - Cache bind groups.
   - Texture uploads currently flush the batch: batch uploads per frame.
   - Measure with the replay tool (`--adapter default`).
5. **CPU-side producer costs** (they matter once the JIT lands):
   - Decoded textures are re-hashed every frame (`texture_load`'s
     validation epoch). Big atlases (64 MB) cost real time in wasm.
   - Cheaper change detection, for example write-tracking of the guest
     pages backing textures, would help. Coordinate in your status file if
     you need vmm hooks.

### Files

- **You own:** `core/gpu/wgsl.{h,c}`, `tests/wgsl_test.c`,
  `platform/web/workers/gpu-executor.ts`, `platform/web/bindings/gpu-records.ts`,
  the `platform/web/tools/*gpu*` and `*wgsl*` tools, and the GPU-mode
  section of `core/gpu/raster3d.c` (the part after
  `/* ---- GPU mode`).
- **You may touch, with care:** the rest of `core/gpu/raster3d.c`,
  `core/gpu/texture.{h,c}`, `core/gpu/gpu_records.h` (bump
  `GPU_STREAM_VERSION` and update `docs/GPU_COMMAND_STREAM.md` on any
  record change), and `platform/web/workers/gpu.worker.ts`.
- **Do not touch:** the CPU backends, the scheduler, nvdec/video (bot 2),
  build flags (bot 4).
