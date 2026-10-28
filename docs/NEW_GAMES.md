# Bringing up a new game

Voland was built up on Hollow Knight: Silksong and has since been brought
up on more titles. Where they stand (October 2026, browser, Chromium):

| Game | State |
|---|---|
| Hollow Knight: Silksong | boot to gameplay at real speed, saves, video cutscenes |
| Hollow Knight | title, menus, saved games and new games (through the intro movie into King's Pass) play at real speed. The new-game intro used to stall after 13 pictures: an all-zero unused voice entry stopped the movie audio's voice 0, so its buffers never drained (fixed 2026-10-22) |
| Celeste | title, file select and gameplay; ~100 fps unpaced |
| Super Smash Bros. Ultimate | matches with correct colours and saves; fights 22-27 fps on three cores (the default since 2026-10-25; 12-21 serial, random stages, pacing off); the VP9 opening movie plays in the browser (2026-10-23) |
| Mario Kart 8 Deluxe (32-bit) | whole races, ~21-23 fps on three cores |
| Nine Sols | title, file select, the VP8 opening cinematic, gameplay and dialogue, ~22 fps. Speech bubbles render since 2026-10-24: the atlas (ASTC 12x12, 512x512) is read straight from the asset file in GOB blocks of 8 although its header says 16 - level 0's block height shrinks to the image like every level's |
| Animal Crossing: New Horizons | boots, presents and draws its intro scene, but never leaves its loading screen: the black is the game's own loading-screen blanking (with those clears skipped, its loading icon shows), while the loader thread is idle and no files are read. Needs the NG word list 2 system file (0100000000000823). Ruled out so far: controller applet result, focus messages, clock snapshot and steady clock, preselected user, nfp (also with one NFC device), BCAT, GPU syncpoint waits (none ever miss), unimplemented commands, the never-started Worker0 thread (by design) |
| The Legend of Zelda: Tears of the Kingdom | plays its opening in the browser: title cards, then the descent with Zelda's subtitles (WebGPU, serial scheduler, ~4-10 fps). Its pixel programs read storage buffers (LDG): since 2026-10-23 those translate for the GPU (address from an NVN descriptor by IADD or MOV, or a register's single definition), so the torch-lit opening draws instead of skipping ~60% of its GPU draws; 2 programs whose address differs per branch remain. It runs on three cores by default since 2026-10-25: its multicore loader corruption came from the scheduler letting a lower-priority thread run on a core while a better one there could (see "Parallel cores" below). The JIT keeps at most 24576 regions installed per core (each costs ~35 KB of engine memory; the tab ran out of memory at ~200k). Since 2026-10-26 compressed textures that are not whole blocks upload compressed too (TOTK GPU textures >1 GB -> ~400 MB), so its tab plus Chrome GPU process went from ~9.5 GB to ~8.6 GB (tab ~5.7 GB, GPU process ~2.9 GB, of which ~0.5 GB are its own textures and targets); Chrome had hung on a 16 GB Mac. Speed (2026-10-24): ~2.6 fps in the gloom scene, bound by host CPU - two thirds the guest's own code (JIT, one core), a third waiting for the GPU thread (96% of its draws shade vertices on the CPU: small draws over 64 KB constant buffers, and untraced global memory); not shading, draw-call commands, compile churn or the CPU clock. The multicore fault recurs in its two "CreateAndDeleteThread" workers (an object with an entry count but a null entry array, ~115-155k slices natively); the exclusive-monitor ABA window it pointed at is closed but was not the cause. BCn/ASTC textures upload compressed on GPUs that sample them, which removed its "larger than the texture cache" misses. Where its time goes on three cores (2026-10-25, all-worker profile): the guest cores are busy only ~30% each, mostly waiting on the GPU command thread, which is the busiest (~48%). A quarter of that is compute shaders run on the CPU: of the 8 compute programs kept there, 6 sample textures and 2 use global atomics. Since 2026-10-25 GPU compute binds textures: the 2 that read a 320x180 2D buffer run on the GPU; the 4 particle programs sampling a 32^3 3D noise volume stay on the CPU: they also read a buffer outside their storage descriptors, which reads zeros on the GPU (their embers turned purple). Moving them changed no fps in the scene measured (10.35 both ways) |

This page is the checklist for trying another one.

## 1. Run it and grab the report

1. Load the game's decrypted Program NCA. Use the game library, or "Load NCA…".
2. Let it run until it stops, crashes, or reaches the point you care about.
3. Click **Compatibility report** under the game, then **Copy** or **Save as file**.

The report holds every distinct warning or error the core logged, with
counts. It is grouped into crashes, missing services and commands,
failing calls, graphics, and audio. It also has renderer counters, game
speed, CPU backend, GPU and browser. Usually it says what is missing:

| In the report | Meaning |
|---|---|
| `[sm] GetServiceHandle("x"): no HLE service registered` | a whole service is missing |
| `[ipc] iface: unimplemented command N` | the service exists; command N does not |
| `[ipc] iface:Cmd failed: 0x...` | a call returned an error. Some of these are normal (file-exists checks return 0x202). |
| `svcBreak reason=0x80000007`, then `reason=0x0` | the game's own code threw an exception and aborted |
| `untranslatedGpuDraws` > 0 | shaders the WGSL translator could not handle (draws skipped) |
| `textureMisses` > 0 | texture formats or sizes the decoder does not support |

Natively, `voland-cli run GAME.nca` prints the same lines. Two
environment variables help: `VOLAND_BACKTRACE=1` adds per-thread
backtraces, and `VOLAND_EXCEPTION_SCAN=1` adds IL2CPP exceptions (Unity
titles).

## 2. Known gaps

- **Parallel cores:** the open multicore bug was the pick letting a
  lower-priority thread run on a core while a better thread there could -
  something a Switch core never does, and a "polling" thread was treated
  as idle. With parallel cores the pick now follows the per-core rule
  (docs/PARALLEL.md "Pollers and priority"):
  - Mario Kart 8 Deluxe's black tiles at the start of a race are gone (two
    three-core browser races clean; every earlier one had them).
  - Tears of the Kingdom mostly stopped corrupting its resource data on
    three cores: 7 of 9 native runs clean, with 1 crash and 1 stall at
    boot, and 3 of 3 browser runs clean. It now runs on three cores by
    default: ~3x serial's progress per virtual second (1300 frames by 150 s
    virtual vs 400) at 3.3 fps. `?cores=0` goes back to serial.

- **Video:** NVDEC decodes H.264, VP8 and VP9 (through WebCodecs; VP9
  headers are rebuilt from the picture setup and NVDEC's probability
  tables). The native CLI has no VP9 decoder: VOLAND_VIDEO_PATTERN=1
  stands in a test picture there.
- **Shader instructions not decoded:** LEA.HI, surface loads (SULD); the warning lists every distinct opcode a program
  lacks. Global atomics in graphics stages are dropped on the GPU (compute
  runs them on the CPU).
- **Updates and DLC are not loaded.** Only the base program NCA runs.
  `OpenPatchDataStorageByCurrentProcess` serves the base RomFS.
- **System archives are not shipped** (§1.6). A game that mounts one
  (MiiModel, the NG word lists) needs the user's own console copy under
  "System files".
- **Offline services answer "signed out"** (`prepo`, `friend`, `bcat`,
  `caps`, `nfp`, `pctl`, `nifm`). Online features stay empty by design
  (§1.6, §20).

## 3. Iterating

- Save a **save state** just before the problem: the whole machine, in
  one click, from the player bar. Fix, rebuild, reload, then load the state.
  A state loads when the build and memory layout match. A fresh page that
  boots the same game usually does.
- Natively, `--snapshot-at SLICE --snapshot-dir DIR` keeps a forked
  snapshot to run jobs from. `--savestate-check N:M` proves a state
  restores bit-exactly.

## 4. Native debugging aids

All are environment variables of `voland-cli` (diagnostics only):

- `VOLAND_SVC_TRACE=FROM:TO` logs every SVC in that virtual-tick window:
  thread, number, arguments, results. Services also log each event handle
  they hand out (`[ipc] event handle 0x...`), so a handle a thread waits on
  can be traced back to the command that created it.
- `VOLAND_PEEK=ADDR[:BYTES][,...]` dumps guest memory at exit (a resolved
  GOT entry, an object a stalled thread polls).
- `VOLAND_TRACE_CALLS=module+0xoff,...` logs calls to those addresses.
- `VOLAND_TRACE_DRAWS=START:LENGTH` logs every draw in a slice window with
  its target, blend state, textures and the pixel program's constants.
- `VOLAND_DUMP_SURFACES=DIR:F1,F2,...` writes the software renderer's
  surfaces at those frames; `--dump-shaders DIR` the decoded programs.
- `VOLAND_COMPUTE_MEMO_VERIFY=1` runs every memoized compute dispatch
  anyway and reports any difference.
- With modules dumped (`VOLAND_DUMP_MODULES=DIR`), a PLT stub's import is
  named by reading its GOT slot's relocation (the module's MOD0 dynamic
  section): titles without symbols still call the SDK by name.
