# Bot 3 status: WebGPU renderer quality and coverage (branch `local/bot3`)

## Done

- **Replay tooling for gameplay-sized streams.**
  - `tools/replay-gpu-stream.mjs` now streams the file in chunks; 3.2 GB replays work. `--at P1,P2,...` captures given presents, and frames are written as they arrive.
  - `voland-cli --gpu-stream F --dump-frames-every N` logs `gpu-stream slice S present P`, which maps slices to presents for `--at`.
- **Gameplay verification (in progress).**
  - The Silksong recipe ran to 4.8M slices in GPU mode: a 3.2 GB stream with 250,416 draws, 5,848 presents and 41 pixel programs. All 46 dumped WGSL modules validate, with 0 untranslated draws and 0 producer warnings.
  - The full replay on Apple Metal takes 20.1 s for 5,848 presents (3.4 ms per frame), with 0 WebGPU errors.
  - Software reference vs replay, every 50k slices: so far (slices 0–900k, title screen), mean ≤ 0.05/255 and max 4/255 per frame. Gameplay frames are pending (the software run is slow).
- **Mipmaps (DESIGN §13, stated deviation; stream version 2).**
  - TEXTURE_CREATE carries mip levels. The executor builds a chain with a 2x2 box filter after uploads.
  - The WGSL picks the level per pixel: from derivatives in uniform control flow, from an explicit LL, or level 0 for LZ. It applies the TSC LOD bias and min/max clamps.
  - The hardware sampler gets the TSC min and mip filters. `VOLAND_GPU_MIPMAPS=0` disables all of this.
  - With one level, the output is bit-identical to version 1: the title replay with mips on is pixel-identical to the one without.
  - New `texture_lod` WebGPU vector, plus structural LOD checks in `wgsl_test`.
  - Silksong barely uses mip chains: one 32x32 texture at the title.

- **Executor efficiency.**
  - Full clears are deferred into the next pass's load operations.
  - Same-format, same-size COPY records become `copyTextureToTexture`.
  - Draw bind groups are cached, with the draw data at a dynamic offset (2 MB window).
  - Gameplay replay: 101,972 → 78,300 render passes. All 97 captured frames are byte-identical to the old executor's.
  - Executor CPU time per record type (whole replay): presents 983 → 590 ms, uploads 976 → 211 ms, copies 54 → 16 ms, draws 1,269 → 1,235 ms. That is about 0.38 ms per frame in total, so the GPU worker is not a bottleneck.
  - Upload batching was not worth doing: only 74 submits beyond one per present in 5,848 frames.

- **Unsupported paths.**
  - SHFL with register lane operands is translated (a lane outside the quad reads its own value, predicate as the reference's).
  - Quad reads are bit-exact: 16-bit halves through derivatives. Before, they were float arithmetic, lossy for integer data.
  - Integer-format COPY: equal formats use a texture copy, the others an integer blit.
  - Integer clear colours are masked and sign-extended to the channel width, as `encode_color` does.
  - New `shfl` WebGPU vector (9/9). The synthetic integer clear/copy stream replays with 0 WebGPU errors.

## Measurements

| What | Value |
|---|---|
| Gameplay stream replay (Metal), 5,848 presents | 20.1 s, 3.4 ms per frame, CPU side included |
| Executor passes for 250k draws | 101,972 |
| Producer at the title | negligible in a host profile; the ARM interpreter dominates |

## Next

1. Finish the gameplay frame comparison and fix any mismatch.
2. The unsupported-path list (register SHFL, global loads, integer copies, cube/3D). Silksong's gameplay hits none of them so far.
3. Producer texture re-hashing: **the coordinator took it over** (texture_load, hashing, texture cache, ASTC). My unfinished attempt is in `~/WORKSPACE/bot3-runs/texture-change-detection.diff`.

## Needs from others

Nothing yet.
