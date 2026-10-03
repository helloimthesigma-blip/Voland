# Bot 2: video playback (cutscenes render black)

Read `/BOTS.md` first; its rules apply. Worktree `../Voland-bot2`, branch
`local/bot2`, status file `docs/handoff/BOT_2_STATUS.md`.

## Why

Silksong's opening cinematic, and presumably the other videos, play black:
there's no video decoding. The owner noticed.

## The task

1. **Find out how Silksong plays video.**
   - Unity's VideoPlayer on Switch uses the platform's hardware decoder.
     That is usually the `nvhost-nvdec` device through nvdrv (ioctls on
     `/dev/nvhost-nvdec`) and the Host1x NVDEC class, sometimes with
     `/dev/nvhost-vic` for colour conversion. The game may also decode in
     software (for example libvpx inside the game).
   - Find out which, from logs (`[ipc]`/`nvdrv` lines in a run through the
     opening video) and from the nvdrv code
     (`core/hle/services/nvdrv/nvdrv.c`).
   - The video starts after New Game. The recipe in BOTS.md reaches it:
     the inputs at 3.0M–3.2M skip it, so stop before them, around 2.9M
     slices.
2. **Implement the decode path.**
   - If it's NVDEC: parse the codec setup and bitstream buffers the game
     submits (H.264 or VP9; find out which). Decode, and write the frames
     where the game expects them: NV12/YUV surfaces the game, or VIC,
     reads back.
   - **Web:** decode with the browser's **WebCodecs `VideoDecoder`**
     (hardware accelerated, no third-party code). Native: use a small,
     self-written path, or leave native decode stubbed with a clear log.
     Do **not** vendor ffmpeg/libvpx: that's third-party code and binaries.
   - Bitstream data and decoded frames must travel through linear memory,
     not per-frame postMessage (CLAUDE.md rule 6). A ring in linear memory,
     consumed by a worker that runs the VideoDecoder, is the pattern. See
     how the GPU stream does it: `core/gpu/gpu_stream.h`,
     `platform/web/workers/gpu.worker.ts`.
   - Video decoding is asynchronous. The guest waits on syncpoints/fences;
     signal them when the frame is ready, and never block the CPU worker
     (rule 3).
3. **Colour conversion (VIC),** if the game uses it to get RGBA.

### Files

- **You own:** new files under `core/hle/services/nvdrv/` for nvdec/vic
  (for example `nvdec.{h,c}`, `vic.{h,c}`), `core/video/**` (new), new web
  worker or bindings files for video, and tests for them.
- **You may touch, with care:** `core/hle/services/nvdrv/nvdrv.{h,c}`
  (registering devices), `core/CMakeLists.txt`, the layout (only if you
  need a new linear-memory region: `core/common/layout.{h,c}`, and update
  DESIGN §4), and `platform/web/src/main.ts` (starting a worker).
- **Do not touch:** `core/gpu/raster3d.c`, `core/gpu/wgsl.c`,
  `platform/web/workers/gpu*.ts`, the CPU backends, the scheduler.

## Milestones

1. **Findings.** Write down how the game decodes: devices, ioctls, codec,
   resolution, and where frames go.
2. **First frame natively.** Decode one frame (or prove the bitstream is
   intact: dump it to a file and identify it).
3. **In the browser.** The opening cinematic plays (screenshot it with the
   homebrew spec).
