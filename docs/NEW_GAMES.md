# Bringing up a new game

Voland was built up on Hollow Knight: Silksong and has since been brought
up on more titles. Where they stand (October 2026, browser, Chromium):

| Game | State |
|---|---|
| Hollow Knight, Hollow Knight: Silksong | boot to gameplay at real speed, saves, video cutscenes |
| Celeste | title, file select and gameplay; ~100 fps unpaced |
| Super Smash Bros. Ultimate | matches with correct colours and saves, 18-22 fps in a fight; its VP9 movies are black |
| Mario Kart 8 Deluxe (32-bit) | whole races, ~23 fps on three cores; occasional black tiles in multicore runs |
| Nine Sols | title, file select and gameplay, ~22 fps; dialogue text shows magenta ASTC blocks (texture data zero past ~10 KB), the VP8 intro movie is black |
| Animal Crossing: New Horizons | boots and presents, but stays behind its own black UI curtain; needs the NG word list 2 system file (0100000000000823) |
| The Legend of Zelda: Tears of the Kingdom | boots (CLI to 1.5M slices) but presents black; uses VMNMX, which is not decoded |

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

- **Video:** NVDEC decodes H.264 only. VP8 (Nine Sols' intro) and VP9
  (SSBU's movies) play black.
- **Shader instructions not decoded:** VMNMX (video min/max), LEA.HI,
  surface loads (SULD); the warning lists every distinct opcode a program
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
