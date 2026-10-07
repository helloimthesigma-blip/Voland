# Bringing up a new game

Voland has been optimized and tested on Hollow Knight: Silksong, and
Hollow Knight saves and plays too. Most other titles are untested. This
page is the checklist for trying one, MK8DX or SSBU for example.

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

## 2. Known gaps that affect big 3D games

- **Vertex shaders run on the CPU.** Pixel programs are translated to
  WGSL; vertex work is done by the reference interpreter. Games with heavy
  geometry (MK8DX, SSBU) will be slow until vertex programs move to the GPU.
- **Compute dispatch is not implemented.** Only the compute engine's
  inline-to-memory methods work. Titles that use compute shaders will
  miss those results.
- **Updates and DLC are not loaded.** Only the base program NCA runs.
  `OpenPatchDataStorageByCurrentProcess` serves the base RomFS.
- **Runtime modules (`nn::ro`) are mapped, not verified.** NRR registration
  is accepted without checks. SSBU loads its fighters this way. The mapping
  is tested; real titles are not yet.
- **32-bit (AArch32) games do not run.** Their address space is laid out,
  but Voland has no 32-bit ARM CPU, so loading one says so plainly.
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
