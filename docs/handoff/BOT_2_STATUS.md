# Bot 2 status: video playback (branch `local/bot2`)

Owner: bot 2. The coordinator reads this when merging.

## Milestones

- [x] **1. Findings.** See below.
- [x] **2. First frame natively.** The H.264 stream rebuilt from the
  NVDEC submissions (synthesized SPS/PPS + the guest's slices) decodes
  bit-identically to the game's `Intro_Cinematic.mp4`: 199/199 frames
  have the same md5 under ffmpeg. The MP4 was extracted with
  `voland-cli romfs`.
- [x] **3. The opening cinematic plays in the browser.**
  `e2e/cinematic.spec.ts` passes on chromium-gpu (JIT core, WebGPU
  renderer). It presses A through title, profile and New Game, and the
  video stream starts 358 s after load. Over 120 s the screen fades in
  from black (mean luma 1 to 65) and keeps changing, and the frames
  match `Intro_Cinematic.mp4` (cave light, fern, the figures on the
  path).

## Findings (milestone 1)

- **The videos are H.264 in MP4.** Silksong's RomFS carries about 31 MP4s
  (`ct_03_Intro.mp4`, `Bellbeast_Travel_Children.mp4`, ...), encoded with
  x264. The opening one's SPS: Main profile, 1280x720 (80x45 MBs), CABAC,
  9 reference frames, `max_num_reorder_frames = 2`.
- **They are decoded on NVDEC**, through `/dev/nvhost-nvdec` channel
  SUBMITs: one command buffer per picture, with host1x METHOD0/METHOD1
  pairs for class 0xF0.
  - Buffers are named by the IOVAs MAP_BUFFER returned, >> 8.
  - Relocations are present but redundant: the commands already hold the
    IOVAs.
- **NVDEC registers** used per picture:

  | Method | Meaning |
  |---|---|
  | 0x200 | application id (3 = H.264) |
  | 0x400 | control params (0x53) |
  | 0x404 | picture setup (`nvdec_h264_pic_s`) |
  | 0x408 | bitstream: Annex-B slices only, **no SPS/PPS** (`00 00 01 65 ...`) |
  | 0x40C | picture index |
  | 0x410 | slice table `{u32 offset, u32 size}` |
  | 0x414 | co-located MVs |
  | 0x418 | history |
  | 0x420 | histogram |
  | 0x424 | status |
  | 0x428 / 0x42C | output luma / chroma |
  | 0x430.. / 0x470.. | reference surfaces |

  The output surfaces are 983040 + 491520 bytes: 1280x720 NV12,
  block-linear with 16-GOB blocks.
- **The game then runs VIC** (class 0x5D) on each decoded picture.

  | Method | Meaning |
  |---|---|
  | 0x400 / 0x404 | input luma / chroma (the NVDEC output surface) |
  | 0x704 | control params |
  | 0x708 | config struct |
  | 0x70C | filter struct |
  | 0x720 / 0x724 | output luma / chroma |

  - The config struct's output-surface word at +0x20 is **format 0x44
    (NV12), pitch-linear**. Sizes at +0x24/+0x28/+0x2C (14-bit `value - 1`
    pairs) are 1280x720 luma and 640x360 chroma.
  - VIC only de-tiles: the game converts YUV to RGB in its own shader.
- **The decoded frames never need to go back into NVDEC's surfaces.** VIC
  is the only reader, so the core's VIC writes the decoded frame straight
  into VIC's output surface.
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

- Browser, 2026-10-03: the video started 358 s after load. WebCodecs
  decodes at 1280x720 NV12 and the cinematic is visible and moving.
- One early run crashed the guest 11 s after load (`pc=0x2de7bb7640`,
  well before any video). It did not reproduce in 2 reruns; it looks
  like nondeterminism in the JIT or parallel guest threads, not video.

- Native snapshot job, opening video: 199 decoded frames, 199
  bit-identical to the source MP4.
- Browser worker test (`e2e/video.spec.ts`, Chromium WebCodecs, a
  self-generated High-profile clip with a synthesized SPS): 28/30 frames
  come out. The last 2 are held back by the declared reorder depth;
  that's intended.

## How to check

- Native, offline: run a snapshot job with `VOLAND_DUMP_VIDEO=out.264`
  (Annex-B as sent to the decoder) and decode it with ffmpeg. Snapshot
  jobs can't use VideoToolbox (it isn't fork-safe), so they only dump.
  - A non-forked run decodes with VideoToolbox, and
    `VOLAND_DUMP_VIDEO_FRAMES=prefix[:N]` writes PPMs.
- `voland-cli romfs <nca> [substring [outdir]]` lists or extracts RomFS
  files.
- Browser:
  `npx playwright test -c playwright.video.config.ts --project=chromium`
  runs the worker test. Set `VOLAND_CINEMATIC_NCA=<nca>` and use
  `--project=chromium-gpu` to play the game to its opening cinematic.

## H.264 details worth knowing

- `nvdec_h264_pic_s` offsets are confirmed against the real stream
  (`core/hle/services/nvdrv/nvdec.c`, `H264_*`). WeightScale lists are
  in raster order.
- **`max_num_ref_frames` isn't in the struct and must be exact.** A wider
  window keeps stale references that shift B-slice lists; a 64x64 x264
  test diverged at frame 20.
  - It starts as `num_ref_idx_l0_default + 1` (x264's choice; 6 for
    Silksong) and widens to the guest DPB's reference count at the next
    IDR.
- The VUI declares `max_num_reorder_frames = 2`. Zero makes ffmpeg drop
  frames; a large value delays output.

## Gameplay performance analysis (2026-10-03)

- Browser CPU worker in gameplay: 84% JIT-compiled guest code, ~16% everything else. Native virtual time: 70.6% executed, 29.4% idle jumps. The game is at 60 fps in virtual time, so browser fps = host execution speed.
- Fixed: the HID npad LIFO sequence was odd half the time and the SDK reader spun on it (9% of guest cycles).
- Found for others: scalar FMIN/FMAX interpreter fallback (fixed by the JIT agent); Unity job-system workers yield-spinning (bot 1's scheduler rule); instruction mix (34% memory accesses) for the JIT.
- Tools: `tools/profiling/README.md`.

## Playability pass (2026-10-03)

- **No controller at boot → controller-applet loop.** The game launched applet 0xC 974 times before the first key press, restarting audout each time (silent title, wasted work). The web keyboard is now player 1 from page load. The CLI is unchanged, for determinism.
  - Worker time to the title (860k slices) went 149.5 s → 173.6 s, because the game's audio engine now really runs (before, it never got past the applet loop). Title fps is unchanged (9.9 → 9.8).
- **Audio below real time** is now time-stretched in the worklet (`src/audio/slow-stretch.ts`): Hann-grain overlap-add, pitch kept, fades out when the core stops producing.
  - Offline on real game audio at 22% speed: silence 83.8% → 27.1% (the source is 27% silent); large sample jumps 1.7/s → 0.7/s.
  - Full speed takes the old path (no added latency); a core producing nothing stays on the old path.
- **Short taps** are held for at least 2 guest frames (40–300 ms); presses are immediate.
- The underrun log is rate-limited to once a minute; plain-language copy, a boot hint and a keyboard hint in the legend.
- **Latency**, keydown → title-menu cursor visibly moving: 0.78–1.7 s (median ~0.9 s ≈ 10 guest frames at 11–13 fps). Being split into game-internal and pipeline parts natively.

## Save persistence (2026-10-04)

- **Before:** the CPU worker mirrored the live save trees file by file every 3 s, whatever the game was doing. A snapshot could catch a save half-written, and a multi-file save could be stored half old, half new. There was no backup.
- **Now:** a save persists when the game commits it (`IFileSystem::Commit`, when a Switch makes save data durable), as a whole.
  - The core snapshots the save into one archive (`core/hle/fs/save_archive.{h,c}`; temp file then swap, so a full pool keeps the previous commit).
  - The worker stores it as one OPFS file per save, `saves-v2/SS-<attr>.vsave`, within 250 ms. `createWritable` swaps it in on close, so a tab closed mid-write keeps the previous commit.
  - Uncommitted writes don't persist, as on hardware. The old per-file layout is still restored; the next commit supersedes it.
- **Backup/restore:** the "Back up saves" / "Restore saves…" buttons export or import a plain tar (`voland-saves/…`, readable with `tar`). Imports are validated by the core before they are stored.
- **Verified:**
  - Silksong across a full browser restart (persistent Chromium profile): session 1 played the gameplay recipe; session 2 logged "restored 1 game save(s)". The archive is byte-identical to the native commit (52.6 KB: `shared.dat` + `Restore_Points1/NODELrestoreData1.dat`).
  - Silksong only writes slot progress at its first save point (a bench), which the recipe doesn't reach, so the profile screen correctly still says New Game.
  - Tests: `tests/save_archive_test.c`, `tests/unit/save-store.test.ts`.
- `voland-cli`: `VOLAND_DUMP_SAVES=DIR` writes the committed archives at exit.
