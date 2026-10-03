# Bot 2 status: video playback (branch `local/bot2`)

Owner: bot 2. The coordinator reads this when merging.

## Milestones

- [ ] **1. Findings.** In progress (see below).
- [ ] 2. First frame natively.
- [ ] 3. The opening cinematic plays in the browser.

## Findings so far

- **The videos are H.264 in MP4.** Silksong's RomFS carries about 31 MP4s
  (`ct_03_Intro.mp4`, `Bellbeast_Travel_Children.mp4`, ...). They were
  encoded with x264, in Main (77) and High (100) profiles at levels
  3.1–4.2. The RomFS is compressed in the NCA, so the files can't be
  carved out raw.
- **The decode path** (nvhost-nvdec ioctls, host1x command buffers, the
  picture-setup struct): being captured from a native run through the
  opening video. Details land here.

## What landed

- `core/video/host1x.{h,c}`: a host1x command-buffer parser (SETCLASS,
  INCR, NONINCR, MASK, IMM). It latches METHOD0/METHOD1 pairs into
  engine methods. Tested in `tests/host1x_test.c`.
- `core/hle/services/nvdrv/nvdec.{h,c}`: NVDEC and VIC register files,
  fed by SUBMIT's command buffers, plus an IOVA table.
  - **MAP_BUFFER on host1x channels now returns synthetic 32-bit IOVAs**,
    one per nvmap handle (1 MiB aligned). It used to return the guest VA
    truncated to 32 bits. Engine methods carry `IOVA >> 8`.
  - The nvhost-gpu/as-gpu paths are untouched. `nvdrv_test` was updated
    for this.
- `core/video/h264.{h,c}`: synthesizes the H.264 SPS and PPS from the
  picture-setup fields (High profile, level 5.1, with a VUI
  bitstream_restriction block).
  - Verified with an x264 High-profile stream (B-pyramid, weighted
    prediction, 8x8 transform): its own SPS/PPS were replaced by
    synthesized ones, and ffmpeg decodes all 60 frames md5-identically.
- `core/video/video_stream.{h,c}` plus a new **layout region**
  (`video_region_base`, the 11th `Memory_Layout` field, appended; 24 MiB):
  - a decode-request ring in the GPU stream's format (CONFIGURE, DECODE
    records);
  - six NV12 frame slots, handed between the core and the decoder with a
    FREE → WRITING → READY state word.
- `platform/web/workers/video.worker.ts` + `platform/web/bindings/video.ts`:
  - the WebCodecs `VideoDecoder` reads the ring;
  - decoded frames are packed to NV12 into the slots;
  - `main.ts` starts the worker on the layout handshake;
  - unit tests are in `tests/unit/video.test.ts`.

## Deviations from DESIGN §13 (to fold into DESIGN when this merges)

- **Decoding runs in a dedicated video worker, not in the GPU worker.**
  BOT_2.md keeps `gpu.worker.ts` off-limits to this lane, and a separate
  worker keeps the decoder's async callbacks off the renderer's queue.
- **Decoded frames are not written back to the guest's NVDEC surfaces.**
  The core's VIC reads the decoded frame for its input surface straight
  from the video region's slots and writes the converted output to guest
  memory. Games only consume NVDEC output through VIC, so the NV12
  block-linear write-back is skipped. If a title samples NVDEC output
  directly, it would come back.
- **Syncpoints still complete at once.** VIC uses the newest decoded frame
  when the exact one isn't ready yet, so a slow decoder shows a frame late
  instead of blocking the guest.

## Needs from others

- **Coordinator (renderer):** the VIC output lands in guest memory through
  `vmm_write_block`, and the game samples it as a texture. If the texture
  cache stops re-hashing every frame, it needs to notice CPU-side writes
  to texture memory, or VIC output will look frozen.

## Measurements

None yet.
